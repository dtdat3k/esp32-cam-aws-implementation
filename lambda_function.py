import json
import boto3
import base64
import io
import telegram
from telegram import InlineKeyboardMarkup, InlineKeyboardButton 
from boto3.dynamodb.conditions import Key
from datetime import datetime, timedelta
import time
from botocore.exceptions import ClientError

# Configure your Telegram bot
TELEGRAM_BOT_TOKEN = '<your-bot-token>'
TELEGRAM_CHAT_ID = '<your-chat-id>'
# How to: https://gist.github.com/nafiesl/4ad622f344cd1dc3bb1ecbe468ff9f8a
bot = telegram.Bot(token=TELEGRAM_BOT_TOKEN)

# Initialize AWS services
dynamodb = boto3.resource('dynamodb')
parts_table = dynamodb.Table('MQTTPhotoParts')
notified_table = dynamodb.Table('NotifiedFaces')
intruder_alert_table = dynamodb.Table('IntruderAlertStatus') 
rekognition_client = boto3.client('rekognition')
s3_client = boto3.client('s3')
BUCKET_NAME = 'esp32cam-photos-dat3k'
COLLECTION_ID = 'dtvtProd'

COOLDOWN_SECONDS = 60         # For familiar faces
INTRUDER_COOLDOWN_SECONDS = 15  # For intruder alerts

# URL for the ESP32 live feed (replace with your actual URL)
LIVE_FEED_URL = 'http://10.124.5.138/'  # Update this with your ESP32 live feed URL

