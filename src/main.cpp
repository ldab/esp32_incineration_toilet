/******************************************************************************
main.cpp
ESP32/ESP8266 based Toilet controller, thermocouple type K, MAX31855
Leonardo Bispo
March, 2022
https://github.com/ldab/toilet_controller
Distributed as-is; no warranty is given.
******************************************************************************/

#define BLYNK_PRINT Serial // Defines the object that is used for printing
// #define BLYNK_DEBUG        // Optional, this enables more detailed prints
// #define APP_DEBUG

#include <Arduino.h>

// Blynk and WiFi

#define USE_SSL

#if defined(ESP32)
#include <ESPmDNS.h>
#include <WiFi.h>
#ifdef USE_SSL
#include <BlynkSimpleEsp32_SSL.h>
#include <WiFiClientSecure.h>
#else
#include <BlynkSimpleEsp32.h>
#include <WiFiClient.h>
#endif
#endif

#include "BlynkEdgent.h"

#include "PapertrailLogger.h"

// OTA
#include <ArduinoOTA.h>
#include <WiFiUdp.h>

// MAX31855
#include <SPI.h>
#include <Wire.h>

#include "Adafruit_MAX31855.h"

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

float temp;
float tInt;

// Control variables
uint32_t initMillis = 0;
uint32_t holdMillis = 0;
int step            = 0;

// Timer instance numbers
int controlTimer;

PapertrailLogger *errorLog;
Adafruit_MAX31855 thermocouple(SPI_CLK, SPI_CS, SPI_MISO);

BLYNK_CONNECTED()
{
  esp_reset_reason_t reset_reason = esp_reset_reason();
  if (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_INT_WDT ||
      reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT ||
      reset_reason == ESP_RST_BROWNOUT) {
    Blynk.logEvent("wdt", reset_reason);
  }

  // TODO recover from cloud

  else if (!edgentTimer.isEnabled(controlTimer)) {
    Blynk.virtualWrite(V7, "Idle 💤");
    Blynk.virtualWrite(V10, LOW);
  }
}

BLYNK_WRITE(V50) { step = param.asInt(); }

BLYNK_WRITE(V10)
{
  uint8_t flushButton = param.asInt();

  if (flushButton && !edgentTimer.isEnabled(controlTimer)) {
    step       = 0;
    initMillis = millis();
    digitalWrite(FAN, HIGH);
    Blynk.virtualWrite(V7, "Flushing 🔥💩");
    edgentTimer.enable(controlTimer);
  } else {
    DBG("Already flushing\n"); // TODO increase timer
    Blynk.virtualWrite(V10, HIGH);
  }
}

void sendData()
{
  Blynk.virtualWrite(V0, temp);
  Blynk.virtualWrite(V8, tInt);
  Blynk.virtualWrite(V50, step);
  Blynk.virtualWrite(V51, WiFi.RSSI());

  if (edgentTimer.isEnabled(controlTimer)) {
    Blynk.virtualWrite(V5, (int)((millis() - initMillis) / (60 * 1000)));
  }
}

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

      Blynk.logEvent("thermocouple_error", error);

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
  Blynk.virtualWrite(V7, "Burning 🔥💩" + _remaining + " min");

  if (_elapsed >= _segment) {
    step++;
    holdMillis = 0;
    DBG("Done with hold, step: %d\n", step);
    Blynk.virtualWrite(V7, "Flushing 🔥💩");
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
      Blynk.logEvent("info", endInfo);
      Blynk.virtualWrite(V10, LOW);
      Blynk.virtualWrite(V7, "Cooling ❄️");
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
    ESP.restart();
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

void otaInit()
{
  ArduinoOTA.setHostname("toilet");

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
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
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

  // wifi_station_set_hostname("kiln"); //setHostname
  BlynkEdgent.begin();

  otaInit();

  edgentTimer.setInterval(2000L, getTemp);
  edgentTimer.setInterval(10000L, sendData);

  controlTimer = edgentTimer.setInterval(5530L, tControl);
  edgentTimer.disable(controlTimer); // enable it after button is pressed
}

void loop()
{
  ArduinoOTA.handle();
  BlynkEdgent.run();
  //  timer.run(); // use edgentTimer
}