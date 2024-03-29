/******************************************************************************
main.cpp
ESP32 based incineration toilet controller, thermocouple type K, MAX31855
Leonardo Bispo
March, 2022
https://github.com/ldab/esp32_incineration_toilet
Distributed as-is; no warranty is given.
******************************************************************************/

#include "ArduinoJson.h"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
}

#include "ArduinoOTA.h"
#include <Arduino.h>
#include <WiFi.h>

#include <AsyncMqttClient.h>

#include "esp_system.h"
#include <Ticker.h>
#include <pthread.h>

#include <DNSServer.h>
#include <ESPmDNS.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "SPIFFS.h"

// PapertrailLogger
#include "PapertrailLogger.h"
#include <WiFiUdp.h>

// MAX31855
#include <SPI.h>
#include <Wire.h>

#include "Adafruit_MAX31855.h"
#include "LCD16x2.h"

#include "time.h"

#include "html_strings.h"

#define PAPERTRAIL_HOST "logs2.papertrailapp.com"
#define PAPERTRAIL_PORT 53139

#ifdef VERBOSE
#define DBG(msg, ...)                                                          \
  {                                                                            \
    Serial.printf("[%lu] " msg, millis(), ##__VA_ARGS__);                      \
    Serial.flush();                                                            \
  }
#else
#define DBG(...)
#endif

#define NOTIFY(msg, ...)                                                       \
  {                                                                            \
    char _msg[64] = "";                                                        \
    sprintf(_msg, msg, ##__VA_ARGS__);                                         \
    Blynk.logEvent("alarm", _msg);                                             \
    DBG("%s\n", _msg);                                                         \
  }

// https://randomnerdtutorials.com/esp8266-pinout-reference-gpios/
#define RELAY        16
#define FAN          17
#define LED_R        25
#define LED_G        26
#define LED_B        27
#define SPI_CS       22
#define SPI_MISO     21
#define SPI_CLK      23
#define I2C_SDA      12
#define I2C_SCL      14
#define LCD_RST      33 // WROOM pin9

#define COSTKWH      2.5

#define BIG_FLUSH    30
#define SMALL_FLUSH  15
#define FINAL_TEMP   550

#define RATEUPDATE   60 // every 60 seconds

#define DIFFERENTIAL 5 // degC

const char *p_mqtt   = "/mqtt.txt";
char mqtt_user[64]   = {'\0'};
char mqtt_pass[64]   = {'\0'};
char mqtt_server[64] = {'\0'};
uint16_t mqtt_port   = 1883;

String ssid, pass, hostname;

float temp;
float tInt;
float currentSetpoint = -9999;
std::vector<float> readings;
std::vector<long> epocTime;
volatile float current;
volatile uint32_t instPower;
volatile uint32_t energy = 0;
volatile uint32_t pulseInterval;
volatile uint8_t button        = 0;

// Control variables
volatile uint32_t energyMillis = 0;
uint32_t initMillis            = 0;
uint32_t holdMillis            = 0;
int step                       = 0;

String info                    = "Idle 💤";

// Timer instance numbers
Ticker controlTimer;
Ticker safetyTimer;
Ticker rampTimer;
Ticker slowCool;
Ticker tempTimer;
Ticker sendTimer;
Ticker buttonTimer;
Ticker restart;

DNSServer dnsServer;

AsyncWebServer server(80);
AsyncEventSource events("/events"); // event source (Server-Sent events)
AsyncWebSocket ws("/ws");           // access at ws://[esp ip]/ws

Adafruit_MAX31855 thermocouple(SPI_CLK, SPI_CS, SPI_MISO);

PapertrailLogger *errorLog;

AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;

LCD16x2 lcd;

void printSegments();
void rampRate();
void tControl();
void getTemp();
void lcdMenu();
String processor(const String &var);
String readFile(fs::FS &fs, const char *path);
void writeFile(fs::FS &fs, const char *path, const char *message);

typedef enum {
  RED,
  GREEN,
  BLUE,
  YELLOW,
  WHITE,
  PURPLE,
  CYAN,
} led_color_t;

class CaptiveRequestHandler : public AsyncWebHandler
{
  public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}

  bool canHandle(AsyncWebServerRequest *request)
  {
    // request->addInterestingHeader("ANY");
    return true;
  }

  void handleRequest(AsyncWebServerRequest *request)
  {
    request->send_P(200, "text/html", HTTP_CONFIG, processor);
  }
};

void espRestart() { ESP.restart(); }

void ledOff()
{
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

void led(led_color_t color)
{
  ledOff();
  switch (color) {
  case RED:
    digitalWrite(LED_R, LOW);
    break;
  case GREEN:
    digitalWrite(LED_G, LOW);
    break;
  case BLUE:
    digitalWrite(LED_B, LOW);
    break;
  case YELLOW:
    digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, LOW);
    break;
  case WHITE:
    digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, LOW);
    digitalWrite(LED_B, LOW);
    break;
  case PURPLE:
    digitalWrite(LED_R, LOW);
    digitalWrite(LED_B, LOW);
    break;
  case CYAN:
    digitalWrite(LED_G, LOW);
    digitalWrite(LED_B, LOW);
    break;

  default:
    break;
  }
}

