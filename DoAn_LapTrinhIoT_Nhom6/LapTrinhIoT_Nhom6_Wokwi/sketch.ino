#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include <math.h>
#include <string.h>

constexpr uint8_t PIN_SOIL_ANALOG = 36;   // ADC1, gia lap do am dat
constexpr uint8_t PIN_RAIN_ANALOG = 34;   // ADC1, gia lap xac suat mua
constexpr uint8_t PIN_DHT = 15;
constexpr uint8_t PIN_RELAY = 18;         // CHI DUNG Relay Active HIGH
constexpr uint8_t PIN_SERVO = 19;
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;

constexpr uint32_t SENSOR_PERIOD_MS = 300;
constexpr uint32_t DHT_PERIOD_MS    = 2000;
constexpr uint32_t LCD_PERIOD_MS    = 500;
constexpr uint32_t SERIAL_LOG_PERIOD_MS = 2000;
constexpr uint32_t API_PERIOD_MS    = 30000;   // Chi phuc vu demo
constexpr uint32_t API_MAX_AGE_MS   = 900000;  // 15 phut: chinh sach minh hoa
constexpr uint16_t HTTP_TIMEOUT_MS = 1000;    // KHONG phai gioi han tong thoi gian GET

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* OPENWEATHER_KEY = "53a7ab658fe4a6316fb1f147b8393064"; // Khong public API key that

DHT dht(PIN_DHT, DHT22);
Servo valveServo;
LiquidCrystal_I2C lcd(0x27, 20, 4);

enum class RainSource : uint8_t { MANUAL, API, FALLBACK };
// De demo 5 test case bang bien tro; gui 'a' tren Serial de vao AUTO,
// gui 'm' de tro lai MANUAL.
bool manualMode = true;
bool apiValid = false;
int apiRainProb = -1;
uint32_t lastApiSuccess = 0;
uint32_t lastApiAttempt = 0;
bool apiAttempted = false;
uint32_t lastSensorAt = 0;
uint32_t lastDhtAt = 0;
uint32_t lastLcdAt = 0;
uint32_t lastSerialLogAt = 0;
bool firstDht = true;
bool firstControl = true;
bool firstLcd = true;

float temperatureC = NAN;
float humidityRH = NAN;
bool dhtValid = false;
int soilMoisture = 0;
int rainProbability = 0;
int dynamicThreshold = 60;
RainSource rainSource = RainSource::MANUAL;
const char* statusMsg = "KHOI DONG...";

void setActuators(bool watering) {
  if (watering) {
    valveServo.write(90);       // Mo van mo phong truoc khi kich bom
    digitalWrite(PIN_RELAY, HIGH);
  } else {
    digitalWrite(PIN_RELAY, LOW); // Tat bom truoc khi dong van
    valveServo.write(0);
  }
}

void printLCDRow(uint8_t row, const char* text) {
  char out[21];
  memset(out, ' ', 20);
  out[20] = '\0';
  for (uint8_t i = 0; i < 20 && text[i] != '\0'; ++i) out[i] = text[i];
  lcd.setCursor(0, row);
  lcd.print(out);
}

void readDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  dhtValid = !isnan(t) && !isnan(h) && t >= -40.0f && t <= 80.0f
             && h >= 0.0f && h <= 100.0f;
  if (dhtValid) {
    temperatureC = t;
    humidityRH = h;
  } else {
    Serial.println("[DHT] Khong doc duoc du lieu hop le; dung tuoi an toan.");
  }
}

bool hasApiKey() {
  return strcmp(OPENWEATHER_KEY, "YOUR_OPENWEATHER_API_KEY") != 0
         && strlen(OPENWEATHER_KEY) > 0;
}

