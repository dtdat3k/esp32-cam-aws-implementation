# ESP32-CAM Face Recognition with AWS Free Tier

This project uses an ESP32-CAM (AI-Thinker) to capture photos, stream a live feed, and send images to AWS for face recognition using Rekognition. Notifications are sent via Telegram. The setup leverages AWS Free Tier services for cost efficiency.

## Prerequisites

### Hardware
- **ESP32-CAM (AI-Thinker)** with OV2640 camera
- **FTDI Programmer** (for uploading code)
- **Jumper Wires**
- **Computer** with USB port

### Software
- **Arduino IDE** (with ESP32 board support)
- **AWS Account** (Free Tier eligible)
- **Telegram Account** (for notifications)
- **Python 3.x** (for AWS Lambda dependencies)

## Step-by-Step Setup

### 1. Hardware Setup
1. **Connect ESP32-CAM to FTDI Programmer:**
   - VCC → 5V
   - GND → GND
   - U0R (RX) → TX
   - U0T (TX) → RX
   - GPIO 0 → GND (for programming mode)
2. **Power the ESP32-CAM:**
   - Connect 5V and GND from FTDI to ESP32-CAM.
3. **Verify Connections:**
   - Ensure no loose connections before powering on.

### 2. Configure Arduino IDE
1. **Install ESP32 Board Support:**
   - In Arduino IDE, go to `File > Preferences`.
   - Add `https://raw.githubusercontent.com/espressif/arduino-esp32/master/package_esp32_index.json` to Additional Boards Manager URLs.
   - Go to `Tools > Board > Boards Manager`, search for "ESP32", and install `esp32` by Espressif.
2. **Select Board:**
   - `Tools > Board > ESP32 Arduino > AI-Thinker ESP32-CAM`.
3. **Install Libraries:**
   - `PubSubClient` (via Library Manager).
   - `WiFiClientSecure` (included with ESP32 core).

### 3. AWS Setup (Free Tier)
1. **Create IAM Role for Lambda:**
   - In AWS Console, go to IAM > Roles > Create Role.
   - Select "AWS Lambda", attach policies: `AWSLambdaBasicExecutionRole`, `AmazonRekognitionFullAccess`, `AmazonS3FullAccess`, `AmazonDynamoDBFullAccess`.
   - Name it `lambda-esp32-role`.
2. **Set Up DynamoDB Tables:**
   - Go to DynamoDB > Create Table:
     - `MQTTPhotoParts`: Partition Key `id` (String), Sort Key `seq` (Number).
     - `NotifiedFaces`: Partition Key `FaceId` (String).
     - `IntruderAlertStatus`: Partition Key `id` (String).
   - Use default settings (Free Tier eligible).
3. **Create S3 Bucket:**
   - Go to S3 > Create Bucket.
   - Name: `esp32cam-photos-dat3k` (unique).
   - Region: e.g., `ap-southeast-1`.
   - Keep defaults (Free Tier).
4. **Set Up Rekognition Collection:**
   - Go to Rekognition > Collections > Create Collection.
   - Name: `dtvtProd`.
5. **Create IoT Core Thing:**
   - Go to IoT Core > Manage > Things > Create Thing.
   - Name: `ESP32CAM`.
   - Download certificates (CA, Client Cert, Private Key) and save them.
   - Create a policy allowing `iot:Connect`, `iot:Publish` on `esp32/photo`.
6. **Create Lambda Function:**
   - Go to Lambda > Create Function.
   - Name: `esp32cam-face-recognition`.
   - Runtime: Python 3.9.
   - Attach `lambda-esp32-role`.
   - Copy the first code block (Python) into the editor.
   - Add Layers:
     - `telegram` (upload `python-telegram-bot` as a ZIP).
     - `boto3` (AWS SDK, included by default).
   - Set Environment Variables:
     - `TELEGRAM_BOT_TOKEN`: `<your-bot-token>`.
     - `TELEGRAM_CHAT_ID`: `<your-chat-id>`.
   - Deploy the function.
7. **Set Up IoT Rule:**
   - Go to IoT Core > Act > Rules > Create Rule.
   - Name: `photo_to_lambda`.
   - Query: `SELECT * FROM 'esp32/photo'`.
   - Action: Send to Lambda (`esp32cam-face-recognition`).

### 4. Telegram Bot Setup
1. **Create Bot:**
   - Open Telegram, message `@BotFather`, use `/newbot`.
   - Follow prompts to get `TELEGRAM_BOT_TOKEN`.
2. **Get Chat ID:**
   - Message your bot, then use `https://api.telegram.org/bot<your-bot-token>/getUpdates` to find `chat_id`.

### 5. Program ESP32-CAM
1. **Update Code:**
   - Copy the second code block (C++) into Arduino IDE.
   - Replace placeholders:
     - `ssid`: Your Wi-Fi SSID.
     - `password`: Your Wi-Fi password.
     - `ca_cert`, `client_cert`, `private_key`: From IoT Core (format as C strings).
   - Set `LIVE_FEED_URL` in Lambda to your ESP32’s IP (e.g., `http://<ESP32-IP>/`).
2. **Upload Code:**
   - Select port (`Tools > Port`).
   - Upload with GPIO 0 grounded, then remove jumper and reset.
3. **Test:**
   - Open Serial Monitor (115200 baud).
   - Check for "WiFi connected" and IP address.

### 6. Test the System
1. **Live Feed:**
   - Open `http://<ESP32-IP>/` in a browser to view the stream.
2. **Photo Capture:**
   - ESP32 sends photos every 0.5s via MQTT.
   - Lambda processes them and sends Telegram alerts.
3. **Face Recognition:**
   - Add known faces to Rekognition collection via AWS Console.
   - Test with known/unknown faces in front of the camera.

## Configuration Details
- **ESP32-CAM:**
  - Frame Size: VGA (640x480).
  - JPEG Quality: 30.
  - Photo Interval: 0.5s.
- **AWS:**
  - Cooldown: 60s (familiar faces), 15s (intruders).
  - Rekognition Threshold: 85% similarity.
- **Telegram:**
  - Alerts include photo and live feed link.

## Troubleshooting
- **Camera Init Fails:** Check wiring, ensure PSRAM is enabled.
- **MQTT Fails:** Verify certificates and Wi-Fi.
- **Lambda Errors:** Check CloudWatch Logs.
- **No Alerts:** Confirm Telegram token/chat ID.

## Notes
- AWS Free Tier limits: 1M DynamoDB requests, 500K Lambda invocations, 20K Rekognition calls/month.
- Secure certificates and tokens; avoid committing them to Git.
