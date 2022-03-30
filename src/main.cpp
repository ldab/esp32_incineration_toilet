#include "Arduino.h"
#include <Ticker.h>

#include <DNSServer.h>
#include <ESPmDNS.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "SPIFFS.h"

// MAX31855
#include <SPI.h>
#include <Wire.h>

#include "Adafruit_MAX31855.h"

#include "html_strings.h"

#ifdef VERBOSE
#define DBG(msg, ...)                                                          \
  {                                                                            \
    Serial.printf("[%lu] " msg, millis(), ##__VA_ARGS__);                      \
  }
#else
#define DBG(...)
#endif

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
char _err[32];

// Control variables
uint32_t initMillis = 0;
uint32_t holdMillis = 0;
uint8_t step        = 0;

String ssid, pass;

const char *serverPath = "/server.txt";
const char *tokenPath  = "/token.txt";

Ticker tempTimer;
Ticker restart;

DNSServer dnsServer;
AsyncWebServer server(80);
AsyncEventSource events("/events"); // event source (Server-Sent events)
AsyncWebSocket ws("/ws");           // access at ws://[esp ip]/ws
Adafruit_MAX31855 thermocouple(SPI_CLK, SPI_CS, SPI_MISO);

String processor(const String &var);
String readFile(fs::FS &fs, const char *path);
void writeFile(fs::FS &fs, const char *path, const char *message);

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

// Send notification to HA, max 32 bytes
void notify(char *msg, size_t length)
{
  HTTPClient http;
  WiFiClientSecure client;

  String url   = readFile(SPIFFS, serverPath);
  String token = readFile(SPIFFS, tokenPath);

  // client.setCACert(CA_CERT);
  client.setInsecure();
  if (!client.connect(url.c_str(), 8123, 4000)) {
    DBG("Connection failed!\n");
  } else {
    DBG("Connected!\n");
    char _msg[32];
    client.println("POST /api/services/notify/notify HTTP/1.1");
    client.print("Host: ");
    client.println(url);
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.printf("%u\n", length + 15);
    client.print("Authorization: Bearer ");
    client.println(token);
    client.println();
    sprintf(_msg, "{\"message\": \"%s\"}\n", msg);
    client.println(_msg);
    DBG("%s\n", _msg);

    while (client.connected()) {
      String line = client.readStringUntil('\n');
      if (line.endsWith("OK\r\n"))
        Serial.println(line);
      if (line == "\r")
        break;
    }

    while (client.available())
      client.read();

    client.stop();
  }
}

void onUpload(AsyncWebServerRequest *request, String filename, size_t index,
              uint8_t *data, size_t len, bool final)
{
  if (!index) {
    Serial.printf("Update Start: %s\n", filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  }
  Serial.printf("Progress: %u of %u\r", Update.progress(), Update.size());
  if (!Update.hasError()) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
    }
  }
  if (final) {
    if (Update.end(true)) {
      Serial.printf("Update Success: %uB\n", index + len);
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
  Serial.print("processor:");
  Serial.println(var);

  if (var == "CSS_TEMPLATE")
    return FPSTR(HTTP_STYLE);
  if (var == "INDEX_JS")
    return FPSTR(HTTP_JS);
  if (var == "HTML_HEAD_TITLE")
    return FPSTR(HTML_HEAD_TITLE);
  if (var == "HTML_INFO_BOX") {
    String ret = "";
    if (WiFi.isConnected()) {
      ret = "<strong> Connected</ strong> to ubx<br><em><small> with IP ";
      ret += WiFi.localIP().toString();
      ret += "</small>";
    } else
      ret = "<strong> Not Connected</ strong>";

    return ret;
  }
  return String();
}

void captiveServer()
{
  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
    int params = request->params();
    for (int i = 0; i < params; i++) {
      AsyncWebParameter *p = request->getParam(i);
      if (p->isPost()) {
        // HTTP POST ssid value
        if (p->name() == "ssid") {
          ssid = p->value().c_str();
          Serial.print("SSID set to: ");
          Serial.println(ssid);
        }
        if (p->name() == "pass") {
          pass = p->value().c_str();
          Serial.print("Password set to: ");
          Serial.println(pass);
        }
        if (p->name() == "server") {
          String url = p->value().c_str();
          writeFile(SPIFFS, serverPath, url.c_str());
        }
        if (p->name() == "token") {
          String token = p->value().c_str();
          writeFile(SPIFFS, tokenPath, token.c_str());
        }
      }
    }
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("Connecting to WiFi ..");
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print('.');
      delay(100);
    }
    Serial.println("connected");
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
    Serial.println("- failed to open file for reading");
    return String();
  }

  String fileContent;
  while (file.available()) {
    fileContent = file.readStringUntil('\n');
    break;
  }
  return fileContent;
}