// Send notification to HA, max 32 bytes
void notify(char *msg, size_t length)
{
  char _msg[128];
  sprintf(_msg, "%s - %s", hostname.c_str(), msg);
  DBG("%s\n", _msg);

  char topic[64] = {'\0'};
  sprintf(topic, "%s/f/notify", mqtt_user);
  mqttClient.publish(topic, 0, false, _msg, strlen(_msg));
}

void onUpload(AsyncWebServerRequest *request, String filename, size_t index,
              uint8_t *data, size_t len, bool final)
{
  if (!index) {
    DBG("Update Start: %s\n", filename.c_str());
    led(CYAN);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  }
  DBG("Progress: %u of %u\r", Update.progress(), Update.size());
  if (!Update.hasError()) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
    }
  }
  if (final) {
    if (Update.end(true)) {
      DBG("Update Success: %uB\n", index + len);
      request->redirect("/");
      restart.once_ms(1000, espRestart);
    } else {
      Update.printError(Serial);
    }
  }
}

void onRequest(AsyncWebServerRequest *request)
{
  // Handle Unknown Request
  request->send(404, "text/plain", "OUCH");
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  // Handle WebSocket event
}

String processor(const String &var)
{
  if (var == "CSS_TEMPLATE")
    return FPSTR(HTTP_STYLE);
  if (var == "INDEX_JS")
    return FPSTR(HTTP_JS);
  if (var == "HTML_HEAD_TITLE")
    return FPSTR(HTML_HEAD_TITLE);
  if (var == "HTML_INFO_BOX") {
    String ret = "";
    if (WiFi.isConnected()) {
      ret = "<strong> Connected</ strong> to ";
      ret += WiFi.SSID();
      ret += "<br><em><small> with IP ";
      ret += WiFi.localIP().toString();
      ret += "</small>";
    } else
      ret = "<strong> Not Connected</ strong>";
    return ret;
  }
  if (var == "UPTIME") {
    String ret = String(millis() / 1000 / 60);
    ret += " min ";
    ret += String((millis() / 1000) % 60);
    ret += " sec";
    return ret;
  }
  if (var == "CHIP_ID") {
    String ret = String((uint32_t)ESP.getEfuseMac());
    return ret;
  }
  if (var == "FREE_HEAP") {
    String ret = String(ESP.getFreeHeap());
    ret += " bytes";
    return ret;
  }
  if (var == "SKETCH_INFO") {
    //%USED_BYTES% / &FLASH_SIZE&<br><progress value="%USED_BYTES%"
    // max="&FLASH_SIZE&">
    String ret = String(ESP.getSketchSize());
    ret += " / ";
    ret += String(ESP.getFlashChipSize());
    ret += "<br><progress value=\"";
    ret += String(ESP.getSketchSize());
    ret += "\" max=\"";
    ret += String(ESP.getFlashChipSize());
    ret += "\">";
    return ret;
  }
  if (var == "HOSTNAME")
    return String(WiFi.getHostname());
  if (var == "MY_MAC")
    return WiFi.macAddress();
  if (var == "MY_RSSI")
    return String(WiFi.RSSI());
  if (var == "FW_VER")
    return String(FIRMWARE_VERSION);
  if (var == "SDK_VER")
    return String(ESP_ARDUINO_VERSION_MAJOR) + "." +
           String(ESP_ARDUINO_VERSION_MINOR) + "." +
           String(ESP_ARDUINO_VERSION_PATCH);
  if (var == "ABOUT_DATE") {
    String ret = String(__DATE__) + " " + String(__TIME__);
    return ret;
  }
  if (var == "GRAPH_DATA" && readings.size()) {
    String graphString;
    graphString.reserve(readings.size() * 2);
    graphString = "[";
    for (size_t i = 0; i < readings.size() - 1; i++) {
      graphString += "[";
      graphString += String(epocTime[i]);
      graphString += ",";
      graphString += String(readings[i], 0);
      graphString += "]";
      graphString += ",";
    }
    graphString += "[";
    graphString += String(epocTime[readings.size() - 1]);
    graphString += ",";
    graphString += String(readings[readings.size() - 1], 0);
    graphString += "]";
    graphString += "]";
    return graphString;
  }

  return String();
}

