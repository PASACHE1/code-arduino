#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"
#include "FS.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "esp_http_server.h"

// Definiciones para la cámara AI-Thinker
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

// Definiciones para el sensor PIR y el LED de estado
#define PIR_PIN 13
#define LED_PIN 4

// Configuración WiFi
//const char* ssid = "PASACHE";
const char* ssid = "HONOR X7b";
//const char* password = "P@s@che2023#";
const char* password = "PASACHE123";
const char* serverUrl = "http://192.168.46.47:5000/upload"; // Cambia por la URL del servidor Flask
//const char* serverUrl = "http://192.168.1.25:5000/upload"; // Cambia por la URL del servidor Flask

// Variables para el streaming
httpd_handle_t stream_httpd = NULL;

// Conectar al WiFi
void conectarWiFi() {
  Serial.println("Conectando al WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado.");
  Serial.print("Dirección IP: ");
  Serial.println(WiFi.localIP());
}

// Configuración de la cámara
void configuracionCamara() {
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

  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error al inicializar la cámara: 0x%x\n", err);
    while (true) delay(1000);
  }
  Serial.println("Cámara inicializada correctamente.");
}

// Handler de streaming
esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len;
  uint8_t *_jpg_buf;
  char part_buf[64];

  res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Error al capturar el frame.");
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
      if (!jpeg_converted) {
        Serial.println("Error al convertir frame a JPEG.");
        esp_camera_fb_return(fb);
        res = ESP_FAIL;
        break;
      }
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    size_t hlen = snprintf(part_buf, 64, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", _jpg_buf_len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }

    esp_camera_fb_return(fb);
    if (res != ESP_OK) {
      break;
    }
  }
  return res;
}

// Iniciar servidor HTTP para streaming
void iniciarServidorStreaming() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;

  httpd_uri_t stream_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = stream_handler,
      .user_ctx = NULL};

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("Servidor de streaming iniciado.");
    Serial.printf("Accede al streaming en: http://%s:81/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Error al iniciar el servidor de streaming.");
  }
}

// Tarea para el sensor PIR
void tareaPIR(void *parameter) {
  while (true) {
    if (digitalRead(PIR_PIN) == HIGH) {
      Serial.println("Movimiento detectado. Capturando foto...");

      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("Error al capturar la foto.");
        continue;
      }

      if (WiFi.status() == WL_CONNECTED) {
        WiFiClient client;
        HTTPClient http;

        http.begin(client, serverUrl);
        http.addHeader("Content-Type", "image/jpeg");

        int httpResponseCode = http.POST(fb->buf, fb->len);
        if (httpResponseCode > 0) {
          Serial.printf("Foto enviada al servidor. Código: %d\n", httpResponseCode);
        } else {
          Serial.printf("Error al enviar la foto. Código: %d\n", httpResponseCode);
        }
        http.end();
      } else {
        Serial.println("WiFi no conectado. Foto no enviada.");
      }

      esp_camera_fb_return(fb);
      delay(10000); // Evitar múltiples capturas en poco tiempo
    }
    delay(100); // Reducir uso de CPU
  }
}

void setup() {
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  conectarWiFi();
  configuracionCamara();
  iniciarServidorStreaming();

  // Crear tarea para el sensor PIR
  xTaskCreate(tareaPIR, "TareaPIR", 4096, NULL, 1, NULL);
}

void loop() {
  delay(100); // Mantener el servidor en ejecución
}


