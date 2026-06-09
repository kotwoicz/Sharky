/*
 * ESP32-CAM + Teachable Machine  ->  ANIMOWANE OCZY (dwa wyswietlacze I2C)
 * ========================================================================
 * Polaczenie dwoch szkicow:
 * - kamera / serwer WWW / Teachable Machine          (z pierwszego pliku)
 * - silnik animacji oka kota na dwoch OLED-ach        (z drugiego pliku)
 *
 * KAZDA KLASA MODELU = JEDNA EMOCJA. Oba wyswietlacze (lewe + prawe oko)
 * pokazuja te sama emocje. Mapowanie klasa->emocja edytujesz w jednym
 * miejscu, w tabeli CLASS_TABLE ponizej.
 *
 * TRIK I2C (zamienione piny + SOFTWARE I2C)
 * -----------------------------------------
 * Oba OLED-y maja ten sam adres (0x3C) i wisza na GPIO15 + GPIO4, ale maja
 * ZAMIENIONE piny SDA/SCL. Odzywa sie tylko ten ekran, dla ktorego linie sa
 * podlaczone poprawnie; drugi widzi przekrecone SDA/SCL i nie odpowiada:
 * Lewe oko  (displayA):  SDA = GPIO15, SCL = GPIO4
 * Prawe oko (displayB):  SDA = GPIO4,  SCL = GPIO15
 *
 * WAZNE: uzywamy SOFTWARE I2C (bit-bang) z U8g2, a NIE sprzetowego Wire1.
 * Na nowym rdzeniu ESP32 (core 3.x / IDF5) wielokrotne Wire1.begin()/end()
 * w petli powodowalo blad "I2C bus id(1) has already been acquired" - sterownik
 * nie zwalnial magistrali. Software I2C nie zajmuje sprzetowego sterownika,
 * wiec problem znika, a kazdy ekran ma swoje (zamienione) piny na stale.
 *
 * UWAGA SPRZETOWA
 * ---------------
 * - PSRAM JEST WLACZONY -> kamera moze uzywac pelnych rozdzielczosci, a bufor
 * klatki trafia do PSRAM. GPIO16 NALEZY do PSRAM - nie wolno go uzywac do
 * niczego innego (dlatego drugie oko jest na GPIO4, a nie na GPIO16).
 * - Jesli masz wyswietlacze SH1106 (czesto 1.3"), zamien w konstruktorach
 * SSD1306 na SH1106:
 * U8G2_SH1106_128X64_NONAME_F_SW_I2C displayA(U8G2_R0, 4, 15, U8X8_PIN_NONE);
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"

// ============================================================
//  USTAWIENIA SIECI
// ============================================================
const char* ssid       = "Wifi z telefonu";
const char* password   = "haslo123";
const char* apssid     = "ESP32-CAM";
const char* appassword = "12345678";

// ============================================================
//  DWA WYSWIETLACZE = DWOJE OCZU  (software I2C, zamienione piny)
// ============================================================
#define PIN_15        15
#define PIN_4          4
#define I2C_BUS_CLOCK 400000   // taktowanie software I2C (zmniejsz jesli sa zaklocenia)

// SW I2C = bit-bang. Kolejnosc argumentow: (rotacja, SCL/clock, SDA/data, reset).
// Piny SA ZAMIENIONE miedzy ekranami (to wlasnie ten trik):
U8G2_SSD1306_128X64_NONAME_F_SW_I2C displayA(U8G2_R0, /*SCL=*/PIN_4,  /*SDA=*/PIN_15, U8X8_PIN_NONE); // lewe oko
U8G2_SSD1306_128X64_NONAME_F_SW_I2C displayB(U8G2_R0, /*SCL=*/PIN_15, /*SDA=*/PIN_4,  U8X8_PIN_NONE); // prawe oko

// ============================================================
//  PINY KAMERY (AI-Thinker ESP32-CAM)
// ============================================================
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ============================================================
//  >>> EDYTUJ TUTAJ: KLASY  ->  EMOCJE <<<
// ============================================================
enum EmotionType { NEUTRAL, ANGRY, SAD, WINK };

#define CONFIDENCE_THRESHOLD   0.75f   // minimalna pewnosc modelu, by zareagowac
#define CLASS_OVERRIDE_TIMEOUT 5000    // ms bez detekcji -> powrot do trybu auto

// Nazwa klasy z Teachable Machine  ->  emocja oczu.
struct ClassEmotion {
  const char* name;      // nazwa klasy tak, jak przychodzi z przegladarki
  EmotionType emotion;   // emocja przypisana do tej klasy
};