void captiveServer()
{
  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
    int params = request->params();
    StaticJsonDocument<384> doc;
    char output[384] = {'\0'};
    for (int i = 0; i < params; i++) {
      AsyncWebParameter *p = request->getParam(i);
      if (p->isPost()) {
        // HTTP POST ssid value
        if (p->name() == "ssid") {
          ssid = p->value().c_str();
          DBG("SSID set to: %s\n", ssid.c_str());
        }
        if (p->name() == "pass") {
          pass = p->value().c_str();
          DBG("Password set to: %s\n", pass.c_str());
        }
        if (p->name() == "hostname") {
          doc["hostname"] = p->value().c_str();
        }
        if (p->name() == "server") {
          doc["s"] = p->value().c_str();
        }
        if (p->name() == "mqtt_pass") {
          doc["pass"] = p->value().c_str();
        }
        if (p->name() == "user") {
          doc["u"] = p->value().c_str();
        }
        if (p->name() == "port") {
          doc["port"] = p->value().c_str();
        }
      }
    }
    serializeJson(doc, output);
    DBG("%s\n", output);
    writeFile(SPIFFS, p_mqtt, output);

    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    DBG("Connecting to WiFi ..");
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print('.');
      delay(100);
    }
    DBG("Connected\n");
    restart.once_ms(1000, espRestart);
    request->redirect("http://" + WiFi.localIP().toString());
  });
}

// Read File from SPIFFS
String readFile(fs::FS &fs, const char *path)
{
  DBG("Reading file: %s\r\n", path);

  File file = fs.open(path);
  if (!file || file.isDirectory()) {
    DBG("- failed to open file for reading\n");
    return String();
  }

  String fileContent;
  while (file.available()) {
    fileContent = file.readStringUntil('\n');
    DBG("Read: %s\n", fileContent.c_str());
    break;
  }
  return fileContent;
}

// Write file to SPIFFS
void writeFile(fs::FS &fs, const char *path, const char *message)
{
  DBG("Writing file: %s\r\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    DBG("- failed to open file for writing\n");
    return;
  }
  if (file.print(message)) {
    DBG("- file written\n");
  } else {
    DBG("- frite failed\n");
  }
}

// temperature, rate, hold/soak (min)
int segments[4][3]     = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
const char *p_segments = "/segments.txt";

