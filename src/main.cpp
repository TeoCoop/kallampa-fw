#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DHT.h>
#include <Preferences.h>
#include <vector>
#include "mdns.h"
#include "esp_system.h"
#include <stdarg.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>

#ifndef FW_VERSION
#define FW_VERSION "desconocida"
#endif

static const char *AP_PASS = "12345678";
static const int RESET_PIN = 0;  // Botón BOOT
static const int DHT_PIN = 4;
static const int MOSFET_PIN = 26;  // TRIG/PWM del módulo MOSFET (humidificador)
static const int EXTRACTOR_PIN = 27;  // IN del módulo relé del extractor
static const int HEATER_PIN = 25;     // IN del módulo relé de la calefacción
static const bool RELAY_ACTIVE_HIGH = false;  // Estos módulos se activan con señal baja (LOW = relé cerrado)
static const unsigned long EXTRACTOR_MAX_SEC = 86400;  // 24 h
static const unsigned long DHT_INTERVAL_MS = 2500;  // El DHT22 necesita al menos 2 s entre lecturas
static const unsigned long SENSOR_GRACE_MS = 2UL * 60 * 1000;  // Si falla, se sigue con la última lectura hasta 2 min
static const unsigned long DISCOVERY_INTERVAL_MS = 30000;
static const uint32_t DISCOVERY_TIMEOUT_MS = 3000;
static const size_t MAX_PEERS = 20;
static const uint8_t PEER_MAX_MISSED = 3;  // Búsquedas seguidas sin respuesta antes de sacarlo de la lista
static const size_t LOG_SIZE = 100;       // Mensajes que se guardan en RAM para el panel
static const size_t LOG_TEXT_LEN = 96;
static const uint16_t HISTORY_INTERVAL_MAX_MIN = 60;
static const size_t HISTORY_QUEUE_MAX = 144;       // Máximo de muestras pendientes sin conexión (RAM)
static const uint16_t HISTORY_HTTP_TIMEOUT_MS = 4000;
static const unsigned long HISTORY_TLS_TIMEOUT_SEC = 8;
static const time_t TIME_VALID_AFTER = 1700000000;  // Antes de esto el reloj todavía no se sincronizó por NTP
static const size_t TEMP_SMOOTH_N = 6;               // Lecturas promediadas para el control (~15 s)
static const uint8_t HEAT_LEARN_CYCLES = 2;          // Ciclos prendido/apagado que se miden antes de regular
static const unsigned long HEAT_MIN_PULSE_MS = 10000;  // Pulso mínimo del relé (prendido o apagado)
static const uint8_t HEAT_LIMIT_HITS_RELEARN = 3;    // Salidas del rango que hacen volver a aprender...
static const unsigned long HEAT_LIMIT_HITS_WINDOW_MS = 3UL * 3600 * 1000;  // ...si pasan dentro de 3 h

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
bool lastReadOk = false;      // Hay una lectura válida de hace menos de SENSOR_GRACE_MS (la usan los controles)
bool hasValidRead = false;
unsigned long lastValidAt = 0;
unsigned long sensorFailingSince = 0;  // 0 = el sensor está leyendo bien
bool sensorGraceExpired = false;
unsigned long lastReadAt = 0;

bool humEnabled = true;
int humMin = 85;
int humMax = 95;
bool humidifierOn = false;
unsigned long humidifierOnSince = 0;
unsigned long humidifierOnMs = 0;  // Tiempo encendido acumulado desde la última muestra del historial

bool heatEnabled = false;
float tempMin = 20.0;
float tempMax = 24.0;
uint16_t heaterWatts = 150;  // Potencia del calefactor, solo para estimar el consumo en el panel
bool heaterOn = false;
unsigned long heaterOnSince = 0;
unsigned long heaterOnMs = 0;

// Temperatura suavizada para el control (promedio de las últimas lecturas válidas)
float tempRing[TEMP_SMOOTH_N];
size_t tempRingCount = 0;
size_t tempRingPos = 0;

// Calefacción adaptativa: primero aprende cómo responde la carpa (con histéresis entre mínima y
// máxima) y después regula con un PI que prende el relé una parte de cada ventana de tiempo
enum HeatMode { HEAT_LEARNING, HEAT_PI };
HeatMode heatMode = HEAT_LEARNING;
bool modelValid = false;
float modelDeadSec = 0;  // Retardo entre prender/apagar y que el sensor lo note
float modelGain = 0;     // °C/min que sube con el calefactor al 100 % (sin contar pérdidas)
float modelLoss = 0;     // °C/min que baja con el calefactor apagado

// Ciclo de aprendizaje en curso
enum LearnPhase { CYC_IDLE, CYC_ON, CYC_OFF };
LearnPhase cycPhase = CYC_IDLE;
unsigned long cycOnAt = 0, cycLowAt = 0, cycOffAt = 0, cycPeakAt = 0;
float cycLow = 0, cycOffTemp = 0, cycPeak = 0;
uint8_t learnCount = 0;
float learnDeadSum = 0, learnGainSum = 0, learnLossSum = 0;