ClassEmotion CLASS_TABLE[] = {
  { "Class%201", NEUTRAL },   // klasa 1 -> spokoj
  { "Class%202", ANGRY   },   // klasa 2 -> zlosc
  { "Class%203", WINK    },   // klasa 3 -> mrugniecie
};
const int CLASS_COUNT = sizeof(CLASS_TABLE) / sizeof(CLASS_TABLE[0]);

// ============================================================
//  SILNIK FIZYKI OKA (interpolacja float)
// ============================================================
float pupilRatio  = 0.05f;
float eyelidRatio = 0.20f;
float browRatio   = 0.12f;

float targetPupilX  = 0,   currentPupilX  = 0;
float targetEyelidA = 23, currentEyelidA = 23;   // lewe oko
float targetEyelidB = 23, currentEyelidB = 23;   // prawe oko

float targetBrowThickTop = 4,  currentBrowThickTop = 4;
float targetBrowThickBot = 11, currentBrowThickBot = 11;
float targetBrowPoint    = 8,  currentBrowPoint    = 8;

int renderPupilX, renderLidA, renderLidB;
int renderBrowThickTop, renderBrowThickBot, renderBrowPoint;

EmotionType currentEmotion  = NEUTRAL;
EmotionType overrideEmotion = NEUTRAL;
bool        classOverrideActive    = false;
unsigned long lastClassDetectionTime = 0;
String activeClassName = ""; // Przechowuje tekst aktualnej klasy do wyswietlenia

unsigned long lastBehaviorChange   = 0;
int           timeBetweenBehavior = 4000;
unsigned long lastBlinkAction     = 0;
int           timeBetweenBlinks   = 3000;

// ============================================================
//  POMOCNICZE DO SERWOWANIA JPEG (bezposrednio z kamery)
// ============================================================
typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

// ============================================================
//  SERWER WWW
// ============================================================
String Feedback = "", Command = "", cmd = "", P1 = "", P2 = "", P3 = "", P4 = "";
String P5 = "", P6 = "", P7 = "", P8 = "", P9 = "";
byte ReceiveState = 0, cmdState = 1, strState = 1;
byte questionstate = 0, equalstate = 0, semicolonstate = 0;

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

extern const char INDEX_HTML[] PROGMEM;

// ============================================================
//  EMOCJE + RYSOWANIE OKA
// ============================================================
void applyEmotionTargets(EmotionType emotion) {
  switch (emotion) {
    case NEUTRAL:
    case WINK:
      targetBrowThickTop = 4;  targetBrowThickBot = 11; targetBrowPoint = 8;
      break;
    case ANGRY:
      targetBrowThickTop = 2;  targetBrowThickBot = 10; targetBrowPoint = 19;
      break;
    case SAD:
      targetBrowThickTop = 13; targetBrowThickBot = 20; targetBrowPoint = 3;
      break;
  }
}

void drawCatEye(U8G2 &u8g2, bool isLeftEye, int eyelidValue) {
  u8g2.clearBuffer();
  const int cX = 64, cY = 40;
  const int rx = 34, ry = 20;
  const int pupilMaxW = 9, pupilH = 40;

  u8g2.setDrawColor(1);
  u8g2.drawEllipse(cX, cY, rx,     ry,     U8G2_DRAW_ALL);
  u8g2.drawEllipse(cX, cY, rx + 1, ry + 1, U8G2_DRAW_ALL);
  u8g2.drawEllipse(cX, cY, rx + 2, ry + 2, U8G2_DRAW_ALL);

  int startY = cY - (pupilH / 2);
  int endY   = cY + (pupilH / 2);
  for (int y = startY; y <= endY; y++) {
    int dy = y - cY;
    if (dy > ry || dy < -ry) continue;
    float eyeWidthAtY = rx * sqrt(1.0f - (float)(dy * dy) / (float)(ry * ry));
    int eyeLeft  = cX - (int)eyeWidthAtY;
    int eyeRight = cX + (int)eyeWidthAtY;
    float normY = (float)dy / (pupilH / 2.0f);
    float pupilWidthAtY = pupilMaxW * (1.0f - normY * normY);
    int pupilCenter = cX + renderPupilX;
    int pupilLeft   = pupilCenter - (int)(pupilWidthAtY / 2.0f);
    int pupilRight  = pupilLeft + (int)pupilWidthAtY;
    int drawLeft  = max(pupilLeft, eyeLeft);
    int drawRight = min(pupilRight, eyeRight);
    if (drawLeft <= drawRight)
      u8g2.drawHLine(drawLeft, y, drawRight - drawLeft + 1);
  }

  // powieki (czarne prostokaty od gory i dolu)
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, 0,                128, cY - eyelidValue);
  u8g2.drawBox(0, cY + eyelidValue, 128, 64 - (cY + eyelidValue));

  // cienka kreska gdy oko prawie zamkniete
  u8g2.setDrawColor(1);
  if (eyelidValue <= 1) {
    u8g2.drawBox(cX - (rx + 2), cY - 1, (rx + 2) * 2, 3);
  }

  // brwi - lustrzane miedzy okiem lewym a prawym
  if (isLeftEye) {
    u8g2.drawTriangle(cX - 34, renderBrowThickTop, cX - 34, renderBrowThickBot, cX + 34, renderBrowPoint);
  } else {
    u8g2.drawTriangle(cX - 34, renderBrowPoint, cX + 34, renderBrowThickTop, cX + 34, renderBrowThickBot);
  }

  // --- Rysowanie podpisu aktualnej klasy (Subtitle) ---
  if (classOverrideActive && activeClassName.length() > 0) {
    u8g2.setDrawColor(1);            
    u8g2.setFont(u8g2_font_6x10_tf); 
    int strWidth = u8g2.getStrWidth(activeClassName.c_str());
    u8g2.drawStr((128 - strWidth) / 2, 11, activeClassName.c_str());
  }
}