void onFire(String input)
{
  StaticJsonDocument<384> doc;
  deserializeJson(doc, input);

  segments[0][0] = doc["preheat"]["st"];
  segments[0][1] = doc["preheat"]["r"];
  segments[0][2] = doc["preheat"]["h"];
  segments[1][0] = doc["step1"]["st"];
  segments[1][1] = doc["step1"]["r"];
  segments[1][2] = doc["step1"]["h"];
  segments[2][0] = doc["step2"]["st"];
  segments[2][1] = doc["step2"]["r"];
  segments[2][2] = doc["step2"]["h"];
  segments[3][0] = doc["final"]["st"];
  segments[3][1] = doc["final"]["r"];
  segments[3][2] = doc["final"]["h"];

  getTemp();

  // guess the step
  // TODO publish retain and account for hold
  if (temp > segments[3][0])
    step = 3;
  else if (temp > segments[1][0])
    step = 3;
  else if (temp > segments[1][0])
    step = 2;
  else if (temp > segments[0][0])
    step = 1;

  // empty parameters (toilet)
  if (segments[3][0] == 0)
    step = 0;

  info             = "Flushing 🔥 @" + String(segments[step][0]) + " C";
  initMillis       = millis();
  currentSetpoint  = temp;

  String flushType = segments[0][2] > 15 ? "Poo" : "Pee";

  lcd.lcdClear();
  lcd.lcdGoToXY(1, 1);
  lcd.lcdWrite(
      (char *)("Flushing: " + String(segments[step][0]) + " C " + flushType)
          .c_str());
  lcdMenu();

  printSegments();
  rampRate();
  digitalWrite(FAN, HIGH);
  controlTimer.attach_ms(5530L, tControl);
  rampTimer.attach_ms(RATEUPDATE * 1000L, rampRate);
}

void sendData()
{
  StaticJsonDocument<192> doc;
  char payload[192];

  current          = (instPower / 1000.0f) / 230.0f;

  JsonObject feeds = doc.createNestedObject("feeds");
  feeds["T"]       = temp;
  feeds["I"]       = current;
  feeds["P"]       = instPower / 1000.0f;
  feeds["E"]       = energy;
  feeds["$"]       = energy / 1000.0f * COSTKWH;
  feeds["Tint"]    = tInt;
  feeds["St"]      = currentSetpoint;
  feeds["Step"]    = step;
  feeds["RSSI"]    = WiFi.RSSI();

  serializeJson(doc, payload);

  char topic[64] = {'\0'};
  sprintf(topic, "%s/g/%s/json", mqtt_user, hostname.c_str());

  mqttClient.publish(topic, 0, false, payload, strlen(payload));

  DBG("topic: %s\n", topic);
  DBG("Publish: %s\n", payload);

  current   = 0;
  instPower = 0;
}

void safetyCheck()
{
  static bool tIntError  = false;
  static bool noRlyError = false;
  static bool highTError = false;

  // if (controlTimer.active()) {
  //   if (temp > (currentSetpoint + 10)) // TODO check differential
  //   {
  //     DBG("HIGH TEMPERATURE ALARM\n");
  //   } else if (temp < (currentSetpoint - 20)) {
  //     DBG("LOW TEMPERATURE ALARM\n");
  //   }
  // }

  if (tInt > 60) {
    if (!tIntError) {
      char tIntChar[32];
      sprintf(tIntChar, "High internal temp: %.1f°C", tInt);
      notify(tIntChar, strlen(tIntChar));
      tIntError = true;
    }
  } else {
    tIntError = false;
  }

  if (temp > 570) {
    if (!highTError) {
      char highTchar[32];
      sprintf(highTchar, "High temp: %.1f°C", temp);
      notify(highTchar, strlen(highTchar));
      highTError = true;
    }
  } else {
    highTError = false;
  }

  uint32_t elapsedTime = millis() - initMillis;

  if (elapsedTime > 60 * 1000 * 1000 && step == 0 && rampTimer.active()) {
    char msg[] = "Too long to heat, check element/thermostat";
    notify(msg, strlen(msg));
    step = 4;
  }

  // TODO long time for cooling == fan problem

  if (temp < 100 && step == 5)
    restart.once_ms(1000, espRestart);
}

