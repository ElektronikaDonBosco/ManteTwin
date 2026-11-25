// ============================================================================
//  ENV III simulado (DHT22 = temperatura/humedad, BMP180 = presión) → MQTT
//  CIFP Don Bosco LHII
//
//  Publica cada 5 s en  mantetwin/env3/telemetry  (JSON)
//  Escucha el LED en    mantetwin/env3/led        ("on" / "off")
//  Estado (LWT)         mantetwin/env3/status     ("online" / "offline")
// ============================================================================
#include <WiFi.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>

#define DHT_PIN 4
#define LED_PIN 2

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
// Con el Private IoT Gateway, host.wokwi.internal es TU ordenador (donde corre Mosquitto)
const char* MQTT_HOST = "host.wokwi.internal";
const uint16_t MQTT_PORT = 1883;

const char* T_TELE   = "mantetwin/env3/telemetry";
const char* T_LED    = "mantetwin/env3/led";
const char* T_STATUS = "mantetwin/env3/status";

DHT dht(DHT_PIN, DHT22);
Adafruit_BMP085 bmp;
bool bmpOk = false;

WiFiClient net;
PubSubClient mqtt(net);
unsigned long lastSend = 0;

void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("<- %s: %s\n", topic, msg.c_str());
  if (String(topic) == T_LED) {
    bool on = (msg == "on" || msg == "1" || msg == "true");
    digitalWrite(LED_PIN, on ? HIGH : LOW);
  }
}

void connectWiFi() {
  Serial.print("WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // en hardware real: WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.printf(" OK (IP %s)\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.printf("MQTT %s:%u ... ", MQTT_HOST, MQTT_PORT);
    // Last Will: si el ESP32 desaparece, el broker publica "offline"
    if (mqtt.connect("env3-wokwi", T_STATUS, 1, true, "offline")) {
      Serial.println("OK");
      mqtt.publish(T_STATUS, "online", true);
      mqtt.subscribe(T_LED);
    } else {
      Serial.printf("error rc=%d, reintento en 2 s\n", mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  dht.begin();
  Wire.begin(21, 22);
  bmpOk = bmp.begin();
  Serial.println(bmpOk ? "BMP180 OK" : "BMP180 no encontrado");

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  if (millis() - lastSend < 5000) return;
  lastSend = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  float p = bmpOk ? bmp.readPressure() / 100.0 : NAN;   // Pa → hPa

  if (isnan(t) || isnan(h)) {
    Serial.println("DHT22: lectura fallida");
    return;
  }

  char js[128];
  snprintf(js, sizeof js, "{\"t\":%.1f,\"h\":%.1f,\"p\":%s,\"led\":%d}",
           t, h, isnan(p) ? "null" : String(p, 1).c_str(), digitalRead(LED_PIN));
  mqtt.publish(T_TELE, js);
  Serial.printf("-> %s\n", js);
}
