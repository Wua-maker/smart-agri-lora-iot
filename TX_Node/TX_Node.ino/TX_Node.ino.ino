// ==== TX: ESP32 + SX1262 LoRa + RS485 七合一 + Voltage + 防盜 ====
// 開源安全版：引腳配置與通訊協定

#include <RadioLib.h>
#include <HardwareSerial.h>

// ---------- LoRa SX1262 腳位定義 ----------
// NSS=5, DIO1=26, RST=14, BUSY=25
SX1262 radio = new Module(5, 26, 14, 25);

// ---------- LoRa 射頻參數設定 ----------
constexpr float RF_FREQUENCY       = 923.2; // 台灣 NCC 合規頻段 (920-925 MHz)
constexpr float RF_BANDWIDTH       = 125.0; // kHz
constexpr uint8_t RF_SPREADING     = 9;
constexpr uint8_t RF_CODING_RATE   = 5;
constexpr int8_t RF_OUTPUT_POWER   = 17;    // dBm

// ---------- RS485 七合一感測器 ----------
HardwareSerial RS485(1);
#define RS485_RX   4    // RO → ESP32 RX
#define RS485_TX   2    // DI → ESP32 TX
#define RS485_DIR  17   // DE+RE 短接 → DIR (HIGH=發送, LOW=接收)

// ---------- 電壓偵測 ----------
#define PIN_VBAT   34   // Voltage Sensor 輸出 → ADC (純輸入腳)

// ---------- 防盜系統 ----------
#define PIN_MICRO  27   // 微動開關：導通接地 = 開門 (LOW)
#define PIN_UNLOCK 15   // 隱藏開關：導通接地 = 解除防盜 (LOW)
#define PIN_BUZZ   13   // 主動蜂鳴器控制腳

// ---------- 傳送週期 ----------
const uint32_t SEND_INTERVAL = 10000; // 10 秒發送一次（測試用）
uint32_t lastSend = 0;

// ---------- 狀態機變數 ----------
bool tamperActive = false;
bool lastDoorOpen = false;
uint32_t lastBuzzToggle = 0;
bool buzzState = false;
uint32_t seq = 0;

// ==========================================================
// CRC16 校驗計算 (Modbus RTU)
// ==========================================================
uint16_t crc16(uint8_t* buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xA001;
      else
        crc >>= 1;
    }
  }
  return crc;
}

// ==========================================================
// 讀取七合一土壤感測器 (ID=1, FC=0x03, addr=0x0000, count=7)
// 回傳：0=成功、1=失敗
// ==========================================================
int readSoilSensor(float &soilMoist, float &soilTemp, float &soilEC,
                   float &soilPH, float &soilN, float &soilP, float &soilK) {
  uint8_t cmd[8] = {1, 3, 0x00, 0x00, 0x00, 0x07, 0, 0};
  uint16_t crc = crc16(cmd, 6);
  cmd[6] = crc & 0xFF;
  cmd[7] = (crc >> 8) & 0xFF;

  // 發送讀取指令
  digitalWrite(RS485_DIR, HIGH);
  delayMicroseconds(800);
  RS485.write(cmd, 8);
  RS485.flush();

  // 切換為接收模式
  digitalWrite(RS485_DIR, LOW);

  uint8_t resp[32];
  memset(resp, 0, sizeof(resp));

  int len = RS485.readBytes(resp, 19);
  if (len != 19) return 1;
  if (resp[0] != 1 || resp[1] != 3 || resp[2] != 0x0E) return 1;

  uint16_t recvCRC = resp[17] | (resp[18] << 8);
  uint16_t calcCRC = crc16(resp, 17);
  if (recvCRC != calcCRC) return 1;

  uint16_t reg[7];
  for (int i = 0; i < 7; i++) {
    reg[i] = (resp[3 + i * 2] << 8) | resp[4 + i * 2];
  }

  soilMoist = reg[0] / 10.0f; // %
  soilTemp  = reg[1] / 10.0f; // °C
  soilEC    = reg[2] * 1.0f;  // uS/cm
  soilPH    = reg[3] / 10.0f; // pH
  soilN     = reg[4];         // mg/kg
  soilP     = reg[5];         // mg/kg
  soilK     = reg[6];         // mg/kg

  return 0;
}