// Regulación PI
float piIntegral = 0;  // % de potencia
float heatPower = 0;   // % de la ventana actual que va prendido
unsigned long windowStart = 0;
unsigned long windowOnMs = 0;
int heatLimit = 0;     // 1 = por encima de la máxima, -1 = por debajo de la mínima
uint8_t limitHits = 0;
unsigned long firstLimitHitAt = 0;

bool extEnabled = false;
unsigned long extOffSec = 900;
unsigned long extOnSec = 30;
bool extractorOn = false;
unsigned long phaseStartedAt = 0;

// Historial en InfluxDB
bool histEnabled = false;
uint16_t historyIntervalMin = 5;  // Cada cuántos minutos se toma una muestra
String influxUrl;
String influxOrg;
String influxBucket;
String influxToken;
std::vector<String> historyQueue;  // Cada elemento es una muestra en line protocol (1 o 2 líneas)
unsigned long lastHistoryAt = 0;
time_t historyLastOk = 0;
String historyLastError;
bool timeSynced = false;

unsigned long extractorPhaseMs();

// ---- Registro: últimos mensajes en RAM (se pierden al reiniciar) ----
struct LogEntry {
  uint32_t seq;
  uint32_t ms;
  uint16_t repeat;
  char text[LOG_TEXT_LEN];
};
LogEntry logBuffer[LOG_SIZE];
uint32_t logSeq = 0;  // seq del último mensaje; el buffer guarda los últimos LOG_SIZE

// Escribe por serial y guarda en el registro. Si se repite el último mensaje, solo suma al contador.
void logMsg(const char *fmt, ...) {
  char text[LOG_TEXT_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  Serial.println(text);

  if (logSeq > 0) {
    LogEntry &last = logBuffer[(logSeq - 1) % LOG_SIZE];
    if (strcmp(last.text, text) == 0) {
      if (last.repeat < UINT16_MAX) last.repeat++;
      last.ms = millis();
      return;
    }
  }
  LogEntry &entry = logBuffer[logSeq % LOG_SIZE];
  entry.seq = ++logSeq;
  entry.ms = millis();
  entry.repeat = 1;
  strncpy(entry.text, text, sizeof(entry.text));
}

const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "encendido o corte de luz";
    case ESP_RST_SW: return "reinicio por software";
    case ESP_RST_PANIC: return "cuelgue (error del programa)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "cuelgue (watchdog)";
    case ESP_RST_BROWNOUT: return "caída de tensión (brownout)";
    case ESP_RST_EXT: return "botón de reset";
    case ESP_RST_DEEPSLEEP: return "salida de deep sleep";
    default: return "desconocido";
  }
}

extern const char index_html[] asm("_binary_web_index_html_start");

void handleRoot() {
  // send_P lo manda desde la flash de a pedazos: copiarlo a un String (~80 KB) puede no entrar en RAM
  server.send_P(200, "text/html", index_html, strlen(index_html));
}

void handleStatus() {
  server.send(200, "application/json", "{\"status\":\"OK\"}");
}

void setHumidifier(bool on) {
  if (on == humidifierOn) return;
  humidifierOn = on;
  if (on) humidifierOnSince = millis();
  else humidifierOnMs += millis() - humidifierOnSince;
  digitalWrite(MOSFET_PIN, on ? HIGH : LOW);
  logMsg(on ? "Humidificador ENCENDIDO" : "Humidificador APAGADO");
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
  if (on) heaterOnSince = millis();
  else heaterOnMs += millis() - heaterOnSince;
  setRelay(HEATER_PIN, on);
  logMsg(on ? "Calefacción ENCENDIDA" : "Calefacción APAGADA");
}

float controlTemperature() {
  if (!tempRingCount) return lastTemperature;
  float sum = 0;
  for (size_t i = 0; i < tempRingCount; i++) sum += tempRing[i];
  return sum / tempRingCount;
}

int currentHeatLimit() {
  return lastTemperature >= tempMax ? 1 : lastTemperature < tempMin ? -1 : 0;
}

float heatSetpoint() {
  return (tempMin + tempMax) / 2;
}

// Ventana del PWM lento: más larga cuanto más lenta es la carpa, para no gastar el relé
unsigned long heatWindowMs() {
  return (unsigned long)constrain(modelDeadSec * 2, 240.0f, 600.0f) * 1000UL;
}

// Ajuste del PI por reglas SIMC, con la carpa vista como un proceso integrador con retardo:
// sube modelGain °C/min al 100 % y el sensor tarda modelDeadSec (+ media ventana) en notarlo
float heatKc() {  // % de potencia por °C de diferencia
  float deadMin = modelDeadSec / 60 + heatWindowMs() / 120000.0f;
  return 100 / (modelGain * 2 * deadMin);
}

float heatTiMin() {  // Tiempo integral en minutos
  return 8 * (modelDeadSec / 60 + heatWindowMs() / 120000.0f);
}

void startHeatPi() {
  heatMode = HEAT_PI;
  // Arranca con la potencia que compensa las pérdidas medidas, sin salto
  piIntegral = constrain(100 * modelLoss / modelGain, 0.0f, 100.0f);
  heatPower = piIntegral;
  heatLimit = currentHeatLimit();
  limitHits = 0;
  windowStart = millis() - heatWindowMs();  // Calcula la primera ventana ya
  logMsg("Calefacción: regulando a %.1f °C (Kc %.0f %%/°C, Ti %.0f min, ventana %lu s)",
         heatSetpoint(), heatKc(), heatTiMin(), heatWindowMs() / 1000);
}

