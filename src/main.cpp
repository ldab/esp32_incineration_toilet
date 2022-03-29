#include "Arduino.h"
#include "Ticker.h"

#include <DNSServer.h>
#include <ESPmDNS.h>

#include "ESPAsyncWebServer.h"
#include <AsyncTCP.h>
#include <WiFi.h>

#include "Update.h"

#include "html_strings.h"

String ssid, pass;

Ticker tempTimer;

DNSServer dnsServer;
AsyncWebServer server(80);
AsyncEventSource events("/events"); // event source (Server-Sent events)
AsyncWebSocket ws("/ws");           // access at ws://[esp ip]/ws

String processor(const String &var);

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

void onUpload(AsyncWebServerRequest *request, String filename, size_t index,
              uint8_t *data, size_t len, bool final)
{
  if (!index) {
    Serial.printf("Update Start: %s\n", filename.c_str());
    // Update.runAsync(true);
    if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
      Update.printError(Serial);
    }
  }
  if (!Update.hasError()) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
    }
  }
  if (final) {
    if (Update.end(true)) {
      Serial.printf("Update Success: %uB\n", index + len);
      request->redirect("/");
      delay(1000);
      ESP.restart();
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
          // Write file to save value
          // writeFile(LittleFS, ssidPath, ssid.c_str());
        }
        // HTTP POST pass value
        if (p->name() == "pass") {
          pass = p->value().c_str();
          Serial.print("Password set to: ");
          Serial.println(pass);
          // Write file to save value
          // writeFile(LittleFS, passPath, pass.c_str());
        }
      }
    }
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("Connecting to WiFi ..");
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print('.');
      delay(100);
    }
    Serial.println("connected");
    dnsServer.stop();
    ESP.restart();
    // request->send_P(200, "text/html", HTTP_CONFIG, processor);
  });
}

void getTemp()
{
  char msg[5];
  sprintf(msg, "%i", WiFi.RSSI());
  events.send(msg, "temperature");
}

void setup()
{
  WiFi.mode(WIFI_STA);
  Serial.begin(115200);

  WiFi.begin();

  if (WiFi.waitForConnectResult() != WL_CONNECTED) { //~ 100 * 100ms
    Serial.printf("WiFi Failed!\n");
    captiveServer();
    // your other setup stuff...
    WiFi.softAP("esp-captive");
    dnsServer.start(53, "*", WiFi.softAPIP());

    Serial.print("started at:");
    Serial.println(WiFi.softAPIP());

    server.addHandler(new CaptiveRequestHandler())
        .setFilter(ON_AP_FILTER); // only when requested from AP
  } else {
    Serial.printf("WiFi Connected!\n");
    Serial.println(WiFi.localIP());

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

    server.on(
        "/u", HTTP_POST,
        [](AsyncWebServerRequest *request) {
          bool shouldReboot                = !Update.hasError();
          AsyncWebServerResponse *response = request->beginResponse(
              200, "text/plain", shouldReboot ? "OK" : "FAIL");
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