bool emotionForClass(const String& className, EmotionType &out) {
  for (int i = 0; i < CLASS_COUNT; i++) {
    if (className == CLASS_TABLE[i].name) { out = CLASS_TABLE[i].emotion; return true; }
  }
  return false;
}

// ============================================================
//  PARSER POLECEN /control
// ============================================================
void getCommand(char c) {
  if (c == '?') ReceiveState = 1;
  if (c == ' ' || c == '\r' || c == '\n') ReceiveState = 0;
  if (ReceiveState = 1) {
    Command = Command + String(c);
    if (c == '=') cmdState = 0;
    if (c == ';') strState++;
    if (cmdState == 1 && (c != '?' || questionstate == 1)) cmd += String(c);
    if (cmdState == 0 && strState == 1 && (c != '=' || equalstate == 1)) P1 += String(c);
    if (cmdState == 0 && strState == 2 && c != ';') P2 += String(c);
    if (cmdState == 0 && strState == 3 && c != ';') P3 += String(c);
    if (cmdState == 0 && strState == 4 && c != ';') P4 += String(c);
    if (cmdState == 0 && strState == 5 && c != ';') P5 += String(c);
    if (cmdState == 0 && strState == 6 && c != ';') P6 += String(c);
    if (cmdState == 0 && strState == 7 && c != ';') P7 += String(c);
    if (cmdState == 0 && strState == 8 && c != ';') P8 += String(c);
    if (cmdState == 0 && strState >= 9 && (c != ';' || semicolonstate == 1)) P9 += String(c);
    if (c == '?') questionstate = 1;
    if (c == '=') equalstate = 1;
    if (strState >= 9 && c == ';') semicolonstate = 1;
  }
}

// ============================================================
//  HANDLERY HTTP
// ============================================================
static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) j->len = 0;
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) return 0;
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res;
  if (fb->format == PIXFORMAT_JPEG) {
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    jpg_chunking_t jchunk = { req, 0 };
    res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
  }
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool ok = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!ok) { Serial.println("JPEG compression failed"); res = ESP_FAIL; }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }

    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }

    if (fb) { esp_camera_fb_return(fb); fb = NULL; _jpg_buf = NULL; }
    else if (_jpg_buf) { free(_jpg_buf); _jpg_buf = NULL; }

    if (res != ESP_OK) break;
  }
  return res;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char *buf;
  size_t buf_len;
  char variable[128] = {0}, value[128] = {0};
  String myCmd = "";

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
          httpd_query_key_value(buf, "val", value,    sizeof(value))    == ESP_OK) {
      } else {
        myCmd = String(buf);
      }
    }
    free(buf);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  Feedback = Command = cmd = P1 = P2 = P3 = P4 = P5 = P6 = P7 = P8 = P9 = "";
  ReceiveState = 0; cmdState = 1; strState = 1;
  questionstate = 0; equalstate = 0; semicolonstate = 0;

  if (myCmd.length() > 0) {
    myCmd = "?" + myCmd;
    for (int i = 0; i < (int)myCmd.length(); i++) getCommand(char(myCmd.charAt(i)));
  }

  if (cmd.length() > 0) {
    if (cmd == "serial") {
      String className = P1;
      float  confidence = P2.toFloat();
      EmotionType emo;
      if (confidence >= CONFIDENCE_THRESHOLD && emotionForClass(className, emo)) {
        overrideEmotion        = emo;
        classOverrideActive    = true;
        lastClassDetectionTime = millis();
        
        activeClassName = className;
        activeClassName.replace("%20", " "); 
        
        Serial.printf("[TM] %s (%.2f) -> emocja %d\n", className.c_str(), confidence, (int)emo);
      } else {
        classOverrideActive = false;   
        activeClassName = "";
      }
    }
    else if (cmd == "restart") { ESP.restart(); }
    else if (cmd == "flash")   { }
    else if (cmd == "ip") {
      Feedback  = "AP IP: "  + WiFi.softAPIP().toString();
      Feedback += "<br>STA IP: " + WiFi.localIP().toString();
    }
    else if (cmd == "mac") { Feedback = "STA MAC: " + WiFi.macAddress(); }
    else { Feedback = "Command is not defined"; }

    if (Feedback == "") Feedback = Command;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, Feedback.c_str(), Feedback.length());
  } else {
    int val = atoi(value);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;
    if       (!strcmp(variable, "framesize"))  { if (s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val); }
    else if (!strcmp(variable, "quality"))    res = s->set_quality(s, val);
    else if (!strcmp(variable, "contrast"))   res = s->set_contrast(s, val);
    else if (!strcmp(variable, "brightness")) res = s->set_brightness(s, val);
    else if (!strcmp(variable, "hmirror"))    res = s->set_hmirror(s, val);
    else if (!strcmp(variable, "vflip"))      res = s->set_vflip(s, val);
    else if (!strcmp(variable, "flash"))      { }
    else res = -1;
    if (res) return httpd_resp_send_500(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
  }
}

