#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DHT.h>
#include <Preferences.h>
#include <vector>
#include "mdns.h"

static const char *AP_PASS = "12345678";
static const int RESET_PIN = 0;  // Botón BOOT
static const int DHT_PIN = 4;
static const int MOSFET_PIN = 26;  // TRIG/PWM del módulo MOSFET (humidificador)
static const int EXTRACTOR_PIN = 27;  // IN del módulo relé del extractor
static const int HEATER_PIN = 25;     // IN del módulo relé de la calefacción
static const bool RELAY_ACTIVE_HIGH = false;  // Estos módulos se activan con señal baja (LOW = relé cerrado)
static const unsigned long EXTRACTOR_MAX_SEC = 86400;  // 24 h
static const unsigned long DHT_INTERVAL_MS = 2500;  // El DHT22 necesita al menos 2 s entre lecturas
static const unsigned long DISCOVERY_INTERVAL_MS = 30000;
static const uint32_t DISCOVERY_TIMEOUT_MS = 3000;
static const size_t MAX_PEERS = 20;
static const uint8_t PEER_MAX_MISSED = 3;  // Búsquedas seguidas sin respuesta antes de sacarlo de la lista

WebServer server(80);
DHT dht(DHT_PIN, DHT22);
Preferences prefs;

String deviceId;    // Últimos 3 bytes de la MAC, ej. "a1b2c3"
String deviceName;  // Nombre editable, también es el hostname (<nombre>.local) y la red del portal

struct Peer {
  String name;
  IPAddress ip;
  uint8_t missed;
};
std::vector<Peer> peers;
mdns_search_once_t *discovery = nullptr;
unsigned long lastDiscoveryAt = 0;

float lastTemperature = NAN;
float lastHumidity = NAN;
bool lastReadOk = false;
unsigned long lastReadAt = 0;

bool humEnabled = true;
int humMin = 85;
int humMax = 95;
bool humidifierOn = false;

bool heatEnabled = false;
float tempMin = 20.0;
float tempMax = 24.0;
bool heaterOn = false;

bool extEnabled = false;
unsigned long extOffSec = 900;
unsigned long extOnSec = 30;
bool extractorOn = false;
unsigned long phaseStartedAt = 0;

unsigned long extractorPhaseMs();

extern const char index_html[] asm("_binary_web_index_html_start");

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleStatus() {
  server.send(200, "application/json", "{\"status\":\"OK\"}");
}

void setHumidifier(bool on) {
  if (on == humidifierOn) return;
  humidifierOn = on;
  digitalWrite(MOSFET_PIN, on ? HIGH : LOW);
  Serial.println(on ? "Humidificador ENCENDIDO" : "Humidificador APAGADO");
}

// Histéresis: enciende por debajo del mínimo, apaga al llegar al máximo
void updateHumidifier() {
  if (!humEnabled || !lastReadOk) {
    setHumidifier(false);
  } else if (lastHumidity < humMin) {
    setHumidifier(true);
  } else if (lastHumidity >= humMax) {
    setHumidifier(false);
  }
}

void setRelay(int pin, bool on) {
  digitalWrite(pin, on == RELAY_ACTIVE_HIGH ? HIGH : LOW);
}

void setHeater(bool on) {
  if (on == heaterOn) return;
  heaterOn = on;
  setRelay(HEATER_PIN, on);
  Serial.println(on ? "Calefacción ENCENDIDA" : "Calefacción APAGADA");
}

// Histéresis: enciende por debajo de la mínima, apaga al llegar a la máxima
void updateHeater() {
  if (!heatEnabled || !lastReadOk) {
    setHeater(false);
  } else if (lastTemperature < tempMin) {
    setHeater(true);
  } else if (lastTemperature >= tempMax) {
    setHeater(false);
  }
}

void readSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  // Además de NaN, se descartan valores imposibles: con el cableado mal (ej. sin pull-up)
  // el DHT22 puede devolver todo en cero y pasar el checksum
  lastReadOk = !isnan(t) && !isnan(h) && !(t == 0 && h == 0) &&
               h >= 0 && h <= 100 && t >= -40 && t <= 80;
  if (lastReadOk) {
    lastTemperature = t;
    lastHumidity = h;
  } else {
    Serial.printf("Error leyendo el DHT22 (t=%.1f h=%.1f)\n", t, h);
  }
  updateHumidifier();
  updateHeater();
}

bool isValidName(const String &name) {
  if (name.length() < 1 || name.length() > 32) return false;
  if (name[0] == '-' || name[name.length() - 1] == '-') return false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

void startMdns() {
  if (!MDNS.begin(deviceName.c_str())) {
    Serial.println("Error iniciando mDNS");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("fungi", "tcp", 80);
  MDNS.addServiceTxt("fungi", "tcp", "name", deviceName);
  Serial.printf("Panel: http://%s.local\n", deviceName.c_str());
}

// Espera a que termine la búsqueda en curso (si la hay) y la libera
void cancelDiscovery() {
  if (!discovery) return;
  mdns_result_t *results = nullptr;
  mdns_query_async_get_results(discovery, DISCOVERY_TIMEOUT_MS, &results);
  if (results) mdns_query_results_free(results);
  mdns_query_async_delete(discovery);
  discovery = nullptr;
}

void mergeDiscoveryResults(mdns_result_t *results) {
  for (Peer &p : peers) p.missed++;

  for (mdns_result_t *r = results; r; r = r->next) {
    IPAddress ip;
    for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
      if (a->addr.type == ESP_IPADDR_TYPE_V4) {
        ip = IPAddress(a->addr.u_addr.ip4.addr);
        break;
      }
    }
    if (ip == IPAddress() || ip == WiFi.localIP()) continue;

    String name = r->hostname ? String(r->hostname) : ip.toString();
    for (size_t i = 0; i < r->txt_count; i++) {
      if (strcmp(r->txt[i].key, "name") == 0 && r->txt[i].value) name = r->txt[i].value;
    }

    bool found = false;
    for (Peer &p : peers) {
      if (p.ip == ip) {
        p.name = name;
        p.missed = 0;
        found = true;
        break;
      }
    }
    if (!found && peers.size() < MAX_PEERS) peers.push_back({name, ip, 0});
  }

  for (size_t i = peers.size(); i-- > 0;) {
    if (peers[i].missed >= PEER_MAX_MISSED) peers.erase(peers.begin() + i);
  }
}

// Busca otros controladores (_fungi._tcp) en segundo plano, sin bloquear el loop
void updateDiscovery() {
  if (discovery) {
    mdns_result_t *results = nullptr;
    if (!mdns_query_async_get_results(discovery, 0, &results)) return;
    mergeDiscoveryResults(results);
    if (results) mdns_query_results_free(results);
    mdns_query_async_delete(discovery);
    discovery = nullptr;
    return;
  }
  if (lastDiscoveryAt != 0 && millis() - lastDiscoveryAt < DISCOVERY_INTERVAL_MS) return;
  lastDiscoveryAt = millis();
  discovery = mdns_query_async_new(NULL, "_fungi", "_tcp", MDNS_TYPE_PTR, DISCOVERY_TIMEOUT_MS, MAX_PEERS, NULL);
}

String jsonString(const String &value) {
  String out = "\"";
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

String deviceJson() {
  return "{\"id\":" + jsonString(deviceId) +
         ",\"name\":" + jsonString(deviceName) +
         ",\"ip\":" + jsonString(WiFi.localIP().toString()) + "}";
}

void handleGetDevice() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", deviceJson());
}

void handlePostDevice() {
  String name = server.arg("name");
  name.trim();
  if (!isValidName(name)) {
    server.send(400, "application/json", "{\"error\":\"El nombre solo puede tener letras minúsculas, números y guiones (1 a 32, sin guion al principio ni al final)\"}");
    return;
  }
  if (name != deviceName) {
    deviceName = name;
    prefs.putString("name", deviceName);
    Serial.printf("Nuevo nombre: %s\n", deviceName.c_str());
    cancelDiscovery();
    MDNS.end();
    startMdns();
    WiFi.setHostname(deviceName.c_str());
  }
  server.send(200, "application/json", deviceJson());
}

void handleGetDevices() {
  String json = "[{\"name\":" + jsonString(deviceName) +
                ",\"ip\":" + jsonString(WiFi.localIP().toString()) + ",\"self\":true}";
  for (const Peer &p : peers) {
    json += ",{\"name\":" + jsonString(p.name) +
            ",\"ip\":" + jsonString(p.ip.toString()) + ",\"self\":false}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleSensors() {
  String json = "{\"temperature\":";
  json += isnan(lastTemperature) ? "null" : String(lastTemperature, 1);
  json += ",\"humidity\":";
  json += isnan(lastHumidity) ? "null" : String(lastHumidity, 1);
  json += ",\"ok\":";
  json += lastReadOk ? "true" : "false";
  json += ",\"humidifier\":";
  json += humidifierOn ? "true" : "false";
  json += ",\"heater\":";
  json += heaterOn ? "true" : "false";
  json += ",\"extractor\":";
  json += extractorOn ? "true" : "false";
  json += ",\"extractorRemaining\":";
  if (extEnabled) {
    unsigned long elapsed = millis() - phaseStartedAt;
    unsigned long phase = extractorPhaseMs();
    json += String(elapsed >= phase ? 0 : (phase - elapsed + 999) / 1000);
  } else {
    json += "null";
  }
  json += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

String configJson() {
  return "{\"enabled\":" + String(humEnabled ? "true" : "false") +
         ",\"humMin\":" + String(humMin) +
         ",\"humMax\":" + String(humMax) + "}";
}

void handleGetConfig() {
  server.send(200, "application/json", configJson());
}

void handlePostConfig() {
  if (!server.hasArg("humMin") || !server.hasArg("humMax")) {
    server.send(400, "application/json", "{\"error\":\"Faltan humMin y humMax\"}");
    return;
  }
  int newMin = server.arg("humMin").toInt();
  int newMax = server.arg("humMax").toInt();
  if (newMin < 0 || newMax > 100 || newMin >= newMax) {
    server.send(400, "application/json", "{\"error\":\"El mínimo debe ser menor que el máximo (0 a 100)\"}");
    return;
  }
  if (server.hasArg("enabled")) {
    humEnabled = server.arg("enabled") == "1";
    prefs.putBool("humEn", humEnabled);
  }
  humMin = newMin;
  humMax = newMax;
  prefs.putInt("humMin", humMin);
  prefs.putInt("humMax", humMax);
  Serial.printf("Humedad: activo=%d humMin=%d humMax=%d\n", humEnabled, humMin, humMax);
  // Con la config nueva se decide desde cero: solo queda encendido si está por debajo del mínimo
  setHumidifier(humEnabled && lastReadOk && lastHumidity < humMin);
  server.send(200, "application/json", configJson());
}

String heaterJson() {
  return "{\"enabled\":" + String(heatEnabled ? "true" : "false") +
         ",\"tempMin\":" + String(tempMin, 1) +
         ",\"tempMax\":" + String(tempMax, 1) + "}";
}

void handleGetHeater() {
  server.send(200, "application/json", heaterJson());
}

void handlePostHeater() {
  if (!server.hasArg("enabled") || !server.hasArg("tempMin") || !server.hasArg("tempMax")) {
    server.send(400, "application/json", "{\"error\":\"Faltan enabled, tempMin y tempMax\"}");
    return;
  }
  float newMin = server.arg("tempMin").toFloat();
  float newMax = server.arg("tempMax").toFloat();
  if (newMin < 0 || newMax > 50 || newMin >= newMax) {
    server.send(400, "application/json", "{\"error\":\"La mínima debe ser menor que la máxima (0 a 50 °C)\"}");
    return;
  }
  heatEnabled = server.arg("enabled") == "1";
  tempMin = newMin;
  tempMax = newMax;
  prefs.putBool("heatEn", heatEnabled);
  prefs.putFloat("tempMin", tempMin);
  prefs.putFloat("tempMax", tempMax);
  Serial.printf("Calefacción: activo=%d tempMin=%.1f tempMax=%.1f\n", heatEnabled, tempMin, tempMax);
  // Con la config nueva se decide desde cero: solo queda encendida si está por debajo de la mínima
  setHeater(heatEnabled && lastReadOk && lastTemperature < tempMin);
  server.send(200, "application/json", heaterJson());
}

void setExtractor(bool on) {
  extractorOn = on;
  phaseStartedAt = millis();
  setRelay(EXTRACTOR_PIN, on);
  Serial.println(on ? "Extractor ENCENDIDO" : "Extractor APAGADO");
}

// El ciclo arranca siempre con la fase encendida
void restartExtractorCycle() {
  setExtractor(extEnabled);
}

unsigned long extractorPhaseMs() {
  return (extractorOn ? extOnSec : extOffSec) * 1000UL;
}

void updateExtractor() {
  if (!extEnabled) return;
  if (millis() - phaseStartedAt >= extractorPhaseMs()) {
    setExtractor(!extractorOn);
  }
}

String extractorJson() {
  return "{\"enabled\":" + String(extEnabled ? "true" : "false") +
         ",\"offSec\":" + String(extOffSec) +
         ",\"onSec\":" + String(extOnSec) + "}";
}

void handleGetExtractor() {
  server.send(200, "application/json", extractorJson());
}

void handlePostExtractor() {
  if (!server.hasArg("enabled") || !server.hasArg("offSec") || !server.hasArg("onSec")) {
    server.send(400, "application/json", "{\"error\":\"Faltan enabled, offSec y onSec\"}");
    return;
  }
  long newOff = server.arg("offSec").toInt();
  long newOn = server.arg("onSec").toInt();
  if (newOff < 1 || newOn < 1 || newOff > (long)EXTRACTOR_MAX_SEC || newOn > (long)EXTRACTOR_MAX_SEC) {
    server.send(400, "application/json", "{\"error\":\"Los tiempos deben estar entre 1 segundo y 24 horas\"}");
    return;
  }
  extEnabled = server.arg("enabled") == "1";
  extOffSec = newOff;
  extOnSec = newOn;
  prefs.putBool("extEn", extEnabled);
  prefs.putULong("extOff", extOffSec);
  prefs.putULong("extOn", extOnSec);
  Serial.printf("Extractor: activo=%d apagado=%lus encendido=%lus\n", extEnabled, extOffSec, extOnSec);
  restartExtractorCycle();
  server.send(200, "application/json", extractorJson());
}

void setup() {
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, LOW);
  pinMode(EXTRACTOR_PIN, OUTPUT);
  setRelay(EXTRACTOR_PIN, false);
  pinMode(HEATER_PIN, OUTPUT);
  setRelay(HEATER_PIN, false);

  Serial.begin(115200);
  pinMode(RESET_PIN, INPUT_PULLUP);

  prefs.begin("config", false);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[7];
  snprintf(id, sizeof(id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  deviceId = id;
  deviceName = prefs.getString("name", "fungi-" + deviceId);
  if (!isValidName(deviceName)) deviceName = "fungi-" + deviceId;
  Serial.printf("\nDispositivo: %s (id %s)\n", deviceName.c_str(), deviceId.c_str());
  humEnabled = prefs.getBool("humEn", humEnabled);
  humMin = prefs.getInt("humMin", humMin);
  humMax = prefs.getInt("humMax", humMax);
  heatEnabled = prefs.getBool("heatEn", heatEnabled);
  tempMin = prefs.getFloat("tempMin", tempMin);
  tempMax = prefs.getFloat("tempMax", tempMax);
  extEnabled = prefs.getBool("extEn", extEnabled);
  extOffSec = prefs.getULong("extOff", extOffSec);
  extOnSec = prefs.getULong("extOn", extOnSec);

  dht.begin();

  WiFiManager wm;

  // Mantener BOOT presionado al arrancar borra las credenciales guardadas
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("Borrando credenciales WiFi...");
    wm.resetSettings();
  }

  // Tras un corte de luz el router puede tardar en volver: reintentar antes de abrir el portal
  wm.setConnectRetries(3);
  wm.setConnectTimeout(15);  // segundos por intento
  wm.setConfigPortalTimeout(180);
  WiFi.setHostname(deviceName.c_str());
  if (!wm.autoConnect(deviceName.c_str(), AP_PASS)) {
    Serial.println("No se pudo conectar, reiniciando...");
    ESP.restart();
  }

  Serial.print("Conectado. IP: ");
  Serial.println(WiFi.localIP());

  startMdns();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/sensors", handleSensors);
  server.on("/device", HTTP_GET, handleGetDevice);
  server.on("/device", HTTP_POST, handlePostDevice);
  server.on("/devices", HTTP_GET, handleGetDevices);
  server.on("/config", HTTP_GET, handleGetConfig);
  server.on("/config", HTTP_POST, handlePostConfig);
  server.on("/heater", HTTP_GET, handleGetHeater);
  server.on("/heater", HTTP_POST, handlePostHeater);
  server.on("/extractor", HTTP_GET, handleGetExtractor);
  server.on("/extractor", HTTP_POST, handlePostExtractor);
  server.begin();

  restartExtractorCycle();
}

void loop() {
  server.handleClient();

  if (millis() - lastReadAt >= DHT_INTERVAL_MS) {
    lastReadAt = millis();
    readSensor();
  }

  updateExtractor();
  updateDiscovery();
}
