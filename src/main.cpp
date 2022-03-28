/******************************************************************************
main.cpp
ESP32/ESP8266 based Toilet controller, thermocouple type K, MAX31855
Leonardo Bispo
March, 2022
https://github.com/ldab/toilet_controller
Distributed as-is; no warranty is given.
******************************************************************************/

#include <Arduino.h>
// #include <FS.h>
// #include <SPIFFS.h>

#include <ESPmDNS.h>
#include <WiFi.h>

#include "PapertrailLogger.h"
#include "Ticker.h"
#include "WiFiManager.h"

// OTA
#include <ArduinoOTA.h>
#include <WiFiUdp.h>

// MAX31855
#include <SPI.h>
#include <Wire.h>

// #include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson

#include "Adafruit_MAX31855.h"

#include "LCD16x2.h"

#ifdef VERBOSE
#define DBG(msg, ...)                                                          \
  {                                                                            \
    Serial.printf("[%lu] " msg, millis(), ##__VA_ARGS__);                      \
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

// https://randomnerdtutorials.com/esp32-pinout-reference-gpios/
#define FAN               14
#define FAN_FB            15
#define ELEMENT           27
#define SPI_CLK           5
#define SPI_CS            18
#define SPI_MISO          22
#define FINAL_TEMPERATURE 550
#define BIG_FLUSH         35
#define DIFFERENTIAL      5 // degC
#define I2C_SDA           13
#define I2C_SCL           12

float temp;
float tInt;

// Control variables
uint32_t initMillis = 0;
uint32_t holdMillis = 0;
int step            = 0;

char mqtt_server[40];
char mqtt_port[6]     = "8080";
char api_token[34]    = "YOUR_API_TOKEN";
bool shouldSaveConfig = false;

WiFiManager wm;
PapertrailLogger *errorLog;
Adafruit_MAX31855 thermocouple(SPI_CLK, SPI_CS, SPI_MISO);
LCD16x2 lcd;
Ticker controlTimer;
Ticker buttonCheck;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server,
                                        40);
WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 12);
WiFiManagerParameter custom_mqtt_token("token", "mqtt token", api_token, 40);

WiFiManagerParameter
    custom_html("<p style=\"color:pink;font-weight:Bold;\">This Is Custom "
                "HTML</p>"); // only custom html
const char _customHtml_checkbox[] = "type=\"checkbox\"";
WiFiManagerParameter custom_checkbox("my_checkbox", "My Checkbox", "T", 2,
                                     _customHtml_checkbox, WFM_LABEL_AFTER);

const char _customHtml_button[] = "type=\"submit\"";
WiFiManagerParameter custom_button("flush", "", "Flush", 6, _customHtml_button,
                                   WFM_LABEL_DEFAULT);

const char *bufferStr = R"(
  <!-- INPUT CHOICE -->
  <br/>
  <p>Select Choice</p>
  <input style='display: inline-block;' type='radio' id='choice1' name='program_selection' value='1'>
  <label for='choice1'>Choice1</label><br/>
  <input style='display: inline-block;' type='radio' id='choice2' name='program_selection' value='2'>
  <label for='choice2'>Choice2</label><br/>
  <!-- INPUT SELECT -->
  <br/>
  <label for='input_select'>Label for Input Select</label>
  <select name="input_select" id="input_select" class="button">
  <option value="0">Option 1</option>
  <option value="1" selected>Option 2</option>
  <option value="2">Option 3</option>
  <option value="3">Option 4</option>
  </select>
  )";

WiFiManagerParameter custom_html_inputs(bufferStr);

// BLYNK_CONNECTED()
// {
//   esp_reset_reason_t reset_reason = esp_reset_reason();
//   if (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_INT_WDT ||
//       reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT ||
//       reset_reason == ESP_RST_BROWNOUT) {
//     Blynk.logEvent("wdt", reset_reason);
//   }

//   // TODO recover from cloud

//   else if (!edgentTimer.isEnabled(controlTimer)) {
//     Blynk.virtualWrite(V7, "Idle 💤");
//     Blynk.virtualWrite(V10, LOW);
//   }
// }

// BLYNK_WRITE(V10)
// {
//   uint8_t flushButton = param.asInt();

//   if (flushButton && !edgentTimer.isEnabled(controlTimer)) {
//     step       = 0;
//     initMillis = millis();
//     digitalWrite(FAN, HIGH);
//     Blynk.virtualWrite(V7, "Flushing 🔥💩");
//     edgentTimer.enable(controlTimer);
//   } else {
//     DBG("Already flushing\n"); // TODO increase timer
//     Blynk.virtualWrite(V10, HIGH);
//   }
// }