void startHeatLearning() {
  heatMode = HEAT_LEARNING;
  cycPhase = CYC_IDLE;
  learnCount = 0;
  learnDeadSum = learnGainSum = learnLossSum = 0;
  logMsg("Calefacción: aprendiendo cómo responde la carpa (%u ciclos)", HEAT_LEARN_CYCLES);
}

// Cierra un ciclo prendido → apagado → prendido y guarda lo que midió
void finishLearningCycle(float t, unsigned long now) {
  cycPhase = CYC_IDLE;
  float onMin = (cycOffAt - cycLowAt) / 60000.0f;
  float rise = cycOffTemp - cycLow;
  float coolMin = (now - cycPeakAt) / 60000.0f;
  float fall = cycPeak - t;
  if (onMin < 1 || rise < 0.2f || coolMin < 3 || fall < 0.1f) {
    logMsg("Aprendizaje: ciclo descartado (subió %.1f °C en %.1f min, bajó %.1f °C en %.1f min)", rise, onMin, fall, coolMin);
    return;
  }
  float loss = fall / coolMin;
  float gain = rise / onMin + loss;
  // Retardo: lo que tarda en empezar a subir al prender y en dejar de subir al apagar
  float dead = constrain(((cycLowAt - cycOnAt) + (cycPeakAt - cycOffAt)) / 2000.0f, 20.0f, 900.0f);
  learnDeadSum += dead;
  learnGainSum += gain;
  learnLossSum += loss;
  learnCount++;
  logMsg("Aprendizaje %u/%u: retardo %.0f s, sube %.2f °C/min, baja %.2f °C/min, se pasó %.1f °C",
         learnCount, HEAT_LEARN_CYCLES, dead, gain - loss, loss, cycPeak - cycOffTemp);
  if (learnCount < HEAT_LEARN_CYCLES) return;

  modelDeadSec = learnDeadSum / learnCount;
  modelGain = learnGainSum / learnCount;
  modelLoss = learnLossSum / learnCount;
  modelValid = true;
  prefs.putFloat("hDead", modelDeadSec);
  prefs.putFloat("hGain", modelGain);
  prefs.putFloat("hLoss", modelLoss);
  startHeatPi();
}

// Mide los ciclos de la histéresis: mínimo después de prender, pico después de apagar
void trackLearningCycle(float t) {
  unsigned long now = millis();
  if (heaterOn) {
    if (cycPhase == CYC_OFF) finishLearningCycle(t, now);
    if (heatMode != HEAT_LEARNING) return;
    if (cycPhase != CYC_ON) {
      cycPhase = CYC_ON;
      cycOnAt = now;
      cycLow = t;
      cycLowAt = now;
    } else if (t <= cycLow) {  // Último momento en el mínimo = cuando empieza a subir
      cycLow = t;
      cycLowAt = now;
    }
  } else if (cycPhase == CYC_ON) {
    cycPhase = CYC_OFF;
    cycOffAt = now;
    cycOffTemp = t;
    cycPeak = t;
    cycPeakAt = now;
  } else if (cycPhase == CYC_OFF && t > cycPeak) {  // Primer momento en el pico = cuando deja de subir
    cycPeak = t;
    cycPeakAt = now;
  }
}

// Si se sale seguido del rango, lo aprendido ya no sirve (cambió la carpa o el calefactor)
void countLimitHit() {
  unsigned long now = millis();
  if (!limitHits || now - firstLimitHitAt > HEAT_LIMIT_HITS_WINDOW_MS) {
    limitHits = 0;
    firstLimitHitAt = now;
  }
  if (++limitHits >= HEAT_LIMIT_HITS_RELEARN) {
    logMsg("Calefacción: se salió del rango %u veces en %lu h, vuelve a aprender", limitHits, HEAT_LIMIT_HITS_WINDOW_MS / 3600000);
    startHeatLearning();
  }
}

void updateHeatPi(float t) {
  unsigned long now = millis();
  unsigned long window = heatWindowMs();

  // La mínima y la máxima siguen siendo límites: fuera del rango manda la histéresis
  int limit = currentHeatLimit();
  if (limit != heatLimit) {
    heatLimit = limit;
    if (limit) {
      logMsg("Calefacción: %s, %s", limit > 0 ? "llegó a la máxima" : "bajó de la mínima", limit > 0 ? "se apaga" : "se prende");
      countLimitHit();
      if (heatMode != HEAT_PI) {
        setHeater(limit < 0);
        return;
      }
    } else {
      windowStart = now - window;  // Al volver al rango recalcula ya
    }
  }
  if (limit) {
    heatPower = limit < 0 ? 100 : 0;
    setHeater(limit < 0);
    return;
  }

  if (now - windowStart >= window) {
    windowStart = now;
    float error = heatSetpoint() - t;
    float kc = heatKc();
    float integral = constrain(piIntegral + kc * error * (window / 60000.0f) / heatTiMin(), 0.0f, 100.0f);
    float out = kc * error + integral;
    // Anti-windup: el integral no sigue creciendo si la salida ya está saturada para ese lado
    if (!((out > 100 && error > 0) || (out < 0 && error < 0))) piIntegral = integral;
    heatPower = constrain(kc * error + piIntegral, 0.0f, 100.0f);
    windowOnMs = heatPower / 100 * window;
    if (windowOnMs < HEAT_MIN_PULSE_MS) windowOnMs = 0;
    else if (window - windowOnMs < HEAT_MIN_PULSE_MS) windowOnMs = window;
  }
  setHeater(now - windowStart < windowOnMs);
}