// ==========================================================
// 蜂鳴器控制（非阻塞式 0.5 秒切換）
// ==========================================================
void updateBuzzer(uint32_t now) {
  if (tamperActive) {
    if (now - lastBuzzToggle >= 500) {
      buzzState = !buzzState;
      digitalWrite(PIN_BUZZ, buzzState ? HIGH : LOW);
      lastBuzzToggle = now;
    }
  } else {
    buzzState = false;
    digitalWrite(PIN_BUZZ, LOW);
  }
}

// ==========================================================
// LoRa 傳送封包
// ==========================================================
void sendLoRa(String msg) {
  int16_t state = radio.transmit(msg);
  Serial.print("TX -> ");
  Serial.println(msg);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa TX error: %d\n", state);
  }
}

// ==========================================================
// 初始化
// ==========================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== TX Boot ===");

  pinMode(PIN_MICRO, INPUT_PULLUP);
  pinMode(PIN_UNLOCK, INPUT_PULLUP);
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW);

  pinMode(RS485_DIR, OUTPUT);
  digitalWrite(RS485_DIR, LOW);
  RS485.begin(4800, SERIAL_8N1, RS485_RX, RS485_TX);
  RS485.setTimeout(500);

  int state = radio.begin(RF_FREQUENCY);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa init failed: %d\n", state);
  } else {
    radio.setBandwidth(RF_BANDWIDTH);
    radio.setSpreadingFactor(RF_SPREADING);
    radio.setCodingRate(RF_CODING_RATE);
    radio.setOutputPower(RF_OUTPUT_POWER);
    Serial.println("LoRa init OK");
  }

  analogReadResolution(12);
}

// ==========================================================
// 主迴圈
// ==========================================================
void loop() {
  uint32_t now = millis();

  bool unlockOn = (digitalRead(PIN_UNLOCK) == LOW); // true = 解除防盜
  bool doorOpen = (digitalRead(PIN_MICRO) == LOW);  // true = 開門
  bool armed    = !unlockOn;                        // 未解除 = 警戒狀態

  // 防盜邏輯 1: 警戒狀態下被開箱
  if (armed && doorOpen && !tamperActive) {
    tamperActive = true;
    Serial.println("[TAMPER] Illegal open detected!");
    sendLoRa("{\"event\":\"alert_illegal_open\",\"seq\":" + String(seq++) + "}");
  }

  // 防盜邏輯 2: 解除防盜或關門時清除非法狀態
  if (tamperActive && (!doorOpen || unlockOn)) {
    tamperActive = false;
    Serial.println("[TAMPER] Cleared.");
    sendLoRa("{\"event\":\"closed_after_tamper\",\"seq\":" + String(seq++) + "}");
  }

  // 防盜邏輯 3: 正常解除後合法開箱
  if (!armed && doorOpen && !lastDoorOpen) {
    Serial.println("[NORMAL] Open box (disarmed).");
    sendLoRa("{\"event\":\"open_ok\",\"seq\":" + String(seq++) + "}");
  }

  lastDoorOpen = doorOpen;
  updateBuzzer(now);

  // 定時採樣發送
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    // 電池電壓換算（2S 鋰電，分壓校正倍率 K ≈ 15.0）
    int raw = analogRead(PIN_VBAT);
    float v_adc = raw * (3.3f / 4095.0f);
    float vbat = v_adc * 15.0f;
    float pct = (vbat - 6.0f) / (8.4f - 6.0f);
    int bat_pct = constrain((int)(pct * 100.0f + 0.5f), 0, 100);

    float soilMoist, soilTemp, soilEC, soilPH, soilN, soilP, soilK;
    int soilErr = readSoilSensor(soilMoist, soilTemp, soilEC, soilPH, soilN, soilP, soilK);

    String js = "{";
    js += "\"event\":\"periodic\",";
    js += "\"seq\":" + String(seq++) + ",";
    js += "\"vbat\":" + String(vbat, 2) + ",";
    js += "\"bat_pct\":" + String(bat_pct) + ",";

    if (!soilErr) {
      js += "\"soil_moist\":" + String(soilMoist, 1) + ",";
      js += "\"soil_temp\":"  + String(soilTemp, 1) + ",";
      js += "\"soil_ec\":"    + String(soilEC, 0)   + ",";
      js += "\"soil_ph\":"    + String(soilPH, 1)   + ",";
      js += "\"soil_n\":"     + String(soilN, 0)    + ",";
      js += "\"soil_p\":"     + String(soilP, 0)    + ",";
      js += "\"soil_k\":"     + String(soilK, 0)    + ",";
      js += "\"soil_error\":0";
    } else {
      js += "\"soil_error\":1";
    }
    js += "}";

    sendLoRa(js);
  }
}