void safetyCheck()
{
  // TODO check if takes too long
  //  (int)((millis() - initMillis) / (60 * 1000))

  // check if temp still rises after step == 1;

  // Check if fan is running
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
    DBG("Thermocouple error #%i\n", error);
    if (!tErr) {
      temp = NAN;
      tErr = true;

      // Blynk.logEvent("thermocouple_error", error);

      digitalWrite(ELEMENT, LOW);
    }
  } else {
    tErr = false;
    DBG("T: %.02fdegC\n", temp);
    DBG("tInt: %.02fdegC\n", tInt);
  }
}

void holdTimer(uint32_t _segment)
{
  if (holdMillis == 0) {
    DBG("Start hold for %dmin\n", _segment);
    holdMillis = millis();
  }
  uint32_t _elapsed = (millis() - holdMillis) / (60 * 1000);
  String _remaining = String(_segment - _elapsed);
  DBG("Remaining: %s\n", _remaining.c_str());
  // Blynk.virtualWrite(V7, "Burning 🔥💩" + _remaining + " min");

  if (_elapsed >= _segment) {
    step++;
    holdMillis = 0;
    DBG("Done with hold, step: %d\n", step);
    // Blynk.virtualWrite(V7, "Flushing 🔥💩");
  }
}

void tControl()
{
  DBG("Control ST: %udegC, step: %u\n", FINAL_TEMPERATURE, step);
  static uint8_t diff;

  if (step == 1) {
    if (digitalRead(ELEMENT)) {
      digitalWrite(ELEMENT, LOW);

      uint8_t h = (millis() - initMillis) / (1000 * 3600);
      uint8_t m = ((millis() - initMillis) - (h * 3600 * 1000)) / (60 * 1000);
      char endInfo[64];
      sprintf(endInfo, "Finished flushing after, after: %d:%d", h, m);
      DBG("%s", endInfo);
      // Blynk.logEvent("info", endInfo);
      // Blynk.virtualWrite(V10, LOW);
      // Blynk.virtualWrite(V7, "Cooling ❄️");
    }
    if (temp <= 100) {
      DBG("Finished cooling\n");
      step++;
    }
    return;
  }
  if (step == 2) {
    digitalWrite(FAN, LOW);
    digitalWrite(ELEMENT, LOW); // just in case
    return;
  }

  if (!isnan(temp)) {
    DBG("Control relay \n");
    float delta_t = FINAL_TEMPERATURE - temp - diff;
    if (delta_t >= 0) {
      if (!digitalRead(ELEMENT)) {
        digitalWrite(ELEMENT, HIGH);
        DBG("RELAY ON \n");
        diff = 0;
      }
    } else if (digitalRead(ELEMENT)) {
      digitalWrite(ELEMENT, LOW);
      DBG("RELAY OFF \n");
      diff = DIFFERENTIAL;
    }
    if (temp > FINAL_TEMPERATURE) {
      holdTimer(BIG_FLUSH);
    }
  }
}

void checkButton(void)
{
  uint8_t buttons = lcd.readButtons();

  if (buttons == 0b1111)
    return;

  lcd.lcdGoToXY(7, 1);
  if (buttons & 0x01)
    lcd.lcdWrite("0");
  else
    lcd.lcdWrite("1");

  lcd.lcdGoToXY(15, 1);
  if (buttons & 0x02)
    lcd.lcdWrite("0");
  else
    lcd.lcdWrite("1");

  lcd.lcdGoToXY(7, 2);
  if (buttons & 0x04)
    lcd.lcdWrite("0");
  else
    lcd.lcdWrite("1");

  lcd.lcdGoToXY(15, 2);
  if (buttons & 0x08)
    lcd.lcdWrite("0");
  else
    lcd.lcdWrite("1");
}

void saveParamsCallback()
{
  Serial.println("Get Params:");
  Serial.print(custom_mqtt_server.getID());
  Serial.print(" : ");
  Serial.println(custom_mqtt_server.getValue());

  controlTimer.attach_ms(5520, tControl);
}

void saveWifiCallback()
{
  Serial.println("[CALLBACK] saveCallback fired");
  shouldSaveConfig = true;
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(api_token, custom_mqtt_token.getValue());
  Serial.println("The values in the file are: ");
  Serial.println("\tmqtt_server : " + String(mqtt_server));
  Serial.println("\tmqtt_port : " + String(mqtt_port));
  Serial.println("\tapi_token : " + String(api_token));

  //   if (shouldSaveConfig) {
  //     Serial.println("saving config");
  // #ifdef ARDUINOJSON_VERSION_MAJOR >= 6
  //     DynamicJsonDocument json(1024);
  // #else
  //     DynamicJsonBuffer jsonBuffer;
  //     JsonObject &json = jsonBuffer.createObject();
  // #endif
  //     json["mqtt_server"] = mqtt_server;
  //     json["mqtt_port"]   = mqtt_port;
  //     json["api_token"]   = api_token;

  //     File configFile     = SPIFFS.open("/config.json", "w");
  //     if (!configFile) {
  //       Serial.println("failed to open config file for writing");
  //     }

  // #ifdef ARDUINOJSON_VERSION_MAJOR >= 6
  //     serializeJson(json, Serial);
  //     serializeJson(json, configFile);
  // #else
  //     json.printTo(Serial);
  //     json.printTo(configFile);
  // #endif
  //     configFile.close();
  //     // end save

  ESP.restart();
}

