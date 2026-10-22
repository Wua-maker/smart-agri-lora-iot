// ==== RX: ESP32 + SX1262 LoRa + WiFi + MQTT + OLED ====
// 開源安全版：Wi-Fi 與 MQTT 憑證已抽離至 config.h

#include <RadioLib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <esp_bt.h>
#include <ArduinoJson.h>

// ---------- 引入憑證設定檔 ----------
#if __has_include("config.h")
  #include "config.h"
#else
  #include "config.example.h"
  #warning "使用預設 config.example.h，請複製並重新命名為 config.h 填入真實連線資訊。"
#endif

// ---------- LoRa 射頻參數 ----------
SX1262 radio = new Module(5, 26, 14, 25); // NSS=5, DIO1=26, RST=14, BUSY=25
constexpr float RF_FREQUENCY     = 923.2;  // MHz
constexpr float RF_BANDWIDTH     = 125.0;  // kHz
constexpr uint8_t RF_SPREADING   = 9;
constexpr uint8_t RF_CODING_RATE = 5;

// ---------- OLED 顯示螢幕 ----------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ---------- 網路客戶端 ----------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// ---------- 狀態資料快取 ----------
String lastEvent = "none";
float vbat       = NAN;
int   bat_pct    = 0;

float soil_moist = NAN;
float soil_temp  = NAN;
float soil_ec    = NAN;
float soil_ph    = NAN;
float soil_n     = NAN;
float soil_p     = NAN;
float soil_k     = NAN;
int   soil_error = 1;

float lastRSSI   = NAN;
float lastSNR    = NAN;

uint32_t lastRxTime     = 0;
uint32_t lastMQTTPush   = 0;

uint8_t  pageIndex      = 0;
uint32_t lastPageSwitch = 0;
const uint32_t PAGE_INTERVAL = 3000; // 每頁輪播 3 秒

// ---------- OLED 繪圖函式 ----------
void drawPage0() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "LoRa-RX Ready");

  char line[32];
  if (!isnan(lastRSSI) && !isnan(lastSNR)) {
    snprintf(line, sizeof(line), "RSSI:%ddBm  SNR:%.1f", (int)lastRSSI, lastSNR);
    u8g2.drawStr(0, 22, line);
  } else {
    u8g2.drawStr(0, 22, "RSSI/SNR: N/A");
  }

  if (!isnan(vbat)) {
    snprintf(line, sizeof(line), "VBAT: %.2f V  %d%%", vbat, bat_pct);
    u8g2.drawStr(0, 34, line);
  } else {
    u8g2.drawStr(0, 34, "VBAT: N/A");
  }

  snprintf(line, sizeof(line), "EVENT: %s", lastEvent.c_str());
  u8g2.drawStr(0, 46, line);

  uint32_t age = (lastRxTime == 0) ? 0 : (millis() - lastRxTime) / 1000;
  snprintf(line, sizeof(line), "AGE: %lus", (unsigned long)age);
  u8g2.drawStr(0, 58, line);

  u8g2.sendBuffer();
}

void drawPage1() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Soil Sensor");

  if (soil_error != 0) {
    u8g2.drawStr(0, 24, "ERROR: soil_error=1");
    u8g2.sendBuffer();
    return;
  }

  char line[32];
  snprintf(line, sizeof(line), "Moist: %.1f %%", soil_moist);
  u8g2.drawStr(0, 22, line);

  snprintf(line, sizeof(line), "Temp : %.1f C", soil_temp);
  u8g2.drawStr(0, 34, line);

  snprintf(line, sizeof(line), "EC   : %.0f uS/cm", soil_ec);
  u8g2.drawStr(0, 46, line);

  snprintf(line, sizeof(line), "pH:%.1f N:%d P:%d K:%d", soil_ph, (int)soil_n, (int)soil_p, (int)soil_k);
  u8g2.drawStr(0, 58, line);

  u8g2.sendBuffer();
}

void drawNoSignal() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 20, "No LoRa Signal >30s");
  u8g2.sendBuffer();
}

void drawOLED() {
  if (lastRxTime != 0 && (millis() - lastRxTime > 30000)) {
    drawNoSignal();
    return;
  }

  uint32_t now = millis();
  if (now - lastPageSwitch > PAGE_INTERVAL) {
    pageIndex = (pageIndex + 1) % 2;
    lastPageSwitch = now;
  }

  if (pageIndex == 0) drawPage0();
  else drawPage1();
}

