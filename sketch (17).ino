// ============================================================================
//  ManteTwin - Akats-eszenatokien nodoa (ESP32)
//  CIFP Don Bosco LHII - Tknika 2026-27
//
//  SCENARIO 1 : Sentsorearen deriba (kalibrazio-galera) -> SHT30 nagusia vs erreferentzia
//  SCENARIO 2 : Sentsore blokeatua / I2C errorea (balio izoztua, NACK, CRC)
//  SCENARIO 5 : Komunikazio-akatsak (RSSI baxua, pakete-galera, lotura etena)
//
//  Firmware bera erabil daiteke ESP32 + M5Stack ENV III errealarekin
//  (SHT30 0x44 helbidean). Horretarako aldatu WIFI_* eta MQTT_HOST.
// ============================================================================

#define SCENARIO 5
#define NODE_ID  "mt-comms"

#include <WiFi.h>
#include <Wire.h>
#include <PubSubClient.h>

// ---------------- Konfigurazioa ----------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// Wokwi Private IoT Gateway-rekin "host.wokwi.internal" zure ordenagailua da
// (bertan Mosquitto Docker-ean). Proba azkarretarako: "broker.hivemq.com".
const char* MQTT_HOST = "host.wokwi.internal";
const uint16_t MQTT_PORT = 1883;

// Pinak
const int PIN_SDA     = 21;
const int PIN_SCL     = 22;
const int PIN_FLT     = 34;  // custom chip-aren FLT -> akatsa injektatuta (ground truth)
const int PIN_LED_OK  = 25;  // berdea: nodoak dena ondo ikusten du
const int PIN_LED_ERR = 26;  // gorria: nodoak (Edge) akatsa detektatu du
const int PIN_LED_INJ = 27;  // horia: akatsa injektatuta dago (etiketa)
#if SCENARIO == 5
const int PIN_POT_ATT  = 32; // seinalearen ahultzea 0..40 dB
const int PIN_POT_LOSS = 33; // pakete-galera 0..50 %
const int PIN_SW_LINK  = 14; // HIGH = brokerrarekiko lotura etenda
#endif

const uint8_t ADDR_MAIN = 0x44;
const uint8_t ADDR_REF  = 0x45;

// ---------------- Topikoak ----------------
String T_TELE, T_STATUS, T_CMD, T_PING, T_EVENT;

WiFiClient net;
PubSubClient mqtt(net);

unsigned long telemetryInterval = 2000;
unsigned long lastTele = 0;
unsigned long seq = 0;
unsigned long reconnects = 0;

// Diagnostiko-kontagailuak
unsigned long i2cNack = 0, i2cCrc = 0;

// ---------------- SHT30 irakurketa ----------------
enum ReadResult { RD_OK, RD_NACK, RD_CRC };

uint8_t crc8(const uint8_t* d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

ReadResult sht30Read(uint8_t addr, float& t, float& h, uint16_t& rawT) {
  Wire.beginTransmission(addr);
  Wire.write(0x24);
  Wire.write(0x00);                       // single shot, errepikakortasun altua
  if (Wire.endTransmission() != 0) return RD_NACK;
  delay(16);
  if (Wire.requestFrom((int)addr, 6) != 6) return RD_NACK;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return RD_CRC;
  rawT = ((uint16_t)b[0] << 8) | b[1];
  uint16_t rawH = ((uint16_t)b[3] << 8) | b[4];
  t = -45.0f + 175.0f * rawT / 65535.0f;
  h = 100.0f * rawH / 65535.0f;
  return RD_OK;
}

// ---------------- Eszenatoki bakoitzaren egoera ----------------
#if SCENARIO == 1
float dTema = 0;                  // deltaren batez besteko mugikorra (EMA)
const float DRIFT_WARN  = 0.5f;   // °C
const float DRIFT_ALARM = 1.5f;   // °C
#endif

#if SCENARIO == 2
const int STUCK_N = 8;            // 8 neurketa berdin jarraian -> blokeatuta
uint16_t lastRaw[STUCK_N];
int rawIdx = 0, rawCount = 0;
unsigned long winReads = 0, winNack = 0, winCrc = 0;   // leiho mugikorra
#endif

#if SCENARIO == 5
// Ping bidezko latentzia eta galera
unsigned long lastPing = 0;
unsigned long pingSent = 0, pingRecv = 0;
float rttEma = -1;
unsigned long txDropped = 0;
bool linkWasCut = false;
unsigned long linkCutSince = 0;
// Ping atzeratua (seinale ahularen birbidalketak simulatzeko)
bool pendingPing = false;
unsigned long pendingRelease = 0, pendingSentAt = 0;
float attDb = 0, lossPct = 0;
#endif

// ---------------- WiFi / MQTT ----------------
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.printf(" OK  IP=%s\n", WiFi.localIP().toString().c_str());
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

#if SCENARIO == 5
  if (t == T_PING) {
    unsigned long sentAt = strtoul(msg.c_str(), nullptr, 10);
    // Seinale ahularekin birbidalketak -> latentzia gehigarria
    unsigned long extra = (attDb > 20) ? random(0, (long)(attDb * 15)) : 0;
    pendingPing = true;
    pendingSentAt = sentAt;
    pendingRelease = millis() + extra;
    return;
  }
#endif

  if (t == T_CMD) {
    Serial.printf("CMD: %s\n", msg.c_str());
    if (msg.startsWith("interval=")) {
      unsigned long v = msg.substring(9).toInt();
      if (v >= 500 && v <= 60000) telemetryInterval = v;
    } else if (msg == "reset") {
      i2cNack = i2cCrc = 0;
#if SCENARIO == 2
      rawCount = 0; winReads = winNack = winCrc = 0;
#endif
#if SCENARIO == 5
      pingSent = pingRecv = 0; txDropped = 0; rttEma = -1;
#endif
    }
    mqtt.publish(T_EVENT.c_str(), (String("{\"cmd_ack\":\"") + msg + "\"}").c_str());
  }
}

