#include "esp_camera.h"
#include <WiFi.h>
#include <PubSubClient.h>
#define MQTT_MAX_PACKET_SIZE 32765  // 128KB buffer for HD
#include "soc/soc.h"           // For brownout detector
#include "soc/rtc_cntl_reg.h"  // For brownout detector
#include "esp_http_server.h"
#include <WiFiClientSecure.h>


// Wi-Fi credentials
const char* ssid = "<your-ssid>";
const char* password = "<your-password>";
const size_t CHUNK_SIZE = 10000;  // 10KB chunks

// AWS IoT settings
const char* mqtt_server = "a3c4zlpp6dij77-ats.iot.ap-southeast-1.amazonaws.com";
const int mqtt_port = 8883;
const char* mqtt_topic = "esp32/photo";

// Certificates (replace with your formatted certs)
const char* ca_cert = "<insert-ca-cert>";
const char* client_cert = "<insert-client-cert>";
const char* private_key = "<insert-priv-key>";

// Pin configuration for AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WiFiClientSecure espClient;
PubSubClient client(espClient);
httpd_handle_t server_handle = NULL;  // Handle for the web server

unsigned long lastPhotoTime = 0;
const long photoInterval = 500;  // 0.5 seconds

// HTML webpage for the live feed, fetching the stream from "/stream"
const char* html = "<html><head>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                  "<title>Live camera feed</title>"
                  "<style>"
                  "body {"
                  "  margin: 0;"
                  "  padding: 20px;"
                  "  font-family: 'Arial', sans-serif;"
                  "  background: linear-gradient(135deg, #f5f7fa 0%, #c3cfe2 100%);"
                  "  min-height: 100vh;"
                  "  display: flex;"
                  "  flex-direction: column;"
                  "  align-items: center;"
                  "}"
                  ".container {"
                  "  width: 100%;"
                  "  max-width: 1280px;"
                  "  background: white;"
                  "  border-radius: 15px;"
                  "  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);"
                  "  overflow: hidden;"
                  "}"
                  "header {"
                  "  background: #2c3e50;"
                  "  color: white;"
                  "  padding: 1rem;"
                  "  text-align: center;"
                  "}"
                  "h1 {"
                  "  margin: 0;"
                  "  font-size: 1.5rem;"
                  "}"
                  ".video-container {"
                  "  padding: 20px;"
                  "  background: #ecf0f1;"
                  "}"
                  "img {"
                  "  width: 100%;"
                  "  height: auto;"
                  "  border-radius: 10px;"
                  "  display: block;"
                  "  transform: scaleX(-1);"  // Mirror the image horizontally
                  "}"
                  "@media (max-width: 768px) {"
                  "  .container {"
                  "    margin: 0 10px;"
                  "  }"
                  "  h1 {"
                  "    font-size: 1.2rem;"
                  "  }"
                  "}"
                  "</style>"
                  "</head>"
                  "<body>"
                  "<div class='container'>"
                  "<header>"
                  "<h1>Live camera feed</h1>"
                  "</header>"
                  "<div class='video-container'>"
                  "<img src='/stream' alt='Live Feed'>"
                  "</div>"
                  "</div>"
                  "</body></html>";

// Handler for the root URL ("/") to serve the webpage
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html, strlen(html));
  return ESP_OK;
}

// Handler for the "/stream" URL to serve the MJPEG stream
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[64];

  static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
  static const char* _STREAM_BOUNDARY = "\r\n--123456789000000000000987654321\r\n";
  static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    }

    if (res != ESP_OK) {
      break;
    }
  }

  return res;
}

// Start the web server
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  // Handler for the root URL ("/") to serve the webpage
  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };

  // Handler for the "/stream" URL to serve the MJPEG stream
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&server_handle, &config) == ESP_OK) {
    httpd_register_uri_handler(server_handle, &index_uri);
    httpd_register_uri_handler(server_handle, &stream_uri);
    Serial.println("Web server started on port: 80");
  } else {
    Serial.println("Failed to start web server");
  }
}

void setup() {
  Serial.begin(115200);

  // Disable brownout detector
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  if (psramFound()) {
    Serial.println("PSRAM found and enabled");
    psramInit();
  } else {
    Serial.println("PSRAM not found!");
    ESP.restart();
  }

  // Camera configuration
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;  // 640x480 for better streaming performance
  config.jpeg_quality = 30;          
  config.fb_location = CAMERA_FB_IN_PSRAM;  
  
  // Initialize camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("ESP32-CAM IP Address: ");
  Serial.println(WiFi.localIP());

  // Start the web server
  startCameraServer();

  // Set up AWS IoT (certificates omitted for brevity)
  espClient.setCACert(ca_cert);  // Add your CA certificate here
  espClient.setCertificate(client_cert);  // Add your client certificate here
  espClient.setPrivateKey(private_key);  // Add your private key here
  client.setServer(mqtt_server, mqtt_port);
  
  // change heap size to 32765 to transfer image 
  if (!client.setBufferSize(32765)) {
    Serial.println("Buffer size resize failed!");
  } else {
    Serial.println("Buffer size set to 32kb.");
  }

  reconnect();
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to AWS IoT...");
    if (client.connect("ESP32CAM")) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      delay(5000);
    }
  }
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long now = millis();
  if (now - lastPhotoTime >= photoInterval) {
    takeAndSendPhoto();
    lastPhotoTime = now;
  }
}

void takeAndSendPhoto() {
  // Capture photo from camera
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  // Encode JPEG image to base64
  String base64Str = base64_encode(fb->buf, fb->len);
  size_t fullLen = base64Str.length();
  size_t partSize = fullLen / 3; // approximate size of each part

  // Generate a unique ID for this photo
  String uniqueId = String(millis());

  // Send 3 separate MQTT messages
  for (int i = 0; i < 3; i++) {
    int startIndex = i * partSize;
    int endIndex = (i < 2) ? (startIndex + partSize) : fullLen;
    String partData = base64Str.substring(startIndex, endIndex);

    String jsonPayload = "{\"id\":\"" + uniqueId + "\",\"seq\":" + String(i+1) + 
                         ",\"total\":3,\"data\":\"" + partData + "\"}";
    size_t payload_len = jsonPayload.length();

    if (client.beginPublish(mqtt_topic, payload_len, false)) {
      client.print(jsonPayload);
      if (client.endPublish()) {
        Serial.print("Part ");
        Serial.print(i + 1);
        Serial.println(" published successfully!");
      } else {
        Serial.print("Failed to finish publishing part ");
        Serial.println(i + 1);
      }
    } else {
      Serial.print("Failed to start publishing part ");
      Serial.println(i + 1);
    }
    
    delay(50);
  }

  esp_camera_fb_return(fb);
}

String base64_encode(uint8_t *data, size_t len) {
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String encoded = "";
  int i = 0, j = 0;
  uint8_t array_3[3], array_4[4];

  while (len--) {
    array_3[i++] = *(data++);
    if (i == 3) {
      array_4[0] = (array_3[0] & 0xfc) >> 2;
      array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
      array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);
      array_4[3] = array_3[2] & 0x3f;

      for (j = 0; j < 4; j++) {
        encoded += base64_chars[array_4[j]];
      }
      i = 0;
    }
  }

  if (i) {
    for (j = i; j < 3; j++) array_3[j] = '\0';
    array_4[0] = (array_3[0] & 0xfc) >> 2;
    array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
    array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);
    array_4[3] = array_3[2] & 0x3f;

    for (j = 0; j < i + 1; j++) encoded += base64_chars[array_4[j]];
    while (i++ < 3) encoded += '=';
  }

  return encoded;
}