static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[512];
  sensor_t *s = esp_camera_sensor_get();
  char *p = json_response;
  *p++ = '{';
  p += sprintf(p, "\"flash\":%d,", 0);
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,",   s->status.quality);
  p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p += sprintf(p, "\"contrast\":%d,",   s->status.contrast);
  p += sprintf(p, "\"hmirror\":%u,",    s->status.hmirror);
  p += sprintf(p, "\"vflip\":%u",       s->status.vflip);
  *p++ = '}'; *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_uri_t index_uri   = { "/",        HTTP_GET, index_handler,   NULL };
  httpd_uri_t status_uri  = { "/status",  HTTP_GET, status_handler,  NULL };
  httpd_uri_t cmd_uri     = { "/control", HTTP_GET, cmd_handler,     NULL };
  httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, NULL };
  httpd_uri_t stream_uri  = { "/stream",  HTTP_GET, stream_handler,  NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
  }
  config.server_port++; config.ctrl_port++;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

esp_err_t initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  
  // --- ZMIANA: Domyślna rozdzielczość startowa ustawiona na VGA ---
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size  = FRAMESIZE_VGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }
  return esp_camera_init(&config);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  if (initCamera() == ESP_OK) {
    sensor_t *s = esp_camera_sensor_get();
    if (s) { s->set_gain_ctrl(s, 1); s->set_exposure_ctrl(s, 1); s->set_aec2(s, 1); }
  } else {
    Serial.println("Camera init failed");
  }

  displayA.setI2CAddress(0x3C * 2);
  displayB.setI2CAddress(0x3C * 2);
  displayA.setBusClock(I2C_BUS_CLOCK);
  displayB.setBusClock(I2C_BUS_CLOCK);
  displayA.begin();
  displayB.begin();

  WiFi.mode(WIFI_AP_STA);
  for (int i = 0; i < 2; i++) {
    WiFi.begin(ssid, password);
    long int startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      if ((startTime + 5000) < millis()) break;
    }
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.softAP((WiFi.localIP().toString() + "_" + (String)apssid).c_str(), appassword);
      Serial.println("STA IP: " + WiFi.localIP().toString());
      break;
    }
  }
  if (WiFi.status() != WL_CONNECTED) WiFi.softAP(apssid, appassword);
  Serial.println("AP IP: " + WiFi.softAPIP().toString());

  // --- NOWOŚĆ: Wyświetlanie adresu IP na ekranach przy uruchomieniu ---
  String ipToDisplay = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String modeToDisplay = (WiFi.status() == WL_CONNECTED) ? "STA Mode (WiFi)" : "AP Mode (Direct)";

  // Wyświetlenie tekstu na lewym oku
  displayA.clearBuffer();
  displayA.setFont(u8g2_font_6x10_tf);
  displayA.drawStr(5, 20, modeToDisplay.c_str());
  displayA.drawStr(5, 40, "IP Address:");
  displayA.drawStr(5, 52, ipToDisplay.c_str());
  displayA.sendBuffer();

  // Wyświetlenie tekstu na prawym oku
  displayB.clearBuffer();
  displayB.setFont(u8g2_font_6x10_tf);
  displayB.drawStr(5, 20, modeToDisplay.c_str());
  displayB.drawStr(5, 40, "IP Address:");
  displayB.drawStr(5, 52, ipToDisplay.c_str());
  displayB.sendBuffer();

  delay(3000); // Zatrzymanie programu na 3 sekundy, aby odczytać link

  startCameraServer();
  randomSeed(esp_random());
}

