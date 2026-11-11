// Personenzähler mit Wemos D1 mini + 2x VL53L1X
//
// Verdrahtung:
//   VL53L1X A + B:  VIN -> 3V3, GND -> G, SDA -> D2, SCL -> D1
//   XSHUT Sensor A (aussen) -> D5
//   XSHUT Sensor B (innen)  -> D6
//
// Zählprinzip: Wird zuerst A und zuletzt B ausgelöst -> Eintritt,
//              zuerst B und zuletzt A -> Austritt.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <VL53L1X.h>

#include "webpages.h"

// --- Hardware --------------------------------------------------------------
constexpr uint8_t PIN_XSHUT_A = D5;
//constexpr uint8_t PIN_XSHUT_B = D6;
constexpr uint8_t PIN_XSHUT_B = D0;
constexpr uint8_t ADDR_A = 0x30;
constexpr uint8_t ADDR_B = 0x31;

// --- Allgemein -------------------------------------------------------------
#define FW_VERSION "1.1.0"
constexpr char FW_BUILD[] = FW_VERSION " (" __DATE__ " " __TIME__ ")";
constexpr char AP_PASSWORD[] = "zaehler1";          // min. 8 Zeichen, leer = offenes WLAN
constexpr char ADMIN_USER[] = "admin";               // Benutzer für die Update-Seite
constexpr char DEFAULT_ADMIN_PASS[] = "zaehler1";    // Update-Passwort, in der Konfig änderbar
constexpr uint32_t WIFI_CONNECT_TIMEOUT = 20000;     // ms
constexpr uint32_t AP_RETRY_INTERVAL = 5UL * 60000;  // im AP-Modus alle 5 min WLAN neu versuchen
constexpr uint32_t COUNT_SAVE_INTERVAL = 60000;      // Zählerstand max. 1x pro Minute ins Flash
constexpr uint32_t MQTT_RETRY_INTERVAL = 30000;     // Verbindungsversuch blockiert kurz -> selten
constexpr uint32_t MQTT_PUBLISH_INTERVAL = 60000;    // zusätzlich periodisch publizieren
constexpr uint8_t RELEASE_SAMPLES = 2;               // freie Messungen, bis ein Sensor als frei gilt

const char *CONFIG_FILE = "/config.json";
const char *COUNTS_FILE = "/counts.json";

struct Config {
  String wifiSsid;
  String wifiPass;
  String hostname = "personenzaehler";
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPass;
  String mqttTopic = "personenzaehler";
  uint16_t threshold = 1000;  // mm
  bool invert = false;
  bool longRange = false;  // false: Short-Modus, 50 Hz, bis ~1.3 m / true: Medium, 30 Hz, bis ~2.9 m
  String adminPass = DEFAULT_ADMIN_PASS;  // schützt Web-Update und ArduinoOTA
} cfg;

ESP8266WebServer server(80);
DNSServer dns;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Ein Sensor mit seinem Zustand im aktuellen Durchgang
struct Lane {
  VL53L1X dev;
  char name;
  uint8_t xshutPin;
  uint8_t addr;
  int dist = -1;
  bool occ = false;
  uint8_t freeCnt = 0;
  uint32_t firstFreeAt = 0;
  bool triggered = false;  // im aktuellen Durchgang ausgelöst
  uint32_t onAt = 0;       // erste Belegung im Durchgang
  uint32_t offAt = 0;      // letzte Freigabe im Durchgang
  uint16_t samples = 0;    // Messungen seit der letzten Debug-Ausgabe

  Lane(char n, uint8_t pin, uint8_t a) : name(n), xshutPin(pin), addr(a) {}
};

Lane laneA('A', PIN_XSHUT_A, ADDR_A);
Lane laneB('B', PIN_XSHUT_B, ADDR_B);
bool passageActive = false;  // mind. ein Sensor belegt -> keine blockierenden Aktionen