void mqttConnect() {
  while (!mqtt.connected()) {
    Serial.printf("MQTT %s:%u ... ", MQTT_HOST, MQTT_PORT);
    String cid = String(NODE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    // LWT: lotura galtzean brokerrak "offline" argitaratzen du
    if (mqtt.connect(cid.c_str(), T_STATUS.c_str(), 1, true, "offline")) {
      Serial.println("OK");
      mqtt.publish(T_STATUS.c_str(), "online", true);
      mqtt.subscribe(T_CMD.c_str());
#if SCENARIO == 5
      mqtt.subscribe(T_PING.c_str());
#endif
      reconnects++;
    } else {
      Serial.printf("errorea rc=%d, 2 s barru berriro\n", mqtt.state());
      delay(2000);
    }
  }
}

// Argitaratu; 5. eszenatokian pakete-galera simulatzen da
bool publishTele(const String& topic, const String& payload) {
#if SCENARIO == 5
  if (random(0, 1000) < (long)(lossPct * 10)) { txDropped++; return false; }
#endif
  return mqtt.publish(topic.c_str(), payload.c_str());
}

void setLeds(bool detected, bool injected) {
  digitalWrite(PIN_LED_OK, !detected);
  digitalWrite(PIN_LED_ERR, detected);
  digitalWrite(PIN_LED_INJ, injected);
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_LED_ERR, OUTPUT);
  pinMode(PIN_LED_INJ, OUTPUT);
  pinMode(PIN_FLT, INPUT);
#if SCENARIO == 5
  pinMode(PIN_SW_LINK, INPUT);
#endif
  Wire.begin(PIN_SDA, PIN_SCL);
  randomSeed(esp_random());

  String base = String("mantetwin/") + NODE_ID;
  T_TELE   = base + "/telemetry";
  T_STATUS = base + "/status";
  T_CMD    = base + "/cmd";
  T_PING   = base + "/ping";
  T_EVENT  = base + "/event";

  Serial.printf("\n=== ManteTwin nodoa %s | eszenatokia %d ===\n", NODE_ID, SCENARIO);
  wifiConnect();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
#if SCENARIO == 5
  mqtt.setKeepAlive(5);    // lotura etenak azkar detektatzeko (LWT)
#else
  mqtt.setKeepAlive(60);   // simulazioaren geldialdi txikiek ez dezaten konexioa moztu
#endif
  mqttConnect();
}