void updateHeater() {
  if (!heatEnabled || !lastReadOk) {
    setHeater(false);
    cycPhase = CYC_IDLE;
    return;
  }
  float t = controlTemperature();
  if (heatMode == HEAT_PI) {
    updateHeatPi(t);
    return;
  }
  // Aprendiendo: histéresis, enciende por debajo de la mínima y apaga al llegar a la máxima
  if (lastTemperature < tempMin) {
    setHeater(true);
  } else if (lastTemperature >= tempMax) {
    setHeater(false);
  }
  trackLearningCycle(t);
}

// Después de cambiar la configuración se decide desde cero
void restartHeater() {
  if (heatMode == HEAT_PI) {
    heatLimit = currentHeatLimit();
    windowStart = millis() - heatWindowMs();
    updateHeater();
  } else {
    cycPhase = CYC_IDLE;
    setHeater(heatEnabled && lastReadOk && lastTemperature < tempMin);
  }
}

void readSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  // Además de NaN, se descartan valores imposibles: con el cableado mal (ej. sin pull-up)
  // el DHT22 puede devolver todo en cero y pasar el checksum
  bool valid = !isnan(t) && !isnan(h) && !(t == 0 && h == 0) &&
               h >= 0 && h <= 100 && t >= -40 && t <= 80;
  if (valid) {
    lastTemperature = t;
    lastHumidity = h;
    tempRing[tempRingPos] = t;
    tempRingPos = (tempRingPos + 1) % TEMP_SMOOTH_N;
    if (tempRingCount < TEMP_SMOOTH_N) tempRingCount++;
    hasValidRead = true;
    lastValidAt = millis();
    if (sensorFailingSince) {
      logMsg("DHT22: volvió a leer después de %lu s sin lecturas", (millis() - sensorFailingSince) / 1000);
      sensorFailingSince = 0;
    }
    sensorGraceExpired = false;
  } else {
    if (!sensorFailingSince) sensorFailingSince = millis();
    logMsg("Error leyendo el DHT22 (t=%.1f h=%.1f)", t, h);
  }

  // Una falla suelta no apaga nada: los controles siguen con la última lectura válida hasta 2 min
  lastReadOk = hasValidRead && millis() - lastValidAt < SENSOR_GRACE_MS;
  if (!lastReadOk && sensorFailingSince && !sensorGraceExpired) {
    sensorGraceExpired = true;
    logMsg("DHT22 sin lecturas por %lu min: se apagan humidificador y calefacción", SENSOR_GRACE_MS / 60000);
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
    logMsg("Error iniciando mDNS");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("fungi", "tcp", 80);
  MDNS.addServiceTxt("fungi", "tcp", "name", deviceName);
  MDNS.enableArduino(3232, false);  // Destino para actualizar por WiFi (OTA)
  logMsg("Panel: http://%s.local", deviceName.c_str());
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
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if ((uint8_t)c < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out + "\"";
}

String deviceJson() {
  return "{\"id\":" + jsonString(deviceId) +
         ",\"name\":" + jsonString(deviceName) +
         ",\"version\":" + jsonString(FW_VERSION) +
         ",\"ip\":" + jsonString(WiFi.localIP().toString()) + "}";
}

void handleLogs() {
  uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0;
  uint32_t oldest = logSeq > LOG_SIZE ? logSeq - LOG_SIZE + 1 : 1;
  uint32_t from = max(since + 1, oldest);

  String json = "{\"now\":" + String(millis()) + ",\"last\":" + String(logSeq) + ",\"entries\":[";
  for (uint32_t seq = from; seq <= logSeq; seq++) {
    const LogEntry &e = logBuffer[(seq - 1) % LOG_SIZE];
    if (seq != from) json += ",";
    json += "{\"seq\":" + String(e.seq) + ",\"ms\":" + String(e.ms) +
            ",\"repeat\":" + String(e.repeat) + ",\"text\":" + jsonString(e.text) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
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
    logMsg("Nuevo nombre: %s", deviceName.c_str());
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
  json += ",\"retrying\":";  // Falló la última lectura pero todavía se usa la anterior
  json += (lastReadOk && sensorFailingSince) ? "true" : "false";
  json += ",\"humidifier\":";
  json += humidifierOn ? "true" : "false";
  json += ",\"heater\":";
  json += heaterOn ? "true" : "false";
  json += ",\"heatMode\":";
  json += heatMode == HEAT_PI ? "\"pi\"" : "\"learning\"";
  json += ",\"heatPower\":";  // % de potencia del PI (null si no está regulando)
  json += heatEnabled && lastReadOk && heatMode == HEAT_PI ? String(heatPower, 0) : "null";
  json += ",\"heatLearnCycle\":";
  json += String(learnCount);
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
  logMsg("Humedad: activo=%d humMin=%d humMax=%d", humEnabled, humMin, humMax);
  // Con la config nueva se decide desde cero: solo queda encendido si está por debajo del mínimo
  setHumidifier(humEnabled && lastReadOk && lastHumidity < humMin);
  server.send(200, "application/json", configJson());
}

String heaterJson() {
  String json = "{\"enabled\":" + String(heatEnabled ? "true" : "false") +
                ",\"tempMin\":" + String(tempMin, 1) +
                ",\"tempMax\":" + String(tempMax, 1) +
                ",\"watts\":" + String(heaterWatts) +
                ",\"mode\":" + String(heatMode == HEAT_PI ? "\"pi\"" : "\"learning\"") +
                ",\"learnCycle\":" + String(learnCount) +
                ",\"learnCycles\":" + String(HEAT_LEARN_CYCLES) +
                ",\"setpoint\":" + String(heatSetpoint(), 2) +
                ",\"model\":";
  if (modelValid) {
    json += "{\"deadSec\":" + String(modelDeadSec, 0) +
            ",\"riseRate\":" + String(modelGain - modelLoss, 3) +
            ",\"lossRate\":" + String(modelLoss, 3) +
            ",\"windowSec\":" + String(heatWindowMs() / 1000) +
            ",\"kc\":" + String(heatKc(), 1) +
            ",\"tiMin\":" + String(heatTiMin(), 1) + "}";
  } else {
    json += "null";
  }
  return json + "}";
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
  long newWatts = server.hasArg("watts") ? server.arg("watts").toInt() : heaterWatts;
  if (newWatts < 1 || newWatts > 5000) {
    server.send(400, "application/json", "{\"error\":\"La potencia del calefactor tiene que ser de 1 a 5000 W\"}");
    return;
  }
  heatEnabled = server.arg("enabled") == "1";
  tempMin = newMin;
  tempMax = newMax;
  heaterWatts = newWatts;
  prefs.putUShort("heatW", heaterWatts);
  prefs.putBool("heatEn", heatEnabled);
  prefs.putFloat("tempMin", tempMin);
  prefs.putFloat("tempMax", tempMax);
  logMsg("Calefacción: activo=%d tempMin=%.1f tempMax=%.1f", heatEnabled, tempMin, tempMax);
  restartHeater();
  server.send(200, "application/json", heaterJson());
}

// Olvida lo aprendido de la carpa y vuelve a medir
void handleHeaterLearn() {
  startHeatLearning();
  restartHeater();
  server.send(200, "application/json", heaterJson());
}

void setExtractor(bool on) {
  extractorOn = on;
  phaseStartedAt = millis();
  setRelay(EXTRACTOR_PIN, on);
  logMsg(on ? "Extractor ENCENDIDO" : "Extractor APAGADO");
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
  logMsg("Extractor: activo=%d apagado=%lus encendido=%lus", extEnabled, extOffSec, extOnSec);
  restartExtractorCycle();
  server.send(200, "application/json", extractorJson());
}

// ---- Historial en InfluxDB ----

String urlEncode(const String &value) {
  String out;
  char hex[4];
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
      out += hex;
    }
  }
  return out;
}

