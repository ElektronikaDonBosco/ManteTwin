/*
  ManteTwin - Motor baten mantentze prediktiboa
  ESP32 + MPU6050 -> MQTT -> Node-RED

  - 200 Hz-tan azelerazioa irakurri, 1 s-ko leihoak (200 lagin)
  - Leiho bakoitzeko ezaugarriak: RMS (X, Y, Z eta guztira), gailurra, crest factor, tenperatura
  - JSON-a MQTT bidez bidali  -> donbosco/mantetwin/motor1/datuak
  - Node-RED-en egoera jaso      <- donbosco/mantetwin/motor1/egoera
    (NORMALA = berdea, ABISUA = horia, ALARMA = gorria, IKASTEN = berdea + horia)

  SIMULAZIOA 1: Wokwi-n MPU6050-a ez da bibratzen, beraz potentziometroarekin
  bibrazio sintetikoa gehitzen da (akats bat simulatzeko). Hardware errealean: 0.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <ArduinoJson.h>

#define SIMULAZIOA 1

// ---------- Konfigurazioa ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

const char* MQTT_HOST = "host.wokwi.internal"; // IoT Gateway -> zure PCko Mosquitto (hardware errealean: PCaren IPa)
const uint16_t MQTT_PORT = 1883;
const char* TOPIC_DATUAK = "donbosco/mantetwin/motor1/datuak";
const char* TOPIC_EGOERA = "donbosco/mantetwin/motor1/egoera";

const int PIN_POT    = 34;   // akatsaren intentsitatea (simulazioa)
const int LED_BERDEA = 25;
const int LED_HORIA  = 26;
const int LED_GORRIA = 27;

const float G = 9.80665f;
const int FS = 200;                              // laginketa-maiztasuna (Hz)
const int N  = 200;                              // leihoa: N / FS = 1 s
const unsigned long PERIOD_US = 1000000UL / FS;

// ---------- Aldagaiak ----------
Adafruit_MPU6050 mpu;
WiFiClient net;
PubSubClient mqtt(net);

float ax[N], ay[N], az[N];
int idx = 0;
float tempC = 0;
unsigned long nextSample = 0;
unsigned long lastReconnect = 0;

// ---------- LEDak ----------
void setLeds(const String& egoera) {
  digitalWrite(LED_BERDEA, egoera == "NORMALA" || egoera == "IKASTEN");
  digitalWrite(LED_HORIA,  egoera == "ABISUA"  || egoera == "IKASTEN");
  digitalWrite(LED_GORRIA, egoera == "ALARMA");
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String egoera;
  for (unsigned int i = 0; i < len; i++) egoera += (char)payload[i];
  egoera.trim();
  Serial.printf("Egoera Node-RED-etik: %s\n", egoera.c_str());
  setLeds(egoera);
}

// ---------- Konexioak ----------
void connectWiFi() {
  Serial.print("WiFi-ra konektatzen");
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // 6 = kanala (Wokwi-n azkarrago). Hardware errealean kendu
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.printf("\nWiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
}

bool connectMQTT() {
  String clientId = "mantetwin-motor1-" + String((uint32_t)esp_random(), HEX);
  if (mqtt.connect(clientId.c_str())) {
    mqtt.subscribe(TOPIC_EGOERA);
    Serial.println("MQTT OK");
    return true;
  }
  Serial.printf("MQTT errorea, rc=%d\n", mqtt.state());
  return false;
}

// ---------- Simulazioa ----------
float zarata() { return random(-1000, 1001) / 1000.0f; }

// ---------- Leihoaren prozesamendua ----------
void prozesatuLeihoa() {
  float mx = 0, my = 0, mz = 0;
  for (int i = 0; i < N; i++) { mx += ax[i]; my += ay[i]; mz += az[i]; }
  mx /= N; my /= N; mz /= N;                     // DC osagaia (grabitatea + jarrera)

  float sx = 0, sy = 0, sz = 0, peak = 0;
  for (int i = 0; i < N; i++) {
    float dx = ax[i] - mx, dy = ay[i] - my, dz = az[i] - mz;
    sx += dx * dx; sy += dy * dy; sz += dz * dz;
    float m = sqrtf(dx * dx + dy * dy + dz * dz);
    if (m > peak) peak = m;
  }
  float rx = sqrtf(sx / N), ry = sqrtf(sy / N), rz = sqrtf(sz / N);
  float rms = sqrtf(rx * rx + ry * ry + rz * rz);
  float crest = rms > 0 ? peak / rms : 0;

  JsonDocument doc;
  doc["id"]    = "motor1";
  doc["rms"]   = roundf(rms * 10000) / 10000.0;
  doc["rmsX"]  = roundf(rx * 10000) / 10000.0;
  doc["rmsY"]  = roundf(ry * 10000) / 10000.0;
  doc["rmsZ"]  = roundf(rz * 10000) / 10000.0;
  doc["peak"]  = roundf(peak * 10000) / 10000.0;
  doc["crest"] = roundf(crest * 100) / 100.0;
  doc["temp"]  = roundf(tempC * 10) / 10.0;

  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(TOPIC_DATUAK, buf, n);
  Serial.println(buf);
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(LED_BERDEA, OUTPUT);
  pinMode(LED_HORIA, OUTPUT);
  pinMode(LED_GORRIA, OUTPUT);
  setLeds("IKASTEN");

  Wire.begin(21, 22);
  if (!mpu.begin()) {
    Serial.println("MPU6050 ez da aurkitu!");
    while (true) delay(1000);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
  mpu.setFilterBandwidth(MPU6050_BAND_94_HZ);

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);   // simulazioaren geldialdi txikiek konexioa ez mozteko
  connectMQTT();

  nextSample = micros();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected() && millis() - lastReconnect > 3000) {
    lastReconnect = millis();
    connectMQTT();
  }
  mqtt.loop();

  unsigned long now = micros();
  if ((long)(now - nextSample) < 0) return;
  // Blokeo luze baten ondoren (adib. birkonexioa) ez egin lagin-zaparradarik
  if (now - nextSample > 10 * PERIOD_US) nextSample = now;
  nextSample += PERIOD_US;

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  float x = a.acceleration.x / G;
  float y = a.acceleration.y / G;
  float z = a.acceleration.z / G;
  tempC = t.temperature;

#if SIMULAZIOA
  // Motor osasuntsua: zarata txikia. Potentziometroa: desoreka (25 Hz = 1500 rpm)
  // eta lerrokatze-akatsa (50 Hz, 2x) simulatzen ditu, 0..1 g.
  float amp = analogRead(PIN_POT) / 4095.0f;
  float ts = now / 1e6f;
  float v = amp * (sinf(2 * PI * 25 * ts) + 0.4f * sinf(2 * PI * 50 * ts));
  x += 0.02f * zarata();
  y += 0.02f * zarata() + 0.3f * v;
  z += 0.02f * zarata() + v;
#endif

  ax[idx] = x; ay[idx] = y; az[idx] = z;
  if (++idx >= N) { idx = 0; prozesatuLeihoa(); }
}