// Goi HTTP DONG BO: co the block vong loop trong luc ket noi/nhan phan hoi.
void updateWeatherFromAPI() {
    // Kiem tra API Key
  if (!hasApiKey()) {
      apiValid = false;

      Serial.println("[API] LOI: Chua cau hinh API Key!");
      return;
  }

  // Kiem tra ket noi WiFi
  if (WiFi.status() != WL_CONNECTED) {
      apiValid = false;

      Serial.println("[WIFI] LOI: Chua ket noi WiFi!");
      Serial.print("[WIFI] Status: ");
      Serial.println(WiFi.status());

      Serial.println("[WIFI] Thu ket noi lai...");
      WiFi.reconnect();

      return;
  }

  // Ket noi WiFi thanh cong
  Serial.println("[WIFI] Da ket noi thanh cong!");
  Serial.println("[API] Dang truy van OpenWeatherMap...");

  String endpoint = "https://api.openweathermap.org/data/2.5/forecast?lat=10.3759"
                    "&lon=105.4185&cnt=1&appid=";
  endpoint += OPENWEATHER_KEY;
  endpoint += "&units=metric";

  // CHI de minh hoa tren Wokwi: bo qua kiem chung chung chi TLS.
  // Khi su dung thuc te PHAI xac minh chung chi CA cua dich vu.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  uint32_t started = millis();
  if (!http.begin(client, endpoint)) {
    apiValid = false;
    Serial.println("[API] Khong khoi tao duoc HTTPClient.");
    return;
  }

  int code = http.GET();
  bool success = false;
  if (code == HTTP_CODE_OK) {
    // cnt=1 giam kich thuoc phan hoi. Filter chi giu pop va dt.
    StaticJsonDocument<128> filter;
    filter["list"][0]["pop"] = true;
    filter["list"][0]["dt"] = true;
    StaticJsonDocument<768> doc;
    String payload = http.getString();
    DeserializationError error = deserializeJson(
      doc, payload, DeserializationOption::Filter(filter));
    JsonVariant value = doc["list"][0]["pop"];
    if (!error && !value.isNull()) {
      float pop = value.as<float>();
      if (!isnan(pop) && pop >= 0.0f && pop <= 1.0f) {
        apiRainProb = constrain((int)roundf(pop * 100.0f), 0, 100);
        lastApiSuccess = millis();
        success = true;
        Serial.printf("[API] pop=%.2f, rain=%d%%, dt=%ld\n",
                      pop, apiRainProb,
                      (long)(doc["list"][0]["dt"] | 0L));
      }
    }
    if (!success) {
      Serial.printf("[API] JSON loi/thieu pop: %s\n", error.c_str());
    }
  } else {
    Serial.printf("[API] HTTP status=%d\n", code);

    String errorBody = http.getString();

    Serial.println("[API] Chi tiet loi:");
    Serial.println(errorBody);
  }
  http.end();
  apiValid = success; // Neu GET loi, khong tai su dung pop cu.
  Serial.printf("[API] elapsed=%lu ms\n", (unsigned long)(millis()-started));
}

void printHelp() {
  Serial.println();
  Serial.println("==================================================");
  Serial.println("       HE THONG TUOI CAY THONG MINH - IOT");
  Serial.println("         KET HOP DU LIEU THOI TIET");
  Serial.println("==================================================");
  Serial.println(" HUONG DAN DEMO:");
  Serial.println("  [m] MANUAL : Xoay bien tro GPIO34 gia lap mua");
  Serial.println("  [a] AUTO   : Lay du bao tu OpenWeatherMap API");
  Serial.println("               API loi -> FALLBACK bang bien tro");
  Serial.println("  [h] HELP   : In lai bang huong dan");
  Serial.println("--------------------------------------------------");
  Serial.println(" LUAT TUOI:");
  Serial.println("  Soil < 60%              : Can xem xet tuoi");
  Serial.println("  T > 35C hoac Hkk < 30% : Nang nguong len 72%");
  Serial.println("  Rain > 70%              : Hoan tuoi");
  Serial.println("  T >= 50C               : Canh bao, dung bom");
  Serial.println("--------------------------------------------------");
  Serial.println(" Che do luc khoi dong: MANUAL");
  Serial.println(" Nhap m, a hoac h vao o Serial Monitor -> Enter");
  Serial.println("==================================================");
  Serial.println();
}

void handleSerialMode() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == 'm' || ch == 'M') {
      manualMode = true;
      Serial.println("[MODE] MANUAL - mua tu GPIO34");
    } else if (ch == 'a' || ch == 'A') {
      manualMode = false;
      apiValid = false;
      apiAttempted = false; // Thu API o vong loop tiep theo
      Serial.println("[MODE] AUTO - API neu hop le, neu khong FALLBACK");
      if (!hasApiKey()) Serial.println("[API] Chua co API key; se dung FALLBACK.");
    } else if (ch == 'h' || ch == 'H') {
      printHelp();
    }
  }
}