bool historyConfigured() {
  return histEnabled && influxUrl.length() && influxOrg.length() && influxBucket.length() && influxToken.length();
}

// Devuelve los segundos encendido desde la última llamada y reinicia el acumulado
unsigned long takeOnSeconds(bool on, unsigned long &since, unsigned long &accumulated) {
  if (on) {
    accumulated += millis() - since;
    since = millis();
  }
  unsigned long seconds = (accumulated + 500) / 1000;
  accumulated = 0;
  return seconds;
}

// Toma una muestra (temperatura, humedad y tiempo encendido) y la agrega a la cola de pendientes
void takeHistorySample() {
  unsigned long humSec = takeOnSeconds(humidifierOn, humidifierOnSince, humidifierOnMs);
  unsigned long heatSec = takeOnSeconds(heaterOn, heaterOnSince, heaterOnMs);
  if (!historyConfigured()) return;

  time_t now = time(nullptr);
  if (now < TIME_VALID_AFTER) {
    logMsg("Historial: muestra descartada, la hora todavía no está sincronizada");
    return;
  }

  String tags = "device=" + deviceName + ",id=" + deviceId;
  String sample;
  if (lastReadOk) {
    sample += "ambiente," + tags + " temperatura=" + String(lastTemperature, 1) +
              ",humedad=" + String(lastHumidity, 1) + " " + String((uint32_t)now) + "\n";
  }
  sample += "actuadores," + tags + " humidificador_seg=" + String(humSec) + "i" +
            ",calefaccion_seg=" + String(heatSec) + "i " + String((uint32_t)now);

  // Hasta ~12 h de pendientes, con un tope de muestras para no llenar la RAM
  size_t maxQueue = min((size_t)(720 / historyIntervalMin), HISTORY_QUEUE_MAX);
  while (historyQueue.size() >= maxQueue) historyQueue.erase(historyQueue.begin());
  historyQueue.push_back(sample);
}