void printSegments()
{
  DBG("Firing ");
  for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++) {
    Serial.print("{");
    for (size_t j = 0; j < sizeof(segments[0]) / (sizeof(int)); j++) {
      Serial.printf("%d,", segments[i][j]);
    }
    Serial.print("} ");
  }
  Serial.print("\n");
}

void IRAM_ATTR readPower()
{
  if (energy == 0) {
    // We don't know the time difference between pulse, reset
    // TODO get from cloud in case MCU reset
    energy       = 1;
    current      = 0;
    instPower    = 0;

    energyMillis = millis();
  } else {
    pulseInterval = millis() - energyMillis;

    if (pulseInterval < 100) { // 10*sqrt(2) Amps =~ 1106.8ms
      // DBG("DEBOUNCE");
      return;
    }

    energy++;                         // each pulse is 0,5Wh
    instPower = 7200 * pulseInterval; // 1Wh = 3600J calculate in mW avoid float

    energyMillis = millis();
  }

  volatile static bool problem = false;
  if (digitalRead(RELAY)) {
    if (problem == true) {
      // notify((char *)"shortError", strlen("shortError"));
    }
    problem = true;
  } else
    problem = false;
}

void getTemp()
{
  static float _t   = 0;
  static uint8_t _s = 0;
  static bool tErr  = false;

  temp              = thermocouple.readCelsius();
  tInt              = thermocouple.readInternal();
  uint8_t error     = thermocouple.readError();

  // average 5x samples
  _t += temp;
  _s++;
  if (_s == 4) {
    temp = (float)(_t / _s);
    _t   = 0;
    _s   = 0;
  }

  // Ignore SCG fault
  // https://forums.adafruit.com/viewtopic.php?f=31&t=169135#p827564
  if (error & 0b001) {
    if (!tErr) {
      temp = NAN;
      tErr = true;

      char tcError[24];
      sprintf(tcError, "Thermocouple error #%i", error);
      notify(tcError, strlen(tcError));

      digitalWrite(RELAY, LOW);
    }
  } else {
    static uint32_t log = millis();
    tErr                = false;
    char msg[8];
    sprintf(msg, "%.01f", temp);

    struct tm timeinfo;
    if ((readings.size() == 0) && getLocalTime(&timeinfo)) {
      time_t epoc = mktime(&timeinfo);
      epocTime.push_back((long)epoc);
      readings.push_back(temp);
      DBG("strlen: %u\n", readings.size());
      log = millis();
    } else if (((millis() - log) > (60 * 1000) && controlTimer.active()) &&
               getLocalTime(&timeinfo)) {
      time_t epoc = mktime(&timeinfo);
      epocTime.push_back((long)epoc);
      readings.push_back(temp);
      DBG("strlen: %u\n", readings.size());
      log = millis();
    }

    char instPowerString[8];
    sprintf(instPowerString, "%.01f", instPower / 1000.0f);
    events.send(msg, "temperature");
    events.send(instPowerString, "KW");
    DBG("T: %sdegC P: %sW\n", msg, instPowerString);
  }
}