void chooseRainSource(uint32_t now) {
  int analogRain = constrain(map(analogRead(PIN_RAIN_ANALOG), 0, 4095, 0, 100), 0, 100);
  if (manualMode) {
    rainSource = RainSource::MANUAL;
    rainProbability = analogRain;
    return;
  }
  bool apiFresh = apiValid && WiFi.status() == WL_CONNECTED
      && ((uint32_t)(now - lastApiSuccess) <= API_MAX_AGE_MS);
  if (apiFresh && apiRainProb >= 0 && apiRainProb <= 100) {
    rainSource = RainSource::API;
    rainProbability = apiRainProb;
  } else {
    // FALLBACK nay chi gia lap duoc khi bien tro 2 thuc su dang duoc noi.
    // Khong co co che phat hien bien tro dut day trong ma demo.
    rainSource = RainSource::FALLBACK;
    rainProbability = analogRain;
  }
}

void updateControl(uint32_t now) {
  soilMoisture = constrain(map(analogRead(PIN_SOIL_ANALOG), 0, 4095, 100, 0), 0, 100);
  chooseRainSource(now);
  dynamicThreshold = (dhtValid &&
      (temperatureC > 35.0f || humidityRH < 30.0f)) ? 72 : 60;

  if (!dhtValid) {
    setActuators(false);
    statusMsg = "LOI CAM BIEN DHT22";
  } else if (temperatureC >= 50.0f) {
    setActuators(false);
    statusMsg = "CANH BAO: SOC NHIET!";
  } else if (soilMoisture >= dynamicThreshold) {
    setActuators(false);
    statusMsg = "BOM: TAT (Du am)";
  } else if (rainProbability > 70) {
    setActuators(false);
    statusMsg = "HOAN: Sap co mua";
  } else {
    setActuators(true);
    statusMsg = dynamicThreshold == 72 ? "BOM: BAT (Nang hot)" : "BOM: BAT (Dang tuoi)";
  }
}

void printStatus() {
  const char* src = rainSource == RainSource::API ? "API" :
                    (rainSource == RainSource::MANUAL ? "MANUAL" : "FALLBACK");
  Serial.printf("[CTRL] Soil=%d%% T=%.1fC H=%.1f%% Rain=%d%% SRC=%s TH=%d | %s\n",
                soilMoisture, temperatureC, humidityRH, rainProbability,
                src, dynamicThreshold, statusMsg);
}

void displayLCD() {
  char text[48];
  snprintf(text, sizeof(text), "Do am dat: %d%%", soilMoisture);
  printLCDRow(0, text);
  snprintf(text, sizeof(text), "Du bao mua: %d%%", rainProbability);
  printLCDRow(1, text);
  if (dhtValid) {
    snprintf(text, sizeof(text), "KK: %.1fC - %.0f%%RH", temperatureC, humidityRH);
  } else {
    snprintf(text, sizeof(text), "KK: LOI DHT22");
  }
  printLCDRow(2, text);
  printLCDRow(3, statusMsg);
}

void setup() {
  Serial.begin(115200);
  delay(150); // Cho mot lan o setup de Serial khoi tao, khong anh huong loop.
  Serial.println("[BOOT] ESP32 dang khoi dong...");
  printHelp();
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  analogReadResolution(12);
  dht.begin();
  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  valveServo.setPeriodHertz(50);
  valveServo.attach(PIN_SERVO, 500, 2400);
  setActuators(false);
  printLCDRow(0, "HE THONG TUOI IOT");
  printLCDRow(1, "Dang khoi dong...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS); // KHONG cho WiFi ket noi de tranh block setup
  Serial.println("[BOOT] Khoi tao xong. He thong bat dau o MANUAL.");
}

void loop() {
  handleSerialMode();
  uint32_t now = millis();
  if (firstDht || (uint32_t)(now - lastDhtAt) >= DHT_PERIOD_MS) {
    firstDht = false;
    lastDhtAt = now;
    readDHT();
  }
  if (firstControl || (uint32_t)(now - lastSensorAt) >= SENSOR_PERIOD_MS) {
    firstControl = false;
    lastSensorAt = now;
    updateControl(now);
  }
  if (firstLcd || (uint32_t)(now - lastLcdAt) >= LCD_PERIOD_MS) {
    firstLcd = false;
    lastLcdAt = now;
    displayLCD();
  }
  if ((uint32_t)(now - lastSerialLogAt) >= SERIAL_LOG_PERIOD_MS) {
    lastSerialLogAt = now;
    printStatus();
  }
  if (!manualMode &&
      (!apiAttempted || (uint32_t)(now - lastApiAttempt) >= API_PERIOD_MS)) {
    lastApiAttempt = now;
    apiAttempted = true;
    updateWeatherFromAPI(); // CAN block; khong xem la xu ly phi dong bo
  }
}