// Prepara un pedido a InfluxDB (http o https) con el token. path empieza con "/api/v2/..."
bool beginInflux(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &secure, const String &path) {
  String base = influxUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  http.setConnectTimeout(HISTORY_HTTP_TIMEOUT_MS);
  http.setTimeout(HISTORY_HTTP_TIMEOUT_MS);
  bool started;
  if (base.startsWith("https://")) {
    // Cifrado sin verificar el certificado del servidor (no hace falta cargar certificados)
    secure.setInsecure();
    secure.setHandshakeTimeout(HISTORY_TLS_TIMEOUT_SEC);
    started = http.begin(secure, base + path);
  } else {
    started = http.begin(plain, base + path);
  }
  if (started) http.addHeader("Authorization", "Token " + influxToken);
  return started;
}

// Texto corto del error de InfluxDB, que responde {"code":"...","message":"..."}
String influxError(HTTPClient &http, int code) {
  String detail = code > 0 ? http.getString() : HTTPClient::errorToString(code);
  int msgAt = detail.indexOf("\"message\":\"");
  if (msgAt >= 0) {
    int start = msgAt + 11;
    int end = detail.indexOf('"', start);
    detail = detail.substring(start, end > start ? end : detail.length());
  }
  if (detail.length() > 160) detail = detail.substring(0, 160);
  return code > 0 ? "error " + String(code) + ": " + detail : detail;
}

// Envía todas las muestras pendientes en un solo POST. Devuelve true si quedó la cola vacía.
bool sendHistory() {
  if (historyQueue.empty()) return true;
  if (WiFi.status() != WL_CONNECTED) {
    historyLastError = "sin WiFi";
    return false;
  }

  String body;
  for (const String &sample : historyQueue) body += sample + "\n";

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  String path = "/api/v2/write?org=" + urlEncode(influxOrg) +
                "&bucket=" + urlEncode(influxBucket) + "&precision=s";
  if (!beginInflux(http, plain, secure, path)) {
    historyLastError = "URL inválida";
    logMsg("Historial: URL inválida");
    return false;
  }
  http.addHeader("Content-Type", "text/plain; charset=utf-8");

  int code = http.POST(body);
  size_t count = historyQueue.size();
  if (code == 204) {
    historyQueue.clear();
    historyLastOk = time(nullptr);
    historyLastError = "";
    logMsg("Historial: %u muestra(s) enviada(s)", (unsigned)count);
    http.end();
    return true;
  }

  historyLastError = influxError(http, code);
  http.end();
  logMsg("Historial: %s", historyLastError.c_str());

  // Datos mal formados o demasiado grandes nunca van a entrar: se descartan para no trabar la cola
  if (code == 400 || code == 413) historyQueue.clear();
  return false;
}

void updateHistory() {
  if (!timeSynced && time(nullptr) >= TIME_VALID_AFTER) {
    timeSynced = true;
    logMsg("Hora sincronizada por NTP");
  }
  if (millis() - lastHistoryAt < historyIntervalMin * 60000UL) return;
  lastHistoryAt = millis();
  takeHistorySample();
  if (historyConfigured()) sendHistory();
}

String historyJson() {
  return "{\"enabled\":" + String(histEnabled ? "true" : "false") +
         ",\"url\":" + jsonString(influxUrl) +
         ",\"org\":" + jsonString(influxOrg) +
         ",\"bucket\":" + jsonString(influxBucket) +
         ",\"tokenSet\":" + String(influxToken.length() ? "true" : "false") +
         ",\"intervalMin\":" + String(historyIntervalMin) +
         ",\"pending\":" + String(historyQueue.size()) +
         ",\"lastOk\":" + (historyLastOk ? String((uint32_t)historyLastOk) : String("null")) +
         ",\"lastError\":" + jsonString(historyLastError) + "}";
}

void handleGetHistoryConfig() {
  server.send(200, "application/json", historyJson());
}

void handlePostHistoryConfig() {
  bool enabled = server.arg("enabled") == "1";
  String url = server.arg("url");
  String org = server.arg("org");
  String bucket = server.arg("bucket");
  String token = server.arg("token");
  url.trim();
  org.trim();
  bucket.trim();
  token.trim();

  if (url.length() && !url.startsWith("http://") && !url.startsWith("https://")) {
    server.send(400, "application/json", "{\"error\":\"La URL tiene que empezar con http:// o https://\"}");
    return;
  }
  long interval = server.hasArg("interval") ? server.arg("interval").toInt() : historyIntervalMin;
  if (interval < 1 || interval > HISTORY_INTERVAL_MAX_MIN) {
    server.send(400, "application/json", "{\"error\":\"El intervalo tiene que ser de 1 a 60 minutos\"}");
    return;
  }
  if (enabled && (!url.length() || !org.length() || !bucket.length() || (!token.length() && !influxToken.length()))) {
    server.send(400, "application/json", "{\"error\":\"Para activarlo completá URL, organización, bucket y token\"}");
    return;
  }

  histEnabled = enabled;
  influxUrl = url;
  influxOrg = org;
  influxBucket = bucket;
  if (token.length()) influxToken = token;
  if (interval != historyIntervalMin) {
    historyIntervalMin = interval;
    lastHistoryAt = millis();  // El nuevo intervalo empieza a contar ahora
  }
  prefs.putUShort("histInt", historyIntervalMin);
  prefs.putBool("histEn", histEnabled);
  prefs.putString("influxUrl", influxUrl);
  prefs.putString("influxOrg", influxOrg);
  prefs.putString("influxBucket", influxBucket);
  prefs.putString("influxToken", influxToken);
  historyLastError = "";
  if (!histEnabled) historyQueue.clear();
  logMsg("Historial: activo=%d cada %u min url=%s bucket=%s", histEnabled, historyIntervalMin, influxUrl.c_str(), influxBucket.c_str());
  server.send(200, "application/json", historyJson());
}