// Write file to SPIFFS
void writeFile(fs::FS &fs, const char *path, const char *message)
{
  Serial.printf("Writing file: %s\r\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("- failed to open file for writing");
    return;
  }
  if (file.print(message)) {
    Serial.println("- file written");
  } else {
    Serial.println("- frite failed");
  }
}

void getTemp()
{
  char msg[5];
  sprintf(msg, "%i", WiFi.RSSI());
  events.send(msg, "temperature");

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
      sprintf(_err, "thermocouple_error: 0x%02X", error);
      notify(_err, strlen(_err));

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

void setup()
{
  pinInit();

  WiFi.mode(WIFI_STA);
  WiFi.begin();

  Serial.begin(115200);

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  if (!thermocouple.begin()) {
    DBG("ERROR.\n");
    // while (1) delay(10);
  } else
    DBG("MAX31855 Good\n");

  if (WiFi.waitForConnectResult() == WL_DISCONNECTED ||
      WiFi.waitForConnectResult() == WL_NO_SSID_AVAIL) { //~ 100 * 100ms
    Serial.printf("WiFi Failed!: %u\n", WiFi.status());

    captiveServer();

    WiFi.softAP("esp-captive");

    server.onNotFound(
        [](AsyncWebServerRequest *request) { request->redirect("/"); });
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());

    DBG("Start Captive Portal at: %s\n", WiFi.softAPIP().toString().c_str());

    server.addHandler(new CaptiveRequestHandler())
        .setFilter(ON_AP_FILTER); // only when requested from AP
  } else {
    Serial.printf("WiFi Connected!\n");
    Serial.println(WiFi.localIP());

    // https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/ResetReason/ResetReason.ino
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_INT_WDT ||
        reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT ||
        reset_reason == ESP_RST_BROWNOUT) {
      char rstMsg[12];
      sprintf(rstMsg, "WDT= %u", reset_reason);
      notify(rstMsg, strlen(rstMsg));
    }

    MDNS.begin("leo_esp32");

    server.addHandler(&events);

    events.onConnect([](AsyncEventSourceClient *client) {
      if (client->lastId()) {
        Serial.printf(
            "Client reconnected! Last message ID that it got is: %u\n",
            client->lastId());
      }
      // send event with message "hello!", id current millis
      // and set reconnect delay to 1 second
      client->send("hello!", NULL, millis(), 10000);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", HTTP_INDEX, processor);
    });

    server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", HTTP_CONFIG, processor);
    });

    server.on("/info", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", HTTP_INFO, processor);
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", HTTP_UPDATE, processor);
    });

    server.on("/notify", HTTP_GET, [](AsyncWebServerRequest *request) {
      notify("hi", strlen("hi"));
      request->send_P(200, "text/plain", "OK");
    });

    server.on(
        "/u", HTTP_POST,
        [](AsyncWebServerRequest *request) {
          AsyncWebServerResponse *response = request->beginResponse(
              200, "text/plain", Update.hasError() ? "OK" : "FAIL");
          response->addHeader("Connection", "close");
          request->send(response);
        },
        onUpload);

    server.on("/gpio", HTTP_GET, [](AsyncWebServerRequest *request) {
      String inputMessage1;
      String inputMessage2;
      // GET input1 value on
      // <ESP_IP>/gpio?output=<inputMessage1>&state=<inputMessage2>
      if (request->hasParam("output") && request->hasParam("state")) {
        inputMessage1 = request->getParam("output")->value();
        inputMessage2 = request->getParam("state")->value();
        digitalWrite(inputMessage1.toInt(), inputMessage2.toInt());
      } else {
        inputMessage1 = "No message sent";
        inputMessage2 = "No message sent";
      }
      Serial.print("GPIO: ");
      Serial.print(inputMessage1);
      Serial.print(" - Set to: ");
      Serial.println(inputMessage2);
      request->send(200, "text/plain", "OK");
    });

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

    tempTimer.attach(2, getTemp);
  }

  server.onNotFound(onRequest);
  server.begin();
}

void loop()
{
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA)
    dnsServer.processNextRequest();
}