// ============================================================
//  GLOWNA PETLA: animacja + przeplot dwoch oczu
// ============================================================
void loop() {
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame < 67) { delay(5); return; }   
  lastFrame = millis();

  unsigned long now = millis();

  if (classOverrideActive) {
    if (now - lastClassDetectionTime > CLASS_OVERRIDE_TIMEOUT) {
      classOverrideActive = false;
      activeClassName = "";
      Serial.println("[EYE] powrot do trybu autonomicznego");
    } else {
      currentEmotion = overrideEmotion;
      applyEmotionTargets(currentEmotion);
      targetPupilX = 0;                 
      
      currentPupilX       = targetPupilX;
      currentBrowThickTop = targetBrowThickTop;
      currentBrowThickBot = targetBrowThickBot;
      currentBrowPoint    = targetBrowPoint;
      
      if (currentEmotion == WINK) {
        targetEyelidA = 0;   
        targetEyelidB = 23;  
      } else {
        targetEyelidA = 23;
        targetEyelidB = 23;
      }
      currentEyelidA = targetEyelidA;
      currentEyelidB = targetEyelidB;
    }
  }
  
  if (!classOverrideActive) {
    currentPupilX       += (targetPupilX       - currentPupilX)       * pupilRatio;
    currentEyelidA      += (targetEyelidA      - currentEyelidA)      * eyelidRatio;
    currentEyelidB      += (targetEyelidB      - currentEyelidB)      * eyelidRatio;
    currentBrowThickTop += (targetBrowThickTop - currentBrowThickTop) * browRatio;
    currentBrowThickBot += (targetBrowThickBot - currentBrowThickBot) * browRatio;
    currentBrowPoint    += (targetBrowPoint    - currentBrowPoint)    * browRatio;

    if (now - lastBehaviorChange > (unsigned long)timeBetweenBehavior) {
      lastBehaviorChange  = now;
      timeBetweenBehavior = random(3000, 6000);
      int dir = random(0, 100);
      if       (dir < 40) targetPupilX = 0;
      else if (dir < 70) targetPupilX = -19;
      else               targetPupilX = 19;

      int e = random(0, 100);
      if       (e < 50) currentEmotion = NEUTRAL;
      else if (e < 70) currentEmotion = ANGRY;
      else if (e < 90) currentEmotion = SAD;
      else             currentEmotion = WINK;
      applyEmotionTargets(currentEmotion);
    }

    static EmotionType prevEmotion = NEUTRAL;
    bool emotionChanged = (currentEmotion != prevEmotion);
    prevEmotion = currentEmotion;

    if (currentEmotion == WINK) {
      targetEyelidA = 0;    
      targetEyelidB = 23;   
    } else {
      if (emotionChanged) { targetEyelidA = 23; targetEyelidB = 23; lastBlinkAction = now; }
      if (targetEyelidA > 0 && targetEyelidB > 0 &&
          (now - lastBlinkAction > (unsigned long)timeBetweenBlinks)) {
        targetEyelidA = 0; targetEyelidB = 0;            
        lastBlinkAction = now;
      } else if (targetEyelidA == 0 && targetEyelidB == 0 &&
                 currentEyelidA < 0.5f && currentEyelidB < 0.5f) {
        targetEyelidA = 23; targetEyelidB = 23;          
        timeBetweenBlinks = random(2000, 5000);
        lastBlinkAction = now;
      }
    }
  }

  renderPupilX       = (int)currentPupilX;
  renderLidA         = (int)currentEyelidA;
  renderLidB         = (int)currentEyelidB;
  renderBrowThickTop = (int)currentBrowThickTop;
  renderBrowThickBot = (int)currentBrowThickBot;
  renderBrowPoint    = (int)currentBrowPoint;

  static bool drawLeftNext = true;
  if (drawLeftNext) {
    drawCatEye(displayA, true, renderLidA);
    displayA.sendBuffer();
  } else {
    drawCatEye(displayB, false, renderLidB);
    displayB.sendBuffer();
  }
  drawLeftNext = !drawLeftNext;
}