// ---------- Wi-Fi 連線排查與建立 ----------
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PSK);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0 < 10000)) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi OK" : "\nWiFi FAIL");
}

// ---------- MQTT 連線管理 ----------
void ensureMQTT() {
  if (mqtt.connected()) return;

  espClient.setInsecure(); // 關閉 TLS 憑證樹驗證
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  String clientId = "LoRa-RX-Gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    Serial.println("MQTT connected");
  } else {
    Serial.println("MQTT connect failed");
  }
}

void publishJSON(bool ok, bool alert) {
  StaticJsonDocument<512> doc;

  doc["device"]     = "LoRa-RX";
  doc["ok"]         = ok;
  doc["alert"]      = alert;
  doc["event"]      = lastEvent;

  doc["vbat"]       = vbat;
  doc["bat_pct"]    = bat_pct;

  doc["soil_moist"] = soil_moist;
  doc["soil_temp"]  = soil_temp;
  doc["soil_ec"]    = soil_ec;
  doc["soil_ph"]    = soil_ph;
  doc["soil_n"]     = soil_n;
  doc["soil_p"]     = soil_p;
  doc["soil_k"]     = soil_k;
  doc["soil_error"] = soil_error;

  doc["rssi"]       = lastRSSI;
  doc["snr"]        = lastSNR;
  doc["ts"]         = millis() / 1000;

  char json[512];
  size_t n = serializeJson(doc, json);
  mqtt.publish(MQTT_TOPIC, json, true);

  Serial.print("MQTT TX: ");
  Serial.write(json, n);
  Serial.println();
}

// ---------- 系統初始化 ----------
void setup() {
  Serial.begin(115200);
  delay(1000);

  // 關閉 Bluetooth 降低 2.4GHz 射頻干擾與功耗
  btStop();
  esp_bt_controller_disable();

  // OLED 初始化
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "LoRa RX Booting...");
  u8g2.sendBuffer();

  ensureWiFi();
  ensureMQTT();

  // LoRa 初始化
  int state = radio.begin(RF_FREQUENCY);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa init failed: %d\n", state);
    while (true) { delay(1000); }
  }

  radio.setBandwidth(RF_BANDWIDTH);
  radio.setSpreadingFactor(RF_SPREADING);
  radio.setCodingRate(RF_CODING_RATE);
  radio.setOutputPower(17);

  Serial.println("LoRa RX Ready!");
}

// ---------- 主迴圈 ----------
void loop() {
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  String str;
  int state = radio.receive(str);

  if (state == RADIOLIB_ERR_NONE) {
    lastRxTime = millis();
    lastRSSI   = radio.getRSSI();
    lastSNR    = radio.getSNR();

    Serial.print("LoRa RX: ");
    Serial.println(str);

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, str);
    if (!err) {
      const char* ev = doc["event"] | "unknown";
      lastEvent = String(ev);

      if (doc.containsKey("vbat"))     vbat    = doc["vbat"].as<float>();
      if (doc.containsKey("bat_pct")) bat_pct = doc["bat_pct"].as<int>();
      if (doc.containsKey("soil_error")) soil_error = doc["soil_error"].as<int>();

      if (soil_error == 0) {
        if (doc.containsKey("soil_moist")) soil_moist = doc["soil_moist"].as<float>();
        if (doc.containsKey("soil_temp"))  soil_temp  = doc["soil_temp"].as<float>();
        if (doc.containsKey("soil_ec"))    soil_ec    = doc["soil_ec"].as<float>();
        if (doc.containsKey("soil_ph"))    soil_ph    = doc["soil_ph"].as<float>();
        if (doc.containsKey("soil_n"))     soil_n     = doc["soil_n"].as<float>();
        if (doc.containsKey("soil_p"))     soil_p     = doc["soil_p"].as<float>();
        if (doc.containsKey("soil_k"))     soil_k     = doc["soil_k"].as<float>();
      }

      drawOLED();
      publishJSON(true, false);
    } else {
      Serial.printf("JSON parse error: %s\n", err.c_str());
    }
  }

  // 30 秒失聯警示偵測
  if (lastRxTime != 0 && (millis() - lastRxTime > 30000)) {
    drawNoSignal();

    if (millis() - lastMQTTPush > 5000) {
      lastMQTTPush = millis();
      lastEvent = "no_signal";
      publishJSON(false, true);
    }
  } else {
    drawOLED();
  }

  delay(50);
}