// ---------------- Loop ----------------
void loop() {
  unsigned long now = millis();

#if SCENARIO == 5
  // --- Akatsen injekzioa (potentziometroak eta etengailua) ---
  attDb   = analogRead(PIN_POT_ATT)  * 40.0f / 4095.0f;
  lossPct = analogRead(PIN_POT_LOSS) * 50.0f / 4095.0f;
  bool linkCut = digitalRead(PIN_SW_LINK) == HIGH;

  if (linkCut) {
    // Ez dugu mqtt.loop() deitzen: keepalive-a iraungi eta brokerrak LWT bidaltzen du
    if (!linkWasCut) {
      linkWasCut = true;
      linkCutSince = now;
      Serial.println("!! Lotura etenda (simulatua)");
    }
    setLeds(true, true);
    delay(50);
    return;
  }
  if (linkWasCut) {
    // Berreskurapena: konexio berria eta etenaldiaren gertaera (store & forward)
    unsigned long outage = now - linkCutSince;
    linkWasCut = false;
    mqtt.disconnect();
    mqttConnect();
    mqtt.publish(T_EVENT.c_str(),
                 (String("{\"event\":\"link_restored\",\"outage_ms\":") + outage + "}").c_str());
  }
#endif

  if (WiFi.status() != WL_CONNECTED) wifiConnect();
  if (!mqtt.connected()) mqttConnect();
  mqtt.loop();

#if SCENARIO == 5
  // Ping atzeratua askatu
  if (pendingPing && now >= pendingRelease) {
    pendingPing = false;
    float rtt = (float)(now - pendingSentAt);
    rttEma = (rttEma < 0) ? rtt : 0.8f * rttEma + 0.2f * rtt;
    pingRecv++;
  }
  // Ping bat 2 segundoan behin (galera-tasa eta latentzia neurtzeko)
  if (now - lastPing >= 2000) {
    lastPing = now;
    pingSent++;
    publishTele(T_PING, String(now));
  }
#endif

  if (now - lastTele < telemetryInterval) return;
  lastTele = now;
  seq++;

  float t = NAN, h = NAN;
  uint16_t rawT = 0;
  ReadResult r = sht30Read(ADDR_MAIN, t, h, rawT);
  if (r == RD_NACK) i2cNack++;
  if (r == RD_CRC)  i2cCrc++;

  String st = "OK";
  bool injected = digitalRead(PIN_FLT) == HIGH;
  String extra;

#if SCENARIO == 1
  // ---- Deriba: sentsore nagusia erreferentziarekin konparatu ----
  float tr = NAN, hr = NAN;
  uint16_t rawR = 0;
  ReadResult rr = sht30Read(ADDR_REF, tr, hr, rawR);
  if (r == RD_OK && rr == RD_OK) {
    float dT = t - tr;
    dTema = 0.9f * dTema + 0.1f * dT;
    if (fabs(dTema) > DRIFT_ALARM)     st = "DRIFT_ALARM";
    else if (fabs(dTema) > DRIFT_WARN) st = "DRIFT_WARN";
    extra = String(",\"t_ref\":") + String(tr, 2) +
            ",\"dT\":" + String(dT, 3) + ",\"dT_ema\":" + String(dTema, 3);
  } else {
    st = "SENSOR_ERR";
  }
#endif

#if SCENARIO == 2
  // ---- Sentsore blokeatua / I2C ----
  winReads++;
  if (r == RD_NACK) winNack++;
  if (r == RD_CRC)  winCrc++;
  if (winReads >= 30) { winReads /= 2; winNack /= 2; winCrc /= 2; }   // leiho "leuna"
  float nackRatio = (float)winNack / (float)winReads;
  float crcRatio  = (float)winCrc  / (float)winReads;

  if (r == RD_OK) {
    lastRaw[rawIdx] = rawT;
    rawIdx = (rawIdx + 1) % STUCK_N;
    if (rawCount < STUCK_N) rawCount++;
  }
  bool stuck = false;
  if (rawCount == STUCK_N) {
    stuck = true;
    for (int i = 1; i < STUCK_N; i++) if (lastRaw[i] != lastRaw[0]) { stuck = false; break; }
  }

  if (nackRatio > 0.8f)       st = "SENSOR_DEAD";
  else if (stuck)             st = "STUCK";
  else if (crcRatio > 0.10f)  st = "BUS_NOISE";
  else if (nackRatio > 0.05f) st = "INTERMITTENT";
  extra = String(",\"nack_ratio\":") + String(nackRatio, 3) +
          ",\"crc_ratio\":" + String(crcRatio, 3) + ",\"stuck\":" + (stuck ? 1 : 0);
#endif

#if SCENARIO == 5
  // ---- Komunikazioak ----
  int rssi = WiFi.RSSI() - (int)attDb;
  float pingLoss = (pingSent > 0) ? 1.0f - (float)pingRecv / (float)pingSent : 0;
  if (pingSent >= 20) { pingSent /= 2; pingRecv /= 2; }  // leiho leuna
  if (pingLoss > 0.10f)  st = "PACKET_LOSS";
  else if (rssi < -80)   st = "WEAK_SIGNAL";
  else if (rttEma > 300) st = "HIGH_LATENCY";
  injected = (attDb > 20) || (lossPct > 5);
  extra = String(",\"rssi\":") + rssi + ",\"rtt_ms\":" + String(rttEma, 0) +
          ",\"ping_loss\":" + String(pingLoss, 3) + ",\"tx_dropped\":" + txDropped +
          ",\"att_db\":" + String(attDb, 1) + ",\"loss_inj\":" + String(lossPct, 1);
#endif

  bool detected = (st != "OK");
  setLeds(detected, injected);

  String js = String("{\"node\":\"") + NODE_ID + "\",\"scn\":" + SCENARIO +
              ",\"seq\":" + seq + ",\"up_ms\":" + now +
              ",\"t\":" + (isnan(t) ? String("null") : String(t, 2)) +
              ",\"h\":" + (isnan(h) ? String("null") : String(h, 1)) +
              ",\"st\":\"" + st + "\"" +
              ",\"i2c_nack\":" + i2cNack + ",\"i2c_crc\":" + i2cCrc +
              ",\"reconn\":" + reconnects +
              extra +
              ",\"label\":" + (injected ? 1 : 0) + "}";

  publishTele(T_TELE, js);
  Serial.println(js);
}
