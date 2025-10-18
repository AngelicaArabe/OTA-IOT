#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Preferences.h>

Preferences prefs;

WebServer server(80);
WebSocketsServer webSocket(81);

const int led = LED_BUILTIN;

// ---- estados e métricas
bool apMode = false;                 // estamos em modo AP (hotspot)?
unsigned long lastSensorMillis = 0;
float lastTemp = 0;
long lastRSSI = 0;
float lastUptime = 0;
int lastLedState = -1;
unsigned long lastCmdMillis = 0;     // debounce p/ comandos WS

// ---- credenciais armazenadas
String savedSsid, savedPass;

// ==================== util ====================
String getFormattedUptime() {
  unsigned long totalSec = millis() / 1000;
  unsigned int hours = totalSec / 3600;
  unsigned int minutes = (totalSec % 3600) / 60;
  unsigned int seconds = totalSec % 60;
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", hours, minutes, seconds);
  return String(buffer);
}

void broadcastLog(const String& msg) {
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String finalMsg = "[" + ip + "] " + msg;
  Serial.println(finalMsg);
  webSocket.broadcastTXT(finalMsg);
}

float getInternalTemp() { return temperatureRead(); }

void sendSystemStatus() {
  float temp = getInternalTemp();
  long rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  float uptimeMin = (millis() / 1000.0) / 60.0;

  if (abs(temp - lastTemp) > 0.5 || abs(rssi - lastRSSI) > 2 || abs(uptimeMin - lastUptime) >= 0.2) {
    lastTemp = temp; lastRSSI = rssi; lastUptime = uptimeMin;
    broadcastLog("🌡 Temp: " + String(temp, 1) + "°C | 📶 RSSI: " + String(rssi) +
                 " dBm | ⏱ Uptime: " + getFormattedUptime());
  }
}

void blinkConfirm() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(led, LOW);
    delay(150);
    digitalWrite(led, HIGH);
    delay(150);
  }
}

// ==================== Wi-Fi provision ====================
bool connectSTA(const String& ssid, const String& pass, uint32_t timeoutMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  Serial.printf("Connecting STA to SSID \"%s\" ...\n", ssid.c_str());
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  const char* apSsid = "ESP32_Setup";
  const char* apPass = ""; // aberto para facilitar; se quiser senha, defina aqui
  bool ok = WiFi.softAP(apSsid, apPass);
  delay(100);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP mode: SSID=%s  IP=%s  %s\n", apSsid, ip.toString().c_str(), ok ? "OK" : "FAIL");
  broadcastLog("📡 AP started: SSID=ESP32_Setup | IP: " + ip.toString());
}

// ==================== HTTP handlers ====================
void handleRoot() {
  if (apMode) {
    // em AP servimos o portal de configuração
    File f = SPIFFS.open("/wifi.html", "r");
    if (!f) { server.send(500, "text/plain", "wifi.html missing"); return; }
    server.streamFile(f, "text/html");
    f.close();
  } else {
    // em STA servimos o painel normal
    File f = SPIFFS.open("/index.html", "r");
    if (!f) { server.send(500, "text/plain", "index.html missing"); return; }
    server.streamFile(f, "text/html");
    f.close();
  }
}

void handleSaveWifi() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method Not Allowed"); return; }

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID required");
    return;
  }

  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  server.send(200, "text/plain", "Saved. Rebooting...");
  broadcastLog("💾 Wi-Fi credentials saved. Rebooting...");
  delay(500);
  ESP.restart();
}

void setupRoutes() {
  // raiz dinâmica conforme modo
  server.on("/", HTTP_GET, handleRoot);

  // OTA upload
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    blinkConfirm();
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Starting OTA: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.printf("OTA done: %u bytes\n", upload.totalSize);
      else Update.printError(Serial);
    }
  });

  // salvar credenciais (usado pelo wifi.html)
  server.on("/save", HTTP_POST, handleSaveWifi);

  // em AP, qualquer 404 cai no portal
  server.onNotFound([]() {
    if (apMode) { handleRoot(); }
    else server.send(404, "text/plain", "Not found");
  });
}

// ==================== setup/loop ====================
void setup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH); // OFF ativo-alto (XIAO S3 LED é invertido)
  Serial.begin(115200);
  Serial.println("\nBooting...");

  if (!SPIFFS.begin(true)) Serial.println("❌ SPIFFS mount failed!");
  else Serial.println("✅ SPIFFS mounted");

  // lê credenciais salvas
  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();

  bool connected = false;
  if (savedSsid.length()) {
    connected = connectSTA(savedSsid, savedPass);
  }

  if (!connected) {
    startAP();
  } else {
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    if (!MDNS.begin("esp32")) Serial.println("mDNS failed");
  }

  // HTTP + WS
  setupRoutes();
  server.begin();
  webSocket.begin();

  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_TEXT) {
      String msg = (const char *)payload;
      unsigned long now = millis();
      if (now - lastCmdMillis < 300) return; // debounce
      lastCmdMillis = now;

      if (msg == "reboot") {
        broadcastLog("🔁 Command received: REBOOT");
        delay(300);
        ESP.restart();
      } else if (msg == "led_on") {
        broadcastLog("💡 Command received: LED ON");
        digitalWrite(led, LOW); // ativo-baixo = ON
      } else if (msg == "led_off") {
        broadcastLog("💡 Command received: LED OFF");
        digitalWrite(led, HIGH); // OFF
      } else {
        webSocket.sendTXT(num, "Unknown command: " + msg);
      }
    }
  });

  String ipTxt = WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  broadcastLog(String(apMode ? "📡 AP portal ready" : "✅ Server started") + " | IP: " + ipTxt + " | ⏱ " + getFormattedUptime());
}

void loop() {
  server.handleClient();
  webSocket.loop();
  yield();

  unsigned long currentMillis = millis();

  if (!apMode) {
    // status periódico só no modo STA (evita RSSI 0 no AP)
    if (currentMillis - lastSensorMillis >= 10000) {
      lastSensorMillis = currentMillis;
      sendSystemStatus();
    }
  }

  int currentLedState = !digitalWrite ? 0 : 0; // evita warning estático
  currentLedState = !digitalRead(led); // invertido (ativo-baixo)
  if (currentLedState != lastLedState) {
    lastLedState = currentLedState;
    broadcastLog("💡 LED changed: " + String(currentLedState ? "ON" : "OFF"));
  }

  delay(0);
}
