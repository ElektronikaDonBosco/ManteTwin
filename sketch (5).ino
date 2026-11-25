// ============================================================
//  ManteTwin - Proyecto 4: MANTENIMIENTO PREVENTIVO
//  Contador de horas y arranques de un motor + TACOMETRO
//  CIFP Don Bosco LHII - Elektronika eta Telekomunikazioak
// ------------------------------------------------------------
//  Hardware:
//    GPIO 4  <- interruptor MARCHA/PARO (3V3 = marcha)
//    GPIO 23 -> rele 5 V (IN, activo alto) -> ventilador 12 V
//    GPIO 19 <- TACOMETRO (cable amarillo del ventilador, 2 pulsos/vuelta)
//    GPIO 21/22 -> OLED SSD1306 128x64 (0x3C)
//    GPIO 25/26/27 -> LEDs verde / amarillo / rojo
//  Solo Wokwi:
//    GPIO 18 -> genera los pulsos del "ventilador" (Wokwi no tiene motor DC)
//    Interruptor BLOQUEO: GPIO19 <- GPIO18 (gira) o GND (rotor bloqueado)
//
//  Novedad: las horas solo cuentan si el motor GIRA DE VERDAD (rpm).
//  Si el rele esta activado pero no llegan pulsos -> MOTOR BLOQUEADO:
//  el ESP32 corta el rele (proteccion) y avisa. Para rearmar: PARO y MARCHA.
//
//  MQTT:
//    publica  donbosco/mantetwin/motor1/prev/datuak   (JSON cada 2 s)
//    publica  donbosco/mantetwin/motor1/prev/status   ("online"/"offline", LWT)
//    escucha  donbosco/mantetwin/motor1/prev/egoera   ("OK;..." | "PROXIMO;..." | "VENCIDO;..." | "FALLO;...")
//    escucha  donbosco/mantetwin/motor1/prev/cmd      ("reset:engrase", "reset:rodamientos",
//                                                       "reset:electrica", "borrar_contadores")
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- Configuracion ----------
#define WOKWI 1            // 1 = simulacion Wokwi, 0 = montaje real

#if WOKWI
  const char* WIFI_SSID = "Wokwi-GUEST";
  const char* WIFI_PASS = "";
  const char* MQTT_HOST = "host.wokwi.internal";   // Private IoT Gateway
  const float ESCALA = 3600.0;                     // 1 s real = 1 h de motor
#else
  const char* WIFI_SSID = "TU_WIFI";
  const char* WIFI_PASS = "TU_CONTRASENA";
  const char* MQTT_HOST = "192.168.1.50";          // IP del servidor Mosquitto
  const float ESCALA = 1.0;                        // tiempo real
#endif
const int MQTT_PORT = 1883;

const char* T_DATOS  = "donbosco/mantetwin/motor1/prev/datuak";
const char* T_STATUS = "donbosco/mantetwin/motor1/prev/status";
const char* T_ESTADO = "donbosco/mantetwin/motor1/prev/egoera";
const char* T_CMD    = "donbosco/mantetwin/motor1/prev/cmd";

// ---------- Pines ----------
const int PIN_SW     = 4;
const int PIN_RELE   = 23;
const int PIN_TACH   = 19;
const int PIN_PULSOS = 18;   // solo Wokwi
const int PIN_VERDE  = 25;
const int PIN_AMAR   = 26;
const int PIN_ROJO   = 27;

// ---------- Tacometro ----------
const int   PULSOS_VUELTA = 2;      // ventiladores de PC: 2 pulsos por vuelta
const float RPM_MIN       = 300;    // por debajo: no gira
const unsigned long T_ARRANQUE = 3000;   // ms de margen para que acelere
#if WOKWI
  const int FREQ_SIM = 40;          // 40 Hz = 1200 rpm simuladas
#endif

volatile uint32_t pulsos = 0;
void IRAM_ATTR isrTach() { pulsos++; }

// ---------- Objetos ----------
WiFiClient wifi;
PubSubClient mqtt(wifi);
Preferences nvs;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;

// ---------- Contadores (persisten en NVS) ----------
double   runSec    = 0;     // segundos de funcionamiento real (escalados)
uint32_t arranques = 0;     // paro -> marcha
uint32_t bloqueos  = 0;     // veces que se ha detectado el rotor bloqueado

struct Tarea { const char* id; bool porArranques; double base; };
Tarea tareas[] = {
  { "engrase",     false, 0 },   // por horas
  { "rodamientos", false, 0 },   // por horas
  { "electrica",   true,  0 }    // por arranques (contactor/rele)
};
const int N_TAREAS = sizeof(tareas) / sizeof(tareas[0]);

// ---------- Estado ----------
bool mando = false;          // posicion del interruptor
bool motorOn = false;        // rele activado
bool bloqueado = false;      // fallo detectado por el tacometro
float rpm = 0;
unsigned long tOn = 0, tUltimo = 0, tPub = 0, tGuardar = 0, tOled = 0, tRpm = 0;
String estado  = "SIN_PLAN";
String mensaje = "Esperando Node-RED";

// ============================================================
//  NVS
// ============================================================
void cargarNVS() {
  nvs.begin("prev", true);
  runSec    = nvs.getDouble("run", 0);
  arranques = nvs.getUInt("arr", 0);
  bloqueos  = nvs.getUInt("blq", 0);
  for (int i = 0; i < N_TAREAS; i++) tareas[i].base = nvs.getDouble(tareas[i].id, 0);
  nvs.end();
}

void guardarNVS() {
  nvs.begin("prev", false);
  nvs.putDouble("run", runSec);
  nvs.putUInt("arr", arranques);
  nvs.putUInt("blq", bloqueos);
  for (int i = 0; i < N_TAREAS; i++) nvs.putDouble(tareas[i].id, tareas[i].base);
  nvs.end();
  Serial.println("[NVS] contadores guardados");
}

// ============================================================
//  Motor
// ============================================================
void ponerRele(bool on) {
  motorOn = on;
  digitalWrite(PIN_RELE, on);
#if WOKWI
  // Simulamos el ventilador: si el rele esta activado, "gira" y da pulsos
  if (on) tone(PIN_PULSOS, FREQ_SIM); else noTone(PIN_PULSOS);
#endif
}

// ============================================================
//  Salidas: LEDs y OLED
// ============================================================
void actualizarLeds() {
  bool rapido = (millis() / 200) % 2, lento = (millis() / 1000) % 2;
  if (bloqueado) {                       // fallo local: rojo parpadeo rapido
    digitalWrite(PIN_VERDE, LOW); digitalWrite(PIN_AMAR, LOW); digitalWrite(PIN_ROJO, rapido);
    return;
  }
  digitalWrite(PIN_VERDE, estado == "OK");
  digitalWrite(PIN_AMAR,  estado == "PROXIMO");
  if (estado == "VENCIDO" || estado == "FALLO") digitalWrite(PIN_ROJO, HIGH);
  else if (estado == "SIN_PLAN")                digitalWrite(PIN_ROJO, lento);
  else                                          digitalWrite(PIN_ROJO, LOW);
}

void actualizarOled() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("MOTOR1 ");
  oled.print(bloqueado ? "BLOQUEADO" : (motorOn ? "MARCHA" : "PARO"));
  oled.setCursor(0, 11);
  oled.printf("%.1f h   %4.0f rpm", runSec / 3600.0, rpm);
  oled.setCursor(0, 21);
  oled.printf("Arranq: %lu  Blq: %lu", (unsigned long)arranques, (unsigned long)bloqueos);
  oled.drawLine(0, 32, 127, 32, SSD1306_WHITE);
  oled.setCursor(0, 36);
  oled.print(bloqueado ? "FALLO" : estado.c_str());
  String m = bloqueado ? String("PARO y MARCHA p/rearmar") : mensaje;
  oled.setCursor(0, 47);
  oled.print(m.substring(0, 21));
  if (m.length() > 21) { oled.setCursor(0, 56); oled.print(m.substring(21, 42)); }
  oled.display();
}

// ============================================================
//  MQTT
// ============================================================
void publicarDatos() {
  char buf[480];
  int n = snprintf(buf, sizeof(buf),
    "{\"id\":\"motor1\",\"on\":%s,\"rpm\":%.0f,\"fallo\":%s,\"h\":%.3f,\"arranques\":%lu,"
    "\"bloqueos\":%lu,\"escala\":%.0f,\"tareas\":{",
    motorOn ? "true" : "false", rpm, bloqueado ? "true" : "false", runSec / 3600.0,
    (unsigned long)arranques, (unsigned long)bloqueos, ESCALA);
  for (int i = 0; i < N_TAREAS; i++) {
    double uso = tareas[i].porArranques ? (arranques - tareas[i].base)
                                        : (runSec - tareas[i].base) / 3600.0;
    n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%.3f", i ? "," : "", tareas[i].id, uso);
  }
  snprintf(buf + n, sizeof(buf) - n, "}}");
  mqtt.publish(T_DATOS, buf);
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] %s -> %s\n", topic, msg.c_str());

  if (strcmp(topic, T_ESTADO) == 0) {
    int p = msg.indexOf(';');
    estado  = (p < 0) ? msg : msg.substring(0, p);
    mensaje = (p < 0) ? ""  : msg.substring(p + 1);
    actualizarLeds();
    actualizarOled();
  }
  else if (strcmp(topic, T_CMD) == 0) {
    if (msg.startsWith("reset:")) {
      String id = msg.substring(6);
      for (int i = 0; i < N_TAREAS; i++) {
        if (id == tareas[i].id) {
          tareas[i].base = tareas[i].porArranques ? arranques : runSec;
          Serial.printf("[PREV] mantenimiento '%s' realizado\n", tareas[i].id);
        }
      }
      guardarNVS();
    } else if (msg == "borrar_contadores") {
      runSec = 0; arranques = 0; bloqueos = 0;
      for (int i = 0; i < N_TAREAS; i++) tareas[i].base = 0;
      guardarNVS();
      Serial.println("[PREV] contadores a cero (motor nuevo)");
    }
    publicarDatos();
  }
}

void conectarWiFi() {
  Serial.print("WiFi");
#if WOKWI
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
#else
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.print(" OK  IP: "); Serial.println(WiFi.localIP());
}

void conectarMQTT() {
  while (!mqtt.connected()) {
    String cid = "mantetwin-prev-" + String((uint32_t)esp_random(), HEX);
    Serial.print("MQTT...");
    if (mqtt.connect(cid.c_str(), T_STATUS, 1, true, "offline")) {   // LWT
      Serial.println(" conectado");
      mqtt.publish(T_STATUS, "online", true);
      mqtt.subscribe(T_ESTADO);
      mqtt.subscribe(T_CMD);
    } else {
      Serial.printf(" fallo rc=%d, reintento en 2 s\n", mqtt.state());
      delay(2000);
    }
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_SW, INPUT_PULLDOWN);
  pinMode(PIN_TACH, INPUT_PULLUP);     // tacometro: colector abierto
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_VERDE, OUTPUT);
  pinMode(PIN_AMAR, OUTPUT);
  pinMode(PIN_ROJO, OUTPUT);
  ponerRele(false);
  attachInterrupt(digitalPinToInterrupt(PIN_TACH), isrTach, FALLING);

  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOk) Serial.println("OLED no encontrado (sigo sin pantalla)");

  cargarNVS();
  Serial.printf("[NVS] horas=%.2f arranques=%lu bloqueos=%lu\n",
                runSec / 3600.0, (unsigned long)arranques, (unsigned long)bloqueos);
  actualizarOled();

  conectarWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);
  conectarMQTT();

  tUltimo = tRpm = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  unsigned long ahora = millis();

  // --- 1. Interruptor MARCHA/PARO (antirrebote) ---
  static bool swAnt = false; static unsigned long tSw = 0;
  bool sw = digitalRead(PIN_SW);
  if (sw != swAnt) { tSw = ahora; swAnt = sw; }
  if (ahora - tSw > 50 && sw != mando) {
    mando = sw;
    if (mando) {
      arranques++;
      bloqueado = false;
      tOn = ahora;
      ponerRele(true);
      Serial.printf("[MOTOR] ARRANQUE n.%lu\n", (unsigned long)arranques);
    } else {
      if (bloqueado) Serial.println("[MOTOR] rearmado");
      bloqueado = false;
      ponerRele(false);
      Serial.println("[MOTOR] PARO");
      guardarNVS();
    }
    publicarDatos();
    actualizarOled();
  }

  // --- 2. Tacometro: rpm cada segundo ---
  if (ahora - tRpm >= 1000) {
    noInterrupts(); uint32_t p = pulsos; pulsos = 0; interrupts();
    rpm = p * 60000.0 / (ahora - tRpm) / PULSOS_VUELTA;
    tRpm = ahora;

    // Rele activado, ya ha pasado el arranque y no gira -> BLOQUEADO
    if (motorOn && ahora - tOn > T_ARRANQUE && rpm < RPM_MIN) {
      bloqueado = true;
      bloqueos++;
      ponerRele(false);                 // proteccion: cortamos el motor
      guardarNVS();
      Serial.println("[MOTOR] *** BLOQUEADO: rele activado pero no gira ***");
      publicarDatos();
    }
  }

  // --- 3. Horas: solo si gira de verdad (o durante el arranque) ---
  double dt = (ahora - tUltimo) / 1000.0;
  tUltimo = ahora;
  bool gira = motorOn && (rpm >= RPM_MIN || ahora - tOn <= T_ARRANQUE);
  if (gira) runSec += dt * ESCALA;

  // --- 4. Tareas periodicas ---
  if (ahora - tPub > 2000)  { tPub = ahora;  publicarDatos(); }
  if (ahora - tOled > 500)  { tOled = ahora; actualizarOled(); }
  static unsigned long tLed = 0;
  if (ahora - tLed > 100)   { tLed = ahora;  actualizarLeds(); }
  if (motorOn && ahora - tGuardar > 60000) { tGuardar = ahora; guardarNVS(); }
}