def lambda_handler(event, context):
    """
    Expected MQTT payload (JSON) for each message:
    {
      "id": "unique_photo_id",
      "seq": <1|2|3>,
      "total": 3,
      "data": "<base64 string for this part>"
    }
    """
    try:
        # Parse payload
        if isinstance(event, dict) and 'payload' in event:
            payload = json.loads(event['payload'])
        elif isinstance(event, str):
            payload = json.loads(event)
        elif isinstance(event, dict):
            payload = event
        else:
            return {'statusCode': 400, 'body': 'Unrecognized event format'}

        photo_id = payload.get('id')
        seq = payload.get('seq')
        total = payload.get('total')
        part_data = payload.get('data')
        if not (photo_id and seq and total and part_data):
            return {'statusCode': 400, 'body': 'Missing required fields'}

        # Store the part in DynamoDB
        parts_table.put_item(
            Item={
                'id': photo_id,
                'seq': int(seq),
                'data': part_data,
                'total': int(total)
            }
        )
        print(f"Stored part {seq} for photo id {photo_id}")

        # Check if all parts are received
        response = parts_table.query(KeyConditionExpression=Key('id').eq(photo_id))
        parts = response.get('Items', [])
        if len(parts) < int(total):
            return {'statusCode': 200, 'body': f"Stored part {seq}. Waiting for remaining parts ({len(parts)}/{total})."}

        # Reassemble image and clean up parts
        parts_sorted = sorted(parts, key=lambda item: int(item['seq']))
        full_base64 = "".join(item['data'] for item in parts_sorted)
        print(f"Reassembled photo id {photo_id}, total length: {len(full_base64)}")
        for item in parts_sorted:
            parts_table.delete_item(Key={'id': photo_id, 'seq': int(item['seq'])})

        # Decode image
        image_data = base64.b64decode(full_base64)

        # Use Rekognition to search for matching faces
        search_response = rekognition_client.search_faces_by_image(
            CollectionId=COLLECTION_ID,
            Image={'Bytes': image_data},
            MaxFaces=10,
            FaceMatchThreshold=85
        )
        face_matches = search_response.get('FaceMatches', [])
        print(f"Found {len(face_matches)} face match(es)")

        current_time = datetime.utcnow()
        current_epoch = int(time.time())

        # Create an inline button for the live feed
        live_feed_button = InlineKeyboardButton(text="Xem Live Feed", url=LIVE_FEED_URL)
        reply_markup = InlineKeyboardMarkup([[live_feed_button]])

        # If face matches exist, treat as familiar face detection
        if face_matches:
            # Build a dictionary mapping ExternalImageId -> best similarity
            face_dict = {}
            for match in face_matches:
                face = match.get('Face', {})
                external_id = face.get('ExternalImageId', 'Unknown')
                similarity = match.get('Similarity', 0)
                face_dict[external_id] = max(face_dict.get(external_id, 0), similarity)

            # Pick the best matching face
            best_external_id, best_similarity = max(face_dict.items(), key=lambda x: x[1])
            
            # Check cooldown for familiar face notification
            seen = notified_table.get_item(Key={'FaceId': best_external_id})
            notify_this = False
            last_seen = seen.get('Item', {}).get('last_seen')
            if not last_seen:
                notify_this = True
            else:
                try:
                    last_seen_time = datetime.fromisoformat(str(last_seen))
                    if (current_time - last_seen_time) > timedelta(seconds=COOLDOWN_SECONDS):
                        notify_this = True
                except Exception as ex:
                    print(f"Error parsing last_seen for {best_external_id}: {ex}")
                    notify_this = True

            if notify_this:
                # Update notified table with current timestamp
                notified_table.put_item(
                    Item={
                        'FaceId': best_external_id,
                        'last_seen': current_time.isoformat()
                    }
                )
                time_str = (current_time + timedelta(hours=7)).strftime("%H:%M:%S")
                message_text = (f"🚨 Cảnh Báo:\n\n"
                                f"{best_external_id} đang ở trước cửa nhà bạn.\n"
                                f"Thời Gian: {time_str}\n"
                                f"Xin chào {best_external_id}")
                print(message_text)
                photo_stream = io.BytesIO(image_data)
                photo_stream.name = f"{photo_id}.jpg"
                bot.send_photo(
                    chat_id=TELEGRAM_CHAT_ID,
                    photo=photo_stream,
                    caption=message_text,
                    reply_markup=reply_markup  # Add the inline button
                )
                print("Sent familiar face notification with photo and live feed button.")
            else:
                print("Familiar face detected but on cooldown; no notification sent.")
            
            return {'statusCode': 200, 'body': 'Familiar face processed.'}
        
        else:
            # If no face matches found, treat as intruder.
            try:
                # Attempt to update last_alert_time atomically with a condition
                intruder_alert_table.update_item(
                    Key={'id': 'global'},
                    UpdateExpression='SET last_alert_time = :new_time',
                    ConditionExpression='last_alert_time < :threshold OR attribute_not_exists(last_alert_time)',
                    ExpressionAttributeValues={
                        ':new_time': current_epoch,
                        ':threshold': current_epoch - INTRUDER_COOLDOWN_SECONDS
                    }
                )
                # If the update succeeds, send the alert
                time_str = (current_time + timedelta(hours=7)).strftime("%H:%M:%S")
                message_text = (f"🚨 Cảnh Báo:\n\n"
                                f"Có 1 người lạ đang ở trước cửa nhà bạn.\n"
                                f"Thời gian: {time_str}\n"
                                f"Vui lòng kiểm tra trên live feed")
                print(message_text)
                photo_stream = io.BytesIO(image_data)
                photo_stream.name = f"{photo_id}.jpg"
                bot.send_photo(
                    chat_id=TELEGRAM_CHAT_ID,
                    photo=photo_stream,
                    caption=message_text,
                    reply_markup=reply_markup  # Add the inline button
                )
                print("Sent intruder alert notification with photo and live feed button.")
            except ClientError as e:
                if e.response['Error']['Code'] == 'ConditionalCheckFailedException':
                    print("Intruder alert on cooldown; no notification sent.")
                else:
                    raise
            return {'statusCode': 200, 'body': 'Intruder alert processed.'}

    except Exception as e:
        print(f"Error: {e}")
        return {'statusCode': 500, 'body': str(e)}