void holdTimer(uint32_t _segment)
{
  if (holdMillis == 0) {
    DBG("Start hold for %dmin\n", _segment);
    holdMillis = millis();
    rampTimer.detach();

    lcd.lcdClear();
    lcd.lcdGoToXY(1, 1);
    lcd.lcdWrite((char *)("Burning: " + String(_segment) + "min").c_str());
    lcdMenu();
  }

  uint32_t _elapsed = (millis() - holdMillis) / (60 * 1000);
  info = "Hold: " + String(currentSetpoint, 0) + "°C-" + String(_elapsed) +
         "/" + String(_segment) + "min";
  events.send(info.c_str(), "display");

  if (_elapsed >= _segment) {
    step++;
    holdMillis = 0;
    rampTimer.attach_ms(RATEUPDATE * 1000L, rampRate);
    DBG("Done with hold, step: %d\n", step);
    info = "Flushing 🔥 @" + String(segments[step][0]) + "°C";
    events.send(info.c_str(), "display");
  }
}

void rampDown()
{
  // https://digitalfire.com/schedule/04dsdh
  if (currentSetpoint < 760 || step == 5) {
    controlTimer.detach();
    slowCool.detach();
    currentSetpoint = 0;
    tControl();
    writeFile(SPIFFS, p_segments, "");
    info = "Cooling ❄️";

    lcd.lcdClear();
    lcd.lcdGoToXY(1, 1);
    lcd.lcdWrite((char *)"Cooling");
    lcdMenu();

    events.send(info.c_str(), "display");
  }

  currentSetpoint -= (float)(83.0 / (3600.0f / RATEUPDATE));
}

void tControl()
{
  DBG("Control ST: %.01fdegC, step: %d\n", currentSetpoint, step);
  static uint8_t diff;

  if (!isnan(temp)) {
    float delta_t = currentSetpoint - temp - diff;
    if (delta_t >= 0) {
      if (!digitalRead(RELAY)) {
        digitalWrite(RELAY, HIGH);
        // restart timer so relay have time to pulse
        safetyTimer.detach();
        safetyTimer.attach_ms(2115L, safetyCheck);
        diff = 0;
      }
    } else if (digitalRead(RELAY)) {
      digitalWrite(RELAY, LOW);
      diff = DIFFERENTIAL;
    }
    if (step == 5)
      return;
    if (temp > segments[step][0]) {
      currentSetpoint = segments[step][0];

      if (segments[step][2] == -1)
        segments[step][2] = 0;
      holdTimer(segments[step][2]);

      if (step == 4) {
        uint8_t h = (millis() - initMillis) / (1000 * 3600);
        uint8_t m = ((millis() - initMillis) - (h * 3600 * 1000)) / (60 * 1000);
        char endInfo[64];
        sprintf(endInfo, "Reached Temp, after: %d:%d", h, m);
        DBG("%s", endInfo);
        info = "Slow Cooling ❄️";
        events.send(info.c_str(), "display");
        slowCool.attach_ms(RATEUPDATE * 1000L, rampDown);
        rampTimer.detach();
        step++;
      }
    }
  }
}

void rampRate()
{
  // http://www.stoneware.net/stoneware/glasyrer/firing.htm

  if (currentSetpoint == -9999)
    currentSetpoint = temp;

  if (currentSetpoint >= segments[step][0]) {
    currentSetpoint = segments[step][0];
  } else {
    currentSetpoint += (float)(segments[step][1] / (3600.0f / RATEUPDATE));
  }

  DBG("Current Setpoint: %.02fdegC, step: %d\n", currentSetpoint, step);
}

void readButton()
{
  uint8_t buttons = lcd.readButtons();

  if (!(buttons & 0x01)) {
    button = 1; // onFire()
    onFire("{\"preheat\":{\"st\":550,\"r\":550,\"h\":15}}");
  }

  else if (!(buttons & 0x02)) {
    button = 2; // onFire()
    onFire("{\"preheat\":{\"st\":550,\"r\":550,\"h\":35}}");
  }

  else if (!(buttons & 0x04)) {
    button = 3; // FAN
    digitalWrite(FAN, HIGH);
    restart.once_ms(15 * 60 * 1000, espRestart);
  }

  else if (!(buttons & 0x08)) {
    button = 4; // Cancel
    restart.once_ms(1000, espRestart);
  }

  else
    button = 0;
}

void pinInit()
{
  pinMode(FAN, OUTPUT);
  digitalWrite(FAN, LOW);

  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, LOW);

  pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_RST, HIGH);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  ledOff();
}

void lcdMenu()
{
  lcd.lcdGoToXY(1, 2);
  lcd.lcdWrite((char *)"Pee");

  lcd.lcdGoToXY(5, 2);
  lcd.lcdWrite((char *)"Poo");

  lcd.lcdGoToXY(10, 2);
  lcd.lcdWrite((char *)"Fan");
}

void lcdInit()
{
  digitalWrite(LCD_RST, LOW);
  delay(500);
  digitalWrite(LCD_RST, HIGH);

  Wire.begin(I2C_SDA, I2C_SCL);

  uint16_t lcdID = lcd.getID();
  DBG("getID(): 0x%02X\n", lcdID);

  lcd.lcdClear();
  lcd.lcdSetBlacklight(128);

  lcd.lcdGoToXY(1, 1);
  lcd.lcdWrite((char *)"Standby");

  lcdMenu();
}

void onMqttConnect(bool sessionPresent) { DBG("Connected to MQTT.\n"); }

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  DBG("Disconnected from MQTT, reason: %u\n", (uint8_t)reason);

  if (WiFi.isConnected()) {
    xTimerStart(mqttReconnectTimer, 0);
  }
}

void connectToMqtt()
{
  DBG("Connecting to MQTT...\n");
  mqttClient.connect();
}

void WiFiEvent(WiFiEvent_t event)
{
  DBG("[WiFi-event] event: %d\n", event);
  switch (event) {
  case SYSTEM_EVENT_STA_GOT_IP:
    // connectToMqtt();
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    xTimerStop(mqttReconnectTimer,
               0); // don't reconnect to MQTT while reconnecting WiFi
    break;
  default:
    break;
  }
}