void IRAM_ATTR ISR()
{
  static volatile uint32_t count      = 0;
  static volatile uint32_t lastMillis = 0;
  count++;
  if (lastMillis == 0)
    lastMillis = millis();
}

void pinInit()
{
  pinMode(FAN, OUTPUT);
  digitalWrite(FAN, LOW);

  pinMode(ELEMENT, OUTPUT);
  digitalWrite(ELEMENT, LOW);

  attachInterrupt(FAN_FB, ISR, FALLING);
}

// void loadSpiffs()
// {
//   // read configuration from FS json
//   Serial.println("mounting FS...");

//   if (SPIFFS.begin()) {
//     Serial.println("mounted file system");
//     if (SPIFFS.exists("/config.json")) {
//       // file exists, reading and loading
//       Serial.println("reading config file");
//       File configFile = SPIFFS.open("/config.json", "r");
//       if (configFile) {
//         Serial.println("opened config file");
//         size_t size = configFile.size();
//         // Allocate a buffer to store contents of the file.
//         std::unique_ptr<char[]> buf(new char[size]);

//         configFile.readBytes(buf.get(), size);

//         DynamicJsonDocument json(1024);
//         auto deserializeError = deserializeJson(json, buf.get());
//         serializeJson(json, Serial);

//         if (!deserializeError) {
//           Serial.println("\nparsed json");
//           strcpy(mqtt_server, json["mqtt_server"]);
//           strcpy(mqtt_port, json["mqtt_port"]);
//           strcpy(api_token, json["api_token"]);
//         } else {
//           Serial.println("failed to load json config");
//         }
//         configFile.close();
//       }
//     }
//   } else {
//     Serial.println("failed to mount FS");
//   }
// }

void setup()
{
#ifdef VERBOSE
  Serial.begin(115200);
  DBG("VERSION %s\n", BLYNK_FIRMWARE_VERSION);
#endif

#ifdef CALIBRATE
  // Measure GPIO in order to determine Vref to gpio 25 or 26 or 27
  adc2_vref_to_gpio(GPIO_NUM_25);
  delay(5000);
  abort();
#endif

  pinInit();

  DBG("resetreason %u\n", esp_reset_reason());

  if (!thermocouple.begin()) {
    DBG("ERROR.\n");
    // while (1) delay(10);
  } else
    DBG("MAX31855 Good\n");

  Wire.begin(I2C_SDA, I2C_SCL);
  DBG("Board ID: 0x%02X\n", lcd.getID());
  lcd.lcdClear();

  lcd.lcdGoToXY(2, 1);
  lcd.lcdWrite("BUT1:");

  lcd.lcdGoToXY(10, 1);
  lcd.lcdWrite("BUT2:");

  lcd.lcdGoToXY(2, 2);
  lcd.lcdWrite("BUT3:");

  lcd.lcdGoToXY(10, 2);
  lcd.lcdWrite("BUT4:");

  // loadSpiffs();

  WiFi.mode(WIFI_STA);
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(20); // how long to try to connect for before continuing
  wm.setConnectRetries(2);
  wm.setConfigPortalTimeout(60); // auto close configportal after n seconds
  wm.setAPClientCheck(true);     // avoid timeout if client connected to softap
  wm.setHostname("toilet");
  wm.setShowPassword(true);

  wm.setTitle("💩");
  wm.setDarkMode(true);
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_button);
  // wm.setCustomHeadElement("<style>html{filter: invert(100%); -webkit-filter:
  // invert(100%);}</style>");
  wm.setSaveParamsCallback(saveParamsCallback);
  wm.setSaveConfigCallback(saveWifiCallback);

  // edgentTimer.setInterval(2000L, getTemp);
  // edgentTimer.setInterval(10000L, sendData);

  // controlTimer = edgentTimer.setInterval(5530L, tControl);
  // edgentTimer.disable(controlTimer); // enable it after button is pressed

  bool res;
  res = wm.autoConnect();

  if (!res) {
    Serial.println("Failed to connect or hit timeout");
    // ESP.restart();
  } else {
    // if you get here you have connected to the WiFi
    Serial.println("connected...yeey :)");
    ArduinoOTA.begin();

    /*
      Set cutom menu via menu[] or vector
      const char* menu[] =
      {"wifi","wifinoscan","info","param","close","sep","erase","restart","exit"};
      wm.setMenu(menu,9); // custom menu array must provide length
    */
    std::vector<const char *> menu = {"param", "info",    "sep",
                                      "close", "restart", "erase"};
    wm.setMenu(menu); // custom menu, pass vector
    wm.startWebPortal();
  }

  buttonCheck.attach_ms(500, checkButton);
}

void loop()
{
  ArduinoOTA.handle();
  wm.process();
}