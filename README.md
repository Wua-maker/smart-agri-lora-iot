# 台灣智慧農業開源專案 (Open Smart Agri Taiwan)

> **低成本、免 SIM 卡、長續航野外環境監測節點與 IoT 閘道器**  
> A cost-effective, SIM-less, long-battery-life open-source agricultural IoT system.

---

## 專案緣起與願景

台灣農業面臨高齡化、極端氣候以及山區訊號覆蓋不足等挑戰。目前市售的商業智慧農業設備動輒數萬至數十萬元，且架構封閉、按月收費，維護成本極高。

**農業智慧化不是要取代人，而是在相同的耕作過程中，讓每一位務農者更專業、更有底氣。**  
本專案全數開源，旨在以百元至千元級的硬體成本，打造在深山「無電、無網路」環境下依然能穩定運行的農業監測與防盜系統。

---

## 系統架構特色

* **穿透山林・免 SIM 卡**：採用 SX1262 LoRa 射頻技術（台灣 NCC 合規 923.2 MHz 頻段），實現數公里免月租、抗干擾遠距傳輸。
* **工業級感知**：採用 RS485 Modbus-RTU 協定讀取土壤七合一感測器（水分、溫度、EC、pH、氮、磷、鉀），避免一般類比感測針短期氧化腐蝕。
* **低功耗與離網供電**：ESP32 深度睡眠（Deep Sleep）結合 18650 / 2S 鋰電池與太陽能供電，達成野外自給自足。
* **實體防盜系統**：整合微動開關與隱藏解鎖開關，非法撬開時觸發本地蜂鳴警報並發送 LoRa 警報封包。
* **雙模式接收閘道器**：RX 端支援 1.3 吋 OLED 即時現地輪播顯示，並透過 TLS (Port 8883) 轉發數據至 HiveMQ 雲端。

---

## 目錄結構

```text
Smart-Agri-IoT/
├── TX_Node/
│   └── TX_Node.ino         # 發射端韌體 (ESP32 + LoRa + RS485 + 防盜)
├── RX_Gateway/
│   ├── RX_Gateway.ino      # 接收端閘道器 (ESP32 + LoRa + WiFi + MQTT + OLED)
│   ├── config.example.h    # 憑證設定範本
│   └── config.h            # 個人憑證 (本機使用，已被 .gitignore 排除)
├── images/                 # 硬體接線與架構圖片
├── .gitignore              # Git 排除清單 (確保安全憑證不外洩)
└── README.md               # 專案說明文件

硬體物料清單 (BOM)
1. 發射節點 (TX Node)
主控板：ESP32 開發板 (NodeMCU-32S / ESP32-WROOM-32)

射頻模組：SX1262 LoRa 模組 (920–925 MHz) + 膠棒天線

通訊轉接：MAX485 / SP3485 TTL 轉 RS485 模組

土壤感測：工業級 RS485 Modbus-RTU 土壤七合一探針

防盜與安全：微動開關 (開門偵測)、隱藏式開關 (解除防盜)、主動式蜂鳴器

電源：2S 鋰電池組 (或 18650 雙串保護板) + 電壓分壓檢測模組 (連接 GPIO 34)

外殼機構：IP65 戶外防水盒 + 防水電纜接頭 (Cable Gland)

2. 接收閘道器 (RX Gateway)
主控板：ESP32 開發板

射頻模組：SX1262 LoRa 模組 + 天線

顯示螢幕：1.3 吋 I2C OLED (SH1106 驅動，128x64)

供電：標準 5V USB 電源供應器

硬體接線定義
TX 發射端
SX1262 LoRa：

VCC -> 3.3V (⚠️ 嚴禁接 5V) | GND -> GND

NSS -> GPIO 5 | DIO1 -> GPIO 26 | RST -> GPIO 14 | BUSY -> GPIO 25

MOSI -> GPIO 23 | MISO -> GPIO 19 | SCK -> GPIO 18

MAX485 (RS485)：

RO -> GPIO 4 (RX) | DI -> GPIO 2 (TX) | DE+RE 短接 -> GPIO 17 (DIR)

防盜與周邊：

微動開關 -> GPIO 27 (導通接地) | 隱藏開關 -> GPIO 15 (導通接地)

蜂鳴器 -> GPIO 13 | 電池電壓 ADC -> GPIO 34

RX 接收端
SX1262 LoRa：腳位與 TX 相同 (GPIO 5, 26, 14, 25, 23, 19, 18)

1.3" OLED (SH1106)：

VCC -> 3.3V | GND -> GND | SDA -> GPIO 21 | SCL -> GPIO 22

系統接線示意圖
快速開始 (Quick Start)
1. 準備編譯環境
安裝 Arduino IDE 或 PlatformIO。

於 Library Manager 安裝以下函式庫：

RadioLib

PubSubClient

ArduinoJson (v6.x)

U8g2

2. 配置接收端憑證
進入 RX_Gateway/ 目錄。

將 config.example.h 複製一份並重新命名為 config.h。

打開 config.h 填入你的連線資訊：

C++


const char* WIFI_SSID  = "你的WiFi名稱";
const char* WIFI_PSK   = "你的WiFi密碼";
const char* MQTT_HOST  = "你的Broker網址";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER  = "使用者帳號";
const char* MQTT_PASS  = "密碼";
const char* MQTT_TOPIC = "agriculture/sensor/01";
3. 燒錄與部署
分別連接 TX 與 RX 開發板進行編譯並燒錄韌體。

啟動 RX 端，OLED 螢幕將顯示開機連線狀態。

啟動 TX 端，感測器將開始採樣並每 10 秒發送一次 JSON 封包。

免責聲明 (Disclaimer)
本專案之電路圖、韌體程式碼與架構設計僅供學術研究、教育及個人 DIY 學習使用。使用者於戶外實際場域部署（包含鋰電池裝配、高溫日曬、防水施作及農用電氣改裝）時，請務必遵循電氣安全規範並自行評估風險。作者不承擔任何因組裝不當、環境因素或設備損壞衍生之直接或連帶責任。

開源授權
本專案採 MIT License 授權釋出，歡迎社群自由 Fork、修改、延伸與實測反饋。