// Reenvía al navegador, pedazo a pedazo, lo que llega de InfluxDB (sin guardarlo entero en RAM)
class ServerStream : public Stream {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    server.sendContent((const char *)buffer, size);
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

String fluxString(const String &value) {
  String out = "\"";
  for (size_t i = 0; i < value.length(); i++) {
    if (value[i] == '"' || value[i] == '\\') out += '\\';
    out += value[i];
  }
  return out + "\"";
}

// Datos para los gráficos, en CSV: valor promedio y segundos prendido por ventana.
// kind=temp (por defecto): temperatura y calefacción; kind=hum: humedad y humidificador
void handleHistory() {
  if (!historyConfigured()) {
    server.send(409, "application/json", "{\"error\":\"Configurá el historial (InfluxDB) para ver gráficos\"}");
    return;
  }
  String range = server.arg("range");
  // Ventana "redonda" para que queden como mucho ~360 barras, nunca menor al intervalo de muestreo
  uint16_t stepMin;
  if (range == "1h" || range == "2h" || range == "6h") stepMin = 1;
  else if (range == "7d") stepMin = 30;
  else {
    range = "24h";
    stepMin = 5;
  }
  uint16_t windowMin = max(stepMin, historyIntervalMin);
  bool hum = server.arg("kind") == "hum";
  String valueField = hum ? "humedad" : "temperatura";
  String onField = hum ? "humidificador_seg" : "calefaccion_seg";

  String every = String(windowMin) + "m";
  String query =
      "base = from(bucket: " + fluxString(influxBucket) + ")\n"
      "  |> range(start: -" + range + ")\n"
      "  |> filter(fn: (r) => r.id == " + fluxString(deviceId) + ")\n"
      "t = base\n"
      "  |> filter(fn: (r) => r._measurement == \"ambiente\" and r._field == \"" + valueField + "\")\n"
      "  |> aggregateWindow(every: " + every + ", fn: mean, createEmpty: false, timeSrc: \"_start\")\n"
      "h = base\n"
      "  |> filter(fn: (r) => r._measurement == \"actuadores\" and r._field == \"" + onField + "\")\n"
      "  |> aggregateWindow(every: " + every + ", fn: sum, createEmpty: false, timeSrc: \"_start\")\n"
      // Los segundos son enteros y la temperatura no: sin esto el pivot choca por tipos distintos en _value
      "  |> toFloat()\n"
      "union(tables: [t, h])\n"
      "  |> keep(columns: [\"_time\", \"_field\", \"_value\"])\n"
      "  |> group()\n"
      "  |> pivot(rowKey: [\"_time\"], columnKey: [\"_field\"], valueColumn: \"_value\")\n"
      "  |> sort(columns: [\"_time\"])\n";
  String body = "{\"query\":" + jsonString(query) +
                ",\"dialect\":{\"annotations\":[],\"header\":true}}";

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginInflux(http, plain, secure, "/api/v2/query?org=" + urlEncode(influxOrg))) {
    server.send(502, "application/json", "{\"error\":\"URL de InfluxDB inválida\"}");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/csv");

  int code = http.POST(body);
  if (code != 200) {
    String error = influxError(http, code);
    http.end();
    if (code == 401 || code == 403) error += " (el token necesita permiso de lectura)";
    logMsg("Gráfico: %s", error.c_str());
    server.send(502, "application/json", "{\"error\":" + jsonString(error) + "}");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("X-Window-Min", String(windowMin));
  server.send(200, "text/csv", "");
  ServerStream out;
  http.writeToStream(&out);
  server.sendContent("");
  http.end();
}

// Toma una muestra ya y la envía, para probar la configuración sin esperar 5 minutos
void handleHistoryTest() {
  if (!historyConfigured()) {
    server.send(400, "application/json", "{\"error\":\"Activá el historial y guardá la configuración primero\"}");
    return;
  }
  if (time(nullptr) < TIME_VALID_AFTER) {
    server.send(400, "application/json", "{\"error\":\"La hora todavía no está sincronizada (NTP); probá en unos segundos\"}");
    return;
  }
  lastHistoryAt = millis();
  takeHistorySample();
  if (sendHistory()) {
    server.send(200, "application/json", historyJson());
  } else {
    server.send(502, "application/json", "{\"error\":" + jsonString(historyLastError) + "}");
  }
}

// Actualización por WiFi desde la PC (./upload-ota.sh). Sin clave: la red es de confianza.
void startOta() {
  ArduinoOTA.setHostname(deviceName.c_str());
  ArduinoOTA.setMdnsEnabled(false);  // mDNS lo maneja startMdns()
  ArduinoOTA.onStart([]() {
    // Durante la actualización el loop no corre: se apagan todos los aparatos por seguridad
    setHumidifier(false);
    setHeater(false);
    extractorOn = false;
    setRelay(EXTRACTOR_PIN, false);
    logMsg("Actualización OTA iniciada");
  });
  ArduinoOTA.onEnd([]() {
    logMsg("Actualización OTA completa, reiniciando");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    const char *reason = error == OTA_AUTH_ERROR ? "autenticación"
                       : error == OTA_BEGIN_ERROR ? "no hay espacio o partición inválida"
                       : error == OTA_CONNECT_ERROR ? "conexión"
                       : error == OTA_RECEIVE_ERROR ? "recepción"
                       : error == OTA_END_ERROR ? "verificación final"
                       : "desconocido";
    logMsg("Error en actualización OTA (%s); sigue la versión anterior", reason);
    restartExtractorCycle();
  });
  ArduinoOTA.begin();
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
  Serial.println();
  logMsg("Inicio (motivo: %s)", resetReasonText());
  logMsg("Dispositivo: %s (id %s) · versión %s", deviceName.c_str(), deviceId.c_str(), FW_VERSION);
  humEnabled = prefs.getBool("humEn", humEnabled);
  humMin = prefs.getInt("humMin", humMin);
  humMax = prefs.getInt("humMax", humMax);
  heatEnabled = prefs.getBool("heatEn", heatEnabled);
  tempMin = prefs.getFloat("tempMin", tempMin);
  tempMax = prefs.getFloat("tempMax", tempMax);
  heaterWatts = prefs.getUShort("heatW", heaterWatts);
  // Lo aprendido de la carpa sobrevive reinicios; sin eso arranca aprendiendo
  if (prefs.isKey("hGain")) {
    modelDeadSec = prefs.getFloat("hDead", 0);
    modelGain = prefs.getFloat("hGain", 0);
    modelLoss = prefs.getFloat("hLoss", 0);
    modelValid = modelDeadSec > 0 && modelGain > 0 && modelLoss >= 0;
  }
  if (modelValid) {
    logMsg("Calefacción: modelo guardado (retardo %.0f s, sube %.2f °C/min, baja %.2f °C/min)",
           modelDeadSec, modelGain - modelLoss, modelLoss);
    startHeatPi();
  }
  extEnabled = prefs.getBool("extEn", extEnabled);
  extOffSec = prefs.getULong("extOff", extOffSec);
  extOnSec = prefs.getULong("extOn", extOnSec);
  histEnabled = prefs.getBool("histEn", histEnabled);
  influxUrl = prefs.getString("influxUrl", "");
  influxOrg = prefs.getString("influxOrg", "");
  influxBucket = prefs.getString("influxBucket", "");
  influxToken = prefs.getString("influxToken", "");
  historyIntervalMin = prefs.getUShort("histInt", historyIntervalMin);
  if (historyIntervalMin < 1 || historyIntervalMin > HISTORY_INTERVAL_MAX_MIN) historyIntervalMin = 5;

  dht.begin();

  WiFiManager wm;

  // Mantener BOOT presionado al arrancar borra las credenciales guardadas
  if (digitalRead(RESET_PIN) == LOW) {
    logMsg("Borrando credenciales WiFi...");
    wm.resetSettings();
  }

  // Tras un corte de luz el router puede tardar en volver: reintentar antes de abrir el portal
  wm.setConnectRetries(3);
  wm.setConnectTimeout(15);  // segundos por intento
  wm.setConfigPortalTimeout(180);
  WiFi.setHostname(deviceName.c_str());
  logMsg("Conectando a la WiFi guardada...");
  if (!wm.autoConnect(deviceName.c_str(), AP_PASS)) {
    logMsg("No se pudo conectar, reiniciando...");
    ESP.restart();
  }

  logMsg("Conectado a %s. IP: %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC; el historial guarda epoch

  startMdns();
  startOta();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/sensors", handleSensors);
  server.on("/device", HTTP_GET, handleGetDevice);
  server.on("/device", HTTP_POST, handlePostDevice);
  server.on("/devices", HTTP_GET, handleGetDevices);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/history-config", HTTP_GET, handleGetHistoryConfig);
  server.on("/history-config", HTTP_POST, handlePostHistoryConfig);
  server.on("/history-test", HTTP_POST, handleHistoryTest);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/config", HTTP_GET, handleGetConfig);
  server.on("/config", HTTP_POST, handlePostConfig);
  server.on("/heater", HTTP_GET, handleGetHeater);
  server.on("/heater", HTTP_POST, handlePostHeater);
  server.on("/heater-learn", HTTP_POST, handleHeaterLearn);
  server.on("/extractor", HTTP_GET, handleGetExtractor);
  server.on("/extractor", HTTP_POST, handlePostExtractor);
  server.begin();

  restartExtractorCycle();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (millis() - lastReadAt >= DHT_INTERVAL_MS) {
    lastReadAt = millis();
    readSensor();
  }

  updateExtractor();
  updateDiscovery();
  updateHistory();
}