// ============================================================
//  STRONA WWW (panel kamery + Teachable Machine)
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html>
    <head>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width,initial-scale=1">
        <meta http-equiv="Access-Control-Allow-Headers" content="Origin, X-Requested-With, Content-Type, Accept">
        <meta http-equiv="Access-Control-Allow-Methods" content="GET,POST,PUT,DELETE,OPTIONS">
        <meta http-equiv="Access-Control-Allow-Origin" content="*">
        <title>Teachable Machine</title>
        <style>
          body{font-family:Arial,Helvetica,sans-serif;background:#181818;color:#EFEFEF;font-size:16px}h2{font-size:18px}section.main{display:flex}#menu,section.main{flex-direction:column}#menu{display:flex;flex-wrap:nowrap;min-width:340px;background:#363636;padding:8px;border-radius:4px;margin-top:-10px;margin-right:10px}#content{display:flex;flex-wrap:wrap;align-items:stretch}figure{padding:0;margin:0;-webkit-margin-before:0;margin-block-start:0;-webkit-margin-after:0;margin-block-end:0;-webkit-margin-start:0;margin-inline-start:0;-webkit-margin-end:0;margin-inline-end:0}figure img{display:block;width:100%;height:auto;border-radius:4px;margin-top:8px}@media (min-width: 800px) and (orientation:landscape){#content{display:flex;flex-wrap:nowrap;align-items:stretch}figure img{display:block;max-width:100%;max-height:calc(100vh - 40px);width:auto;height:auto}figure{padding:0;margin:0;-webkit-margin-before:0;margin-block-start:0;-webkit-margin-after:0;margin-block-end:0;-webkit-margin-start:0;margin-inline-start:0;-webkit-margin-end:0;margin-inline-end:0}}section#buttons{display:flex;flex-wrap:nowrap;justify-content:space-between}#nav-toggle{cursor:pointer;display:block}#nav-toggle-cb{outline:0;opacity:0;width:0;height:0}#nav-toggle-cb:checked+#menu{display:none}.input-group{display:flex;flex-wrap:nowrap;line-height:22px;margin:5px 0}.input-group>label{display:inline-block;padding-right:10px;min-width:47%}.input-group input,.input-group select{flex-grow:1}.range-max,.range-min{display:inline-block;padding:0 5px}button{display:block;margin:5px;padding:0 12px;border:0;line-height:28px;cursor:pointer;color:#fff;background:#ff3034;border-radius:5px;font-size:16px;outline:0}button:hover{background:#ff494d}button:active{background:#f21c21}button.disabled{cursor:default;background:#a0a0a0}input[type=range]{-webkit-appearance:none;width:100%;height:22px;background:#363636;cursor:pointer;margin:0}input[type=range]:focus{outline:0}input[type=range]::-webkit-slider-runnable-track{width:100%;height:2px;cursor:pointer;background:#EFEFEF;border-radius:0;border:0 solid #EFEFEF}input[type=range]::-webkit-slider-thumb{border:1px solid rgba(0,0,30,0);height:22px;width:22px;border-radius:50px;background:#ff3034;cursor:pointer;-webkit-appearance:none;margin-top:-11.5px}input[type=range]:focus::-webkit-slider-runnable-track{background:#EFEFEF}input[type=range]::-moz-range-track{width:100%;height:2px;cursor:pointer;background:#EFEFEF;border-radius:0;border:0 solid #EFEFEF}input[type=range]::-moz-range-thumb{border:1px solid rgba(0,0,30,0);height:22px;width:22px;border-radius:50px;background:#ff3034;cursor:pointer}input[type=range]::-ms-track{width:100%;height:2px;cursor:pointer;background:0 0;border-color:transparent;color:transparent}input[type=range]::-ms-fill-lower{background:#EFEFEF;border:0 solid #EFEFEF;border-radius:0}input[type=range]::-ms-fill-upper{background:#EFEFEF;border:0 solid #EFEFEF;border-radius:0}input[type=range]::-ms-thumb{border:1px solid rgba(0,0,30,0);height:22px;width:22px;border-radius:50px;background:#ff3034;cursor:pointer;height:2px}input[type=range]:focus::-ms-fill-lower{background:#EFEFEF}input[type=range]:focus::-ms-fill-upper{background:#363636}.switch{display:block;position:relative;line-height:22px;font-size:16px;height:22px}.switch input{outline:0;opacity:0;width:0;height:0}.slider{width:50px;height:22px;border-radius:22px;cursor:pointer;background-color:grey}.slider,.slider:before{display:inline-block;transition:.4s}.slider:before{position:relative;content:"";border-radius:50%;height:16px;width:16px;left:4px;top:3px;background-color:#fff}input:checked+.slider{background-color:#ff3034}input:checked+.slider:before{-webkit-transform:translateX(26px);transform:translateX(26px)}select{border:1px solid #363636;font-size:14px;height:22px;outline:0;border-radius:5px}.image-container{position:relative;min-width:160px}.close{position:absolute;right:5px;top:5px;background:#ff3034;width:16px;height:16px;border-radius:100px;color:#fff;text-align:center;line-height:18px;cursor:pointer}.hidden{display:none}
        </style>
        <script src="https:\/\/ajax.googleapis.com/ajax/libs/jquery/1.8.0/jquery.min.js"></script>
        <script src="https:\/\/cdn.jsdelivr.net/npm/@tensorflow/tfjs@1.3.1/dist/tf.min.js"></script>
        <script src="https:\/\/cdn.jsdelivr.net/npm/@teachablemachine/image@0.8/dist/teachablemachine-image.min.js"></script>
        <script src="https:\/\/cdn.jsdelivr.net/npm/@teachablemachine/pose@0.8/dist/teachablemachine-pose.min.js"></script>
    </head>
    <body>
        <section class="main">
            <figure>
              <div id="stream-container" class="image-container hidden">
                <div class="close" id="close-stream" style="display:none">×</div>
                <img id="stream" src="" style="display:none" crossorigin="anonymous">
                <canvas id="canvas" width="0" height="0"></canvas>
              </div>
            </figure>
            <section id="buttons">
                <table>
                <tr><td><button id="restart" onclick="try{fetch(document.location.origin+'/control?restart');}catch(e){}">Restart</button></td><td><button id="get-still" style="display:none">Get Still</button></td><td><button id="toggle-stream" style="display:none"></td></tr>
                </table>
            </section>
            <div id="logo">
                <label for="nav-toggle-cb" id="nav-toggle">&#9776;&nbsp;&nbsp;Toggle settings</label>
            </div>
            <div id="content">
                <div id="sidebar">
                    <input type="checkbox" id="nav-toggle-cb">
                    <nav id="menu">
                        <div class="input-group">
                          <label for="kind">Kind</label>
                          <select id="kind">
                            <option value="image">image</option>
                            <option value="pose">pose</option>
                          </select>
                        </div>
                        <div class="input-group">
                          <label for="modelPath">Model Path</label>
                          <input type="text" id="modelPath" value="">
                        </div>
                        <div class="input-group">
                            <label for="btnModel"></label>
                            <button type="button" id="btnModel" onclick="LoadModel();">Start Recognition</button>
                        </div>
                        <div class="input-group" id="framesize-group">
                            <label for="framesize">Resolution</label>
                            <select id="framesize" class="default-action">
                                <option value="10">UXGA(1600x1200)</option>
                                <option value="9">SXGA(1280x1024)</option>
                                <option value="8">XGA(1024x768)</option>
                                <option value="7">SVGA(800x600)</option>
                                <option value="6" selected="selected">VGA(640x480)</option>
                                <option value="5">CIF(400x296)</option>
                                <option value="4">QVGA(320x240)</option>
                                <option value="3">HQVGA(240x176)</option>
                                <option value="0">QQVGA(160x120)</option>
                            </select>
                        </div>
                        <div class="input-group" id="quality-group">
                            <label for="quality">Quality</label>
                            <div class="range-min">10</div>
                            <input type="range" id="quality" min="10" max="63" value="10" class="default-action">
                            <div class="range-max">63</div>
                        </div>
                        <div class="input-group" id="brightness-group">
                            <label for="brightness">Brightness</label>
                            <div class="range-min">-2</div>
                            <input type="range" id="brightness" min="-2" max="2" value="0" class="default-action">
                            <div class="range-max">2</div>
                        </div>
                        <div class="input-group" id="contrast-group">
                            <label for="contrast">Contrast</label>
                            <div class="range-min">-2</div>
                            <input type="range" id="contrast" min="-2" max="2" value="0" class="default-action">
                            <div class="range-max">2</div>
                        </div>
                        <div class="input-group" id="hmirror-group">
                            <label for="hmirror">H-Mirror</label>
                            <div class="switch">
                                <input id="hmirror" type="checkbox" class="default-action" checked="checked">
                                <label class="slider" for="hmirror"></label>
                            </div>
                        </div>
                        <div class="input-group" id="vflip-group">
                            <label for="vflip">V-Flip</label>
                            <div class="switch">
                                <input id="vflip" type="checkbox" class="default-action" checked="checked">
                                <label class="slider" for="vflip"></label>
                            </div>
                        </div>
                    </nav>
                </div>
            </div>
        </section>
        <br>
        <div id="result" style="color:red"><div>

        <script>
          document.addEventListener('DOMContentLoaded', function (event) {
            var baseHost = document.location.origin
            var streamUrl = baseHost + ':81'

            const hide = el => { el.classList.add('hidden') }
            const show = el => { el.classList.remove('hidden') }

            const updateValue = (el, value, updateRemote) => {
              updateRemote = updateRemote == null ? true : updateRemote
              let initialValue
              if (el.type === 'checkbox') {
                initialValue = el.checked
                value = !!value
                el.checked = value
              } else {
                initialValue = el.value
                el.value = value
              }
              if (updateRemote && initialValue !== value) {
                updateConfig(el);
              }
            }

            function updateConfig (el) {
              let value
              switch (el.type) {
                case 'checkbox':
                  value = el.checked ? 1 : 0
                  break
                case 'range':
                case 'select-one':
                  value = el.value
                  break
                case 'button':
                case 'submit':
                  value = '1'
                  break
                default:
                  return
              }
              const query = `${baseHost}/control?var=${el.id}&val=${value}`
              fetch(query).then(response => {
                console.log(`request to ${query} finished, status: ${response.status}`)
              })
            }

            document.querySelectorAll('.close').forEach(el => {
              el.onclick = () => { hide(el.parentNode) }
            })

            fetch(`${baseHost}/status`)
              .then(function (response) { return response.json() })
              .then(function (state) {
                document.querySelectorAll('.default-action').forEach(el => {
                  updateValue(el, state[el.id], false)
                })
              })

            const view = document.getElementById('stream')
            const viewContainer = document.getElementById('stream-container')
            const stillButton = document.getElementById('get-still')
            const streamButton = document.getElementById('toggle-stream')
            const closeButton = document.getElementById('close-stream')

            const stopStream = () => {
              view.src = "";
              streamButton.innerHTML = 'Start Stream'
            }

            const startStream = () => {
              view.src = `${streamUrl}/stream`
              show(viewContainer)
              streamButton.innerHTML = 'Stop Stream'
            }

            stillButton.onclick = () => {
              stopStream()
              view.src = `${baseHost}/capture?_cb=${Date.now()}`
              show(viewContainer)
            }

            closeButton.onclick = () => {
              stopStream()
              hide(viewContainer)
            }

            streamButton.onclick = () => {
              const streamEnabled = streamButton.innerHTML === 'Stop Stream'
              if (streamEnabled) { stopStream() } else { startStream() }
            }

            document.querySelectorAll('.default-action').forEach(el => {
              el.onchange = () => updateConfig(el)
            })
          })
        </script>

        <script>
        var getStill = document.getElementById('get-still');
        var ShowImage = document.getElementById('stream');
        var canvas = document.getElementById("canvas");
        var context = canvas.getContext("2d");
        var modelPath = document.getElementById('modelPath');
        var result = document.getElementById('result');
        var kind = document.getElementById('kind');
        let Model;

        async function LoadModel() {
          if (modelPath.value=="") {
            result.innerHTML = "Please input model path.";
            return;
          }
          result.innerHTML = "Please wait for loading model.";
          const URL = modelPath.value;
          const modelURL = URL + "model.json";
          const metadataURL = URL + "metadata.json";
          if (kind.value=="image") {
            Model = await tmImage.load(modelURL, metadataURL);
          } else if (kind.value=="pose") {
            Model = await tmPose.load(modelURL, metadataURL);
          }
          maxPredictions = Model.getTotalClasses();
          result.innerHTML = "";
          getStill.style.display = "block";
          getStill.click();
        }

        async function predict() {
          var data = "";
          var maxClassName = "";
          var maxProbability = "";

          canvas.setAttribute("width", ShowImage.width);
          canvas.setAttribute("height", ShowImage.height);
          context.drawImage(ShowImage, 0, 0, ShowImage.width, ShowImage.height);

          if (kind.value=="image")
            var prediction = await Model.predict(canvas);
          else if (kind.value=="pose") {
            var { pose, posenetOutput } = await Model.estimatePose(canvas);
            var prediction = await Model.predict(posenetOutput);
          }

          if (maxPredictions>0) {
            for (let i = 0; i < maxPredictions; i++) {
              if (i==0) {
                maxClassName = prediction[i].className;
                maxProbability = prediction[i].probability;
              } else {
                if (prediction[i].probability>maxProbability) {
                  maxClassName = prediction[i].className;
                  maxProbability = prediction[i].probability;
                }
              }
              data += prediction[i].className + "," + prediction[i].probability.toFixed(2) + "<br>";
            }
            result.innerHTML = data;
            result.innerHTML += "<br>Result: " + maxClassName + "," + maxProbability;
            $.ajax({url: document.location.origin+'/control?serial='+maxClassName+";"+maxProbability+';stop', async: false});
          } else {
            result.innerHTML = "Unrecognizable";
          }
          getStill.click();
        }

        ShowImage.onload = function (event) {
          if (Model) {
            try {
              document.createEvent("TouchEvent");
              setTimeout(function(){predict();},250);
            } catch(e) {
              predict();
            }
          }
        }
        </script>
    </body>
</html>)rawliteral";