bool apMode = false;
uint32_t apStartedAt = 0;
bool sensorsOk = false;
uint32_t restartAt = 0;

// Zähler
long inside = 0;
unsigned long totalIn = 0, totalOut = 0;
bool countsDirty = false;
uint32_t lastCountSave = 0;

// MQTT
uint32_t lastMqttAttempt = 0;
uint32_t lastMqttPublish = 0;

// ---------------------------------------------------------------------------
// Logging: Ausgabe auf Serial + Ringpuffer für die Webseite /log
// ---------------------------------------------------------------------------
constexpr size_t LOG_LINES = 60;
constexpr uint32_t DEBUG_DIST_INTERVAL = 500;  // ms, Distanz-Ausgabe im Debugmodus

String logBuf[LOG_LINES];
size_t logHead = 0;
size_t logCount = 0;
uint32_t logSeq = 0;
bool debugMode = false;  // ausführliche Sensor-Logs, umschaltbar auf /log
uint32_t lastDistLog = 0;

void logMsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logMsg(const char *fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  unsigned s = millis() / 1000;
  char line[180];
  snprintf(line, sizeof(line), "[%02u:%02u:%02u] %s", s / 3600, (s / 60) % 60, s % 60, msg);

  Serial.println(line);
  logBuf[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
  logSeq++;
}

#define DBG(...) \
  do { if (debugMode) logMsg(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Konfiguration / Persistenz
// ---------------------------------------------------------------------------
void loadConfig() {
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    cfg.wifiSsid = doc["ssid"] | "";
    cfg.wifiPass = doc["pass"] | "";
    cfg.hostname = doc["hostname"] | "personenzaehler";
    cfg.mqttHost = doc["mqtt_host"] | "";
    cfg.mqttPort = doc["mqtt_port"] | 1883;
    cfg.mqttUser = doc["mqtt_user"] | "";
    cfg.mqttPass = doc["mqtt_pass"] | "";
    cfg.mqttTopic = doc["mqtt_topic"] | "personenzaehler";
    cfg.threshold = doc["threshold"] | 1000;
    cfg.invert = doc["invert"] | false;
    cfg.longRange = doc["long_range"] | false;
    cfg.adminPass = doc["admin_pass"] | DEFAULT_ADMIN_PASS;
  }
  f.close();
}

void saveConfig() {
  JsonDocument doc;
  doc["ssid"] = cfg.wifiSsid;
  doc["pass"] = cfg.wifiPass;
  doc["hostname"] = cfg.hostname;
  doc["mqtt_host"] = cfg.mqttHost;
  doc["mqtt_port"] = cfg.mqttPort;
  doc["mqtt_user"] = cfg.mqttUser;
  doc["mqtt_pass"] = cfg.mqttPass;
  doc["mqtt_topic"] = cfg.mqttTopic;
  doc["threshold"] = cfg.threshold;
  doc["invert"] = cfg.invert;
  doc["long_range"] = cfg.longRange;
  doc["admin_pass"] = cfg.adminPass;
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void loadCounts() {
  File f = LittleFS.open(COUNTS_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    inside = doc["inside"] | 0;
    totalIn = doc["in"] | 0;
    totalOut = doc["out"] | 0;
  }
  f.close();
}

void saveCounts() {
  JsonDocument doc;
  doc["inside"] = inside;
  doc["in"] = totalIn;
  doc["out"] = totalOut;
  File f = LittleFS.open(COUNTS_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  countsDirty = false;
  lastCountSave = millis();
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
String topic(const char *suffix) { return cfg.mqttTopic + "/" + suffix; }

void publishState() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["inside"] = inside;
  doc["in"] = totalIn;
  doc["out"] = totalOut;
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(topic("state").c_str(), payload.c_str(), true);
  mqtt.publish(topic("inside").c_str(), String(inside).c_str(), true);
  lastMqttPublish = millis();
}

void resetCounts() {
  inside = 0;
  totalIn = 0;
  totalOut = 0;
  saveCounts();
  publishState();
  logMsg("Zähler zurückgesetzt");
}

void mqttCallback(char *t, byte *payload, unsigned int len) {
  String tp(t);
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  logMsg("MQTT empfangen: %s = '%s'", t, msg.c_str());

  if (tp == topic("reset")) {
    resetCounts();
  } else if (tp == topic("set")) {
    // Personen im Raum manuell korrigieren, z.B. Payload "3"
    inside = max(0L, msg.toInt());
    countsDirty = true;
    publishState();
  }
}

void mqttLoop() {
  if (apMode || cfg.mqttHost.isEmpty()) return;

  if (!mqtt.connected()) {
    if (passageActive || millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL) return;
    lastMqttAttempt = millis();

    String clientId = cfg.hostname + "-" + String(ESP.getChipId(), HEX);
    String avail = topic("availability");
    bool ok;
    if (cfg.mqttUser.isEmpty()) {
      ok = mqtt.connect(clientId.c_str(), avail.c_str(), 1, true, "offline");
    } else {
      ok = mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str(),
                        avail.c_str(), 1, true, "offline");
    }
    if (ok) {
      logMsg("MQTT verbunden mit %s:%u", cfg.mqttHost.c_str(), cfg.mqttPort);
      mqtt.publish(avail.c_str(), "online", true);
      mqtt.subscribe(topic("reset").c_str());
      mqtt.subscribe(topic("set").c_str());
      publishState();
    } else {
      // rc: -4 Timeout, -2 Verbindung fehlgeschlagen, 4 falsche Zugangsdaten, 5 nicht autorisiert
      logMsg("MQTT-Verbindung zu %s:%u fehlgeschlagen, rc=%d", cfg.mqttHost.c_str(), cfg.mqttPort,
           mqtt.state());
    }
    return;
  }

  mqtt.loop();
  if (millis() - lastMqttPublish > MQTT_PUBLISH_INTERVAL) publishState();
}

// ---------------------------------------------------------------------------
// Sensoren & Zähllogik
// ---------------------------------------------------------------------------
bool initSensor(Lane &l) {
  digitalWrite(l.xshutPin, HIGH);
  delay(10);
  l.dev.setTimeout(500);
  if (!l.dev.init()) {
    logMsg("Sensor %c (XSHUT GPIO%u) antwortet nicht (Verdrahtung/Stromversorgung prüfen)", l.name,
           l.xshutPin);
    return false;
  }
  l.dev.setAddress(l.addr);
  if (cfg.longRange) {
    // Medium: bis ~2.9 m, min. 33 ms Messzeit -> ~30 Messungen/s
    l.dev.setDistanceMode(VL53L1X::Medium);
    l.dev.setMeasurementTimingBudget(33000);
    l.dev.startContinuous(33);
  } else {
    // Short: bis ~1.3 m, 20 ms Messzeit -> ~50 Messungen/s, unempfindlicher gegen Fremdlicht
    l.dev.setDistanceMode(VL53L1X::Short);
    l.dev.setMeasurementTimingBudget(20000);
    l.dev.startContinuous(20);
  }
  return true;
}

bool initSensors() {
  pinMode(PIN_XSHUT_A, OUTPUT);
  pinMode(PIN_XSHUT_B, OUTPUT);
  digitalWrite(PIN_XSHUT_A, LOW);
  digitalWrite(PIN_XSHUT_B, LOW);
  delay(10);

  Wire.begin();  // SDA = D2, SCL = D1
  Wire.setClock(400000);

  // Nacheinander einschalten und jedem Sensor eine eigene I2C-Adresse geben
  bool okA = initSensor(laneA);
  bool okB = initSensor(laneB);
  logMsg("Sensor A: %s, Sensor B: %s, Modus: %s", okA ? "OK" : "FEHLER", okB ? "OK" : "FEHLER",
         cfg.longRange ? "Medium (30 Hz)" : "Short (50 Hz)");
  return okA && okB;
}

bool isUsableReading(uint8_t status) {
  // Neben "gültig" auch die Fälle zulassen, in denen ein Objekt sehr nah ist
  // oder die Wrap-Prüfung (noch) fehlt - die treten bei schnellen Bewegungen auf
  return status == VL53L1X::RangeValid || status == VL53L1X::RangeValidMinRangeClipped ||
         status == VL53L1X::RangeValidNoWrapCheckFail;
}

void readSensor(Lane &l) {
  if (!l.dev.dataReady()) return;
  l.dev.read(false);
  uint32_t now = millis();
  l.samples++;

  uint8_t status = l.dev.ranging_data.range_status;
  bool usable = isUsableReading(status);
  l.dist = usable ? l.dev.ranging_data.range_mm : -1;
  bool hit = usable && l.dist < cfg.threshold;

  if (hit) {
    // Belegung sofort übernehmen - bei schnellen Durchgängen zählt jede Messung
    l.freeCnt = 0;
    if (!l.occ) {
      l.occ = true;
      if (!l.triggered) {
        l.triggered = true;
        l.onAt = now;
      }
      DBG("Sensor %c BELEGT (%d mm)", l.name, l.dist);
    }
  } else if (l.occ) {
    // Freigabe erst nach mehreren freien Messungen (überbrückt Lücken, z.B. zwischen Armen/Beinen)
    if (l.freeCnt == 0) l.firstFreeAt = now;
    if (++l.freeCnt >= RELEASE_SAMPLES) {
      l.occ = false;
      l.freeCnt = 0;
      l.offAt = l.firstFreeAt;
      DBG("Sensor %c frei (Status: %s)", l.name, VL53L1X::rangeStatusToString(
                                                     (VL53L1X::RangeStatus)status));
    }
  }
}

void countEvent(bool entry) {
  if (cfg.invert) entry = !entry;
  if (entry) {
    totalIn++;
    inside++;
  } else {
    totalOut++;
    if (inside > 0) inside--;
  }
  countsDirty = true;
  // Einzelereignis +1/-1, nicht retained: so lassen sich mehrere Zähler (Türen)
  // einfach aufsummieren, ohne dass beim Neuverbinden etwas doppelt gezählt wird
  if (mqtt.connected()) mqtt.publish(topic("reinraus").c_str(), entry ? "1" : "-1", false);
  publishState();
  logMsg("%s -> im Raum: %ld (rein %lu / raus %lu)", entry ? "EINTRITT" : "AUSTRITT", inside,
         totalIn, totalOut);
}

void evaluatePassage() {
  if (!laneA.triggered || !laneB.triggered) {
    DBG("Durchgang ignoriert: nur Sensor %c ausgelöst", laneA.triggered ? 'A' : 'B');
    return;
  }

  // Positiv = A war früher dran als B. Ein- und Austrittszeit zusammen auswerten:
  // Rennt jemand durch, lösen beide Sensoren oft fast gleichzeitig aus - dann
  // entscheidet, welcher Sensor zuerst wieder frei wurde.
  int32_t dOn = (int32_t)(laneB.onAt - laneA.onAt);
  int32_t dOff = (int32_t)(laneB.offAt - laneA.offAt);
  int32_t score = dOn + dOff;

  DBG("Durchgang: B belegt %+ld ms nach A, B frei %+ld ms nach A, Dauer %lu ms", (long)dOn,
      (long)dOff, (unsigned long)(max(laneA.offAt, laneB.offAt) - min(laneA.onAt, laneB.onAt)));

  if (score > 0) countEvent(true);
  else if (score < 0) countEvent(false);
  else DBG("Durchgang ignoriert: Richtung nicht eindeutig");
}

void processPassage() {
  bool anyOcc = laneA.occ || laneB.occ;
  if (anyOcc) {
    passageActive = true;
    return;
  }
  if (!passageActive) return;

  // beide Sensoren wieder frei -> Durchgang beendet
  passageActive = false;
  evaluatePassage();
  laneA.triggered = false;
  laneB.triggered = false;
}

// ---------------------------------------------------------------------------
// WLAN
// ---------------------------------------------------------------------------
bool connectWifi() {
  if (cfg.wifiSsid.isEmpty()) return false;
  logMsg("Verbinde mit WLAN '%s' ...", cfg.wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.hostname(cfg.hostname);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    logMsg("WLAN-Verbindung fehlgeschlagen (Status %d)", WiFi.status());
    return false;
  }
  logMsg("WLAN verbunden, IP: %s, RSSI: %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

void startAccessPoint() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);  // STA bleibt aktiv, damit die Netzwerksuche funktioniert
  String apName = "Personenzaehler-" + String(ESP.getChipId(), HEX);
  WiFi.softAP(apName, AP_PASSWORD);
  dns.start(53, "*", WiFi.softAPIP());  // Captive Portal
  apMode = true;
  apStartedAt = millis();
  logMsg("Access Point '%s' gestartet, IP: %s", apName.c_str(),
       WiFi.softAPIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// Webserver
// ---------------------------------------------------------------------------
void handleStatus() {
  JsonDocument doc;
  doc["inside"] = inside;
  doc["in"] = totalIn;
  doc["out"] = totalOut;
  doc["distA"] = laneA.dist;
  doc["distB"] = laneB.dist;
  doc["occA"] = laneA.occ;
  doc["occB"] = laneB.occ;
  doc["sensors"] = sensorsOk;
  doc["mode"] = apMode ? "Access Point" : "WLAN (" + cfg.wifiSsid + ")";
  doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["mqtt"] = cfg.mqttHost.isEmpty() ? "deaktiviert"
                : mqtt.connected()     ? "verbunden"
                                       : "getrennt";
  doc["uptime"] = millis() / 1000;
  doc["version"] = FW_BUILD;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleReset() {
  resetCounts();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGetConfig() {
  // Passwörter werden bewusst nicht ausgeliefert
  JsonDocument doc;
  doc["ssid"] = cfg.wifiSsid;
  doc["hostname"] = cfg.hostname;
  doc["mqtt_host"] = cfg.mqttHost;
  doc["mqtt_port"] = cfg.mqttPort;
  doc["mqtt_user"] = cfg.mqttUser;
  doc["mqtt_topic"] = cfg.mqttTopic;
  doc["threshold"] = cfg.threshold;
  doc["invert"] = cfg.invert;
  doc["long_range"] = cfg.longRange;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSaveConfig() {
  cfg.wifiSsid = server.arg("ssid");
  if (server.arg("pass").length()) cfg.wifiPass = server.arg("pass");
  if (server.arg("hostname").length()) cfg.hostname = server.arg("hostname");
  cfg.mqttHost = server.arg("mqtt_host");
  cfg.mqttPort = server.arg("mqtt_port").toInt() > 0 ? server.arg("mqtt_port").toInt() : 1883;
  cfg.mqttUser = server.arg("mqtt_user");
  if (server.arg("mqtt_pass").length()) cfg.mqttPass = server.arg("mqtt_pass");
  if (server.arg("mqtt_topic").length()) cfg.mqttTopic = server.arg("mqtt_topic");
  if (server.arg("threshold").toInt() > 0) cfg.threshold = server.arg("threshold").toInt();
  cfg.invert = server.hasArg("invert");
  cfg.longRange = server.hasArg("long_range");
  if (server.arg("admin_pass").length()) cfg.adminPass = server.arg("admin_pass");

  saveConfig();
  saveCounts();
  logMsg("Konfiguration gespeichert - Neustart");
  server.send_P(200, "text/html", SAVED_HTML);
  restartAt = millis() + 1500;
}

void handleScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i).isEmpty()) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleLog() {
  // ?since=<seq> liefert nur neue Zeilen seit dem letzten Abruf
  uint32_t since = server.arg("since").toInt();
  uint32_t newLines = logSeq - since;
  if (since == 0 || since > logSeq || newLines > logCount) newLines = logCount;

  JsonDocument doc;
  doc["seq"] = logSeq;
  doc["debug"] = debugMode;
  JsonArray lines = doc["lines"].to<JsonArray>();
  for (size_t i = logCount - newLines; i < logCount; i++) {
    size_t idx = (logHead + LOG_LINES - logCount + i) % LOG_LINES;
    lines.add(logBuf[idx]);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleDebug() {
  debugMode = server.arg("on") == "1";
  logMsg("Debugmodus %s", debugMode ? "EIN" : "AUS");
  server.send(200, "application/json", debugMode ? "{\"debug\":true}" : "{\"debug\":false}");
}

// ---------------------------------------------------------------------------
// Login & OTA-Update über den Browser
// ---------------------------------------------------------------------------
bool updateAuthorized = false;
String updateError;

bool checkAdmin() {
  if (server.authenticate(ADMIN_USER, cfg.adminPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Personenzaehler", "Anmeldung erforderlich");
  return false;
}

// Wrapper: Handler nur nach erfolgreicher Anmeldung ausführen
template <typename F>
std::function<void()> protect(F handler) {
  return [handler] {
    if (checkAdmin()) handler();
  };
}

void handleUpdateUpload() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    updateError = "";
    updateAuthorized = server.authenticate(ADMIN_USER, cfg.adminPass.c_str());
    if (!updateAuthorized) return;
    logMsg("Web-Update gestartet: %s", up.filename.c_str());
    if (countsDirty) saveCounts();
    uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSpace, U_FLASH)) updateError = Update.getErrorString();
    return;
  }
  if (!updateAuthorized || updateError.length()) return;

  if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) updateError = Update.getErrorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) logMsg("Web-Update erfolgreich: %u Bytes", (unsigned)up.totalSize);
    else updateError = Update.getErrorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    updateError = "Upload abgebrochen";
  }
}

void handleUpdateDone() {
  if (!updateAuthorized) {
    server.requestAuthentication(BASIC_AUTH, "Personenzaehler", "Anmeldung erforderlich");
    return;
  }
  if (updateError.length()) {
    logMsg("Web-Update fehlgeschlagen: %s", updateError.c_str());
    server.send(500, "text/plain", "Fehler: " + updateError);
    return;
  }
  server.send(200, "text/plain", "OK");
  restartAt = millis() + 1000;
}

// ---------------------------------------------------------------------------
// OTA-Update direkt aus PlatformIO / Arduino IDE (espota, Port 8266)
// ---------------------------------------------------------------------------
void setupArduinoOTA() {
  ArduinoOTA.setHostname(cfg.hostname.c_str());
  ArduinoOTA.setPassword(cfg.adminPass.c_str());
  ArduinoOTA.onStart([] {
    if (ArduinoOTA.getCommand() == U_FS) {
      LittleFS.end();  // Dateisystem wird überschrieben
    } else if (countsDirty) {
      saveCounts();
    }
    logMsg("OTA-Update gestartet (%s)", ArduinoOTA.getCommand() == U_FS ? "Dateisystem" : "Firmware");
  });
  ArduinoOTA.onEnd([] { logMsg("OTA-Update fertig - Neustart"); });
  ArduinoOTA.onError([](ota_error_t e) { logMsg("OTA-Update fehlgeschlagen, Fehler %u", e); });
  ArduinoOTA.begin(false);  // mDNS wird in setup() selbst verwaltet
}

void handleNotFound() {
  if (apMode) {
    // Captive Portal: alle unbekannten Anfragen auf die Konfigseite umleiten
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/config", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Nicht gefunden");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/config", HTTP_GET, protect([] { server.send_P(200, "text/html", CONFIG_HTML); }));
  server.on("/config", HTTP_POST, protect(handleSaveConfig));
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/config", HTTP_GET, protect(handleGetConfig));
  server.on("/api/scan", HTTP_GET, protect(handleScan));
  server.on("/update", HTTP_GET, protect([] { server.send_P(200, "text/html", UPDATE_HTML); }));
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/log", HTTP_GET, [] { server.send_P(200, "text/html", LOG_HTML); });
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/debug", HTTP_POST, handleDebug);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  logMsg("Personenzähler %s startet (Reset-Grund: %s)", FW_BUILD, ESP.getResetReason().c_str());

  WiFi.persistent(false);  // Zugangsdaten nicht zusätzlich im SDK-Flash speichern

  if (!LittleFS.begin()) {
    logMsg("LittleFS formatieren...");
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();
  loadCounts();
  logMsg("Konfig: SSID='%s', MQTT='%s:%u', Topic='%s', Schwelle=%u mm, invertiert=%s",
       cfg.wifiSsid.c_str(), cfg.mqttHost.c_str(), cfg.mqttPort, cfg.mqttTopic.c_str(),
       cfg.threshold, cfg.invert ? "ja" : "nein");
  if (!cfg.longRange && cfg.threshold > 1300)
    logMsg("WARNUNG: Schwelle %u mm liegt über der Reichweite des Short-Modus (~1300 mm)",
           cfg.threshold);
  logMsg("Zählerstand geladen: im Raum %ld, rein %lu, raus %lu", inside, totalIn, totalOut);

  sensorsOk = initSensors();

  if (!connectWifi()) startAccessPoint();

  if (!apMode) {
    MDNS.begin(cfg.hostname);
    MDNS.addService("http", "tcp", 80);
    MDNS.enableArduino(8266, true);  // OTA-Ziel in PlatformIO/Arduino IDE sichtbar machen
  }
  setupArduinoOTA();

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(1);
  wifiClient.setTimeout(1000);  // TCP-Connect zum Broker max. 1 s blockieren

  setupWebServer();
}

void loop() {
  // längste Pause zwischen zwei Loop-Durchläufen messen (Debug: blockiert etwas die Sensoren?)
  static uint32_t lastLoopAt = millis();
  static uint32_t maxLoopGap = 0;
  uint32_t now = millis();
  maxLoopGap = max(maxLoopGap, now - lastLoopAt);
  lastLoopAt = now;

  if (sensorsOk) {
    readSensor(laneA);
    readSensor(laneB);
    processPassage();

    // nicht während eines Durchgangs loggen - die serielle Ausgabe kostet Zeit
    uint32_t elapsed = now - lastDistLog;
    if (debugMode && !passageActive && elapsed > DEBUG_DIST_INTERVAL) {
      logMsg("A: %5d mm %s %2u Hz | B: %5d mm %s %2u Hz | max. Loop-Pause %u ms", laneA.dist,
             laneA.occ ? "[X]" : "[ ]", (unsigned)(laneA.samples * 1000UL / elapsed), laneB.dist,
             laneB.occ ? "[X]" : "[ ]", (unsigned)(laneB.samples * 1000UL / elapsed),
             (unsigned)maxLoopGap);
      lastDistLog = now;
      laneA.samples = laneB.samples = 0;
      maxLoopGap = 0;
    }
  }

  if (apMode) {
    dns.processNextRequest();
    // Periodisch neu versuchen, falls WLAN konfiguriert ist und niemand am AP hängt
    if (!cfg.wifiSsid.isEmpty() && WiFi.softAPgetStationNum() == 0 &&
        millis() - apStartedAt > AP_RETRY_INTERVAL) {
      logMsg("Kein Client am AP - Neustart für neuen WLAN-Versuch");
      if (countsDirty) saveCounts();
      ESP.restart();
    }
  } else {
    MDNS.update();
  }

  server.handleClient();
  ArduinoOTA.handle();
  mqttLoop();

  if (countsDirty && !passageActive && millis() - lastCountSave > COUNT_SAVE_INTERVAL) saveCounts();

  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
}