void otaInit()
{
  ArduinoOTA.setHostname("kiln");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    DBG("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    DBG("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();
}

void setup()
{
#ifdef VERBOSE
  Serial.begin(115200);
  DBG("VERSION %s\n", FIRMWARE_VERSION);
#endif

  // 1440 samples, every 1 min = 24 hours
  readings.reserve(1440);
  epocTime.reserve(1440);

#ifdef CALIBRATE
  // Measure GPIO in order to determine Vref to gpio 25 or 26 or 27
  adc2_vref_to_gpio(GPIO_NUM_25);
  delay(5000);
  abort();
#endif

  pinInit();
  lcdInit();
  led(RED);

  mqttReconnectTimer =
      xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void *)0,
                   reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));

  WiFi.mode(WIFI_STA);
  WiFi.onEvent(WiFiEvent);
  WiFi.begin();

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    DBG("An Error has occurred while mounting SPIFFS\n");
    return;
  }

  // This function does not return so not true if sensor is faulty
  if (!thermocouple.begin()) {
    DBG("ERROR.\n");
  } else
    DBG("MAX31855 Good\n");

  if (WiFi.waitForConnectResult() == WL_DISCONNECTED ||
      WiFi.waitForConnectResult() == WL_NO_SSID_AVAIL) { //~ 100 * 100ms
    DBG("WiFi Failed!: %u\n", WiFi.status());

    captiveServer();

    WiFi.softAP("myToilet");

    server.onNotFound(
        [](AsyncWebServerRequest *request) { request->redirect("/"); });
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());

    DBG("Start Captive Portal at: %s\n", WiFi.softAPIP().toString().c_str());

    server.addHandler(new CaptiveRequestHandler())
        .setFilter(ON_AP_FILTER); // only when requested from AP
  } else {
    DBG("WiFi Connected, IP: %s\n", WiFi.localIP().toString().c_str());

    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "0.pool.ntp.org",
                 "1.pool.ntp.org");

    char input[256] = {'\0'};
    sprintf(input, "%s", readFile(SPIFFS, p_mqtt).c_str());

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, input);

    if (error) {
      DBG("deserializeJson() failed: %s\n", error.c_str());
      return;
    }

    const char *h = doc["hostname"];
    const char *u = doc["u"];
    const char *s = doc["s"];          // "adafruitojdfoisdjfosdijfoi.com"
    const char *p = doc["pass"];       // "12345678123456781234567812345678"
    mqtt_port     = atoi(doc["port"]); // 9999

    strcpy(mqtt_user, u);
    strcpy(mqtt_pass, p);
    strcpy(mqtt_server, s);

    mqttClient.setServer(mqtt_server, 1883);
    mqttClient.setCredentials(mqtt_user, mqtt_pass);

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    connectToMqtt();

    MDNS.begin(h);
    hostname = String(h);

    server.addHandler(&events);

    events.onConnect([](AsyncEventSourceClient *client) {
      DBG("Client connected!\n");
      events.send(info.c_str(), "display");
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", HTTP_INDEX, processor);
    });

    server.on("/small", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->redirect("/");
      onFire("{\"preheat\":{\"st\":550,\"r\":550,\"h\":15}}");
    });

    server.on("/big", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->redirect("/");
      onFire("{\"preheat\":{\"st\":550,\"r\":550,\"h\":35}}");
    });

    server.on("/fan", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->redirect("/");
      digitalWrite(FAN, HIGH);
    });

    server.on("/abort", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->redirect("/");
      restart.once_ms(1000, espRestart);
    });

    server.on("/info", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", HTTP_INFO, processor);
    });

    server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->redirect("/");
      restart.once_ms(1000, espRestart);
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", HTTP_UPDATE, processor);
    });

    server.on(
        "/update", HTTP_POST, [](AsyncWebServerRequest *request) {}, onUpload);

    server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
      String json = "[";
      int n       = WiFi.scanComplete();
      if (n == WIFI_SCAN_FAILED)
        WiFi.scanNetworks(false, false, false, 100);

      n = WiFi.scanComplete();

      if (n) {
        for (int i = 0; i < n; ++i) {
          if (i)
            json += ",";
          json += "{";
          json += "\"rssi\":" + String(WiFi.RSSI(i));
          json += ",\"ssid\":\"" + WiFi.SSID(i) + "\"";
          json += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
          json += ",\"channel\":" + String(WiFi.channel(i));
          json += ",\"secure\":" + String(WiFi.encryptionType(i));
          json += "}";
        }
        WiFi.scanDelete();
      }
      json += "]";
      Serial.println(json);
      request->send(200, "application/json", json);
      json = String();
    });

    uint16_t lcdID = lcd.getID();
    DBG("getID(): 0x%02X\n", lcdID);

    if (lcdID == 0x65) {
      buttonTimer.attach_ms(500, readButton);
    }

    tempTimer.attach(2, getTemp);
    sendTimer.attach_ms(10000L, sendData);

    led(GREEN);

    // https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/ResetReason/ResetReason.ino
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_INT_WDT ||
        reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT ||
        reset_reason == ESP_RST_BROWNOUT) {
      errorLog = new PapertrailLogger(PAPERTRAIL_HOST, PAPERTRAIL_PORT,
                                      LogLevel::Error, "\033[0;31m",
                                      "untrol.io", "kiln");

      char rstMsg[12];
      sprintf(rstMsg, "RST= %u", reset_reason);
      errorLog->printf("%s\n", rstMsg);

      notify(rstMsg, strlen(rstMsg));

      String segmentRecover = readFile(SPIFFS, p_segments);
      if (segmentRecover.length())
        onFire(segmentRecover);
    }

    server.onNotFound(onRequest);
  }

  // otaInit();

  safetyTimer.attach_ms(2115L, safetyCheck);

  server.begin();
}

void loop()
{
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA)
    dnsServer.processNextRequest();

  // ArduinoOTA.handle();
}