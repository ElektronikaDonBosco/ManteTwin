# ManteTwin · Proyecto 4 · Mantenimiento PREVENTIVO
Contador de horas de funcionamiento y arranques de un motor (ESP32 + MQTT + Node-RED)

## Idea
- El **ESP32 cuenta**: horas con el motor en marcha y número de arranques (paro → marcha). Lo guarda en memoria NVS: no se pierde al quitar la alimentación.
- **Node-RED decide**: compara con el plan de mantenimiento y devuelve el estado OK / PROXIMO / VENCIDO.
- El ESP32 muestra el estado en los LEDs (verde/amarillo/rojo) y en la OLED.
- Cuando el técnico hace el mantenimiento pulsa **"Hecho"** en el dashboard → se reinicia ese contador y se apunta en el libro de mantenimiento (CSV).

| Tarea | Intervalo | Se cuenta por |
|---|---|---|
| Engrase | 250 h | horas |
| Revisión rodamientos | 1000 h | horas |
| Revisión contactor/relé | 30 arranques | arranques |
PROXIMO a partir del 90 %, VENCIDO al 100 %. Se cambia en la función "Plan de mantenimiento".

## Novedad: tacómetro (¿gira de verdad?)
- El ESP32 no se fía de la orden: lee los **pulsos del tacómetro** del ventilador (GPIO 19, 2 pulsos/vuelta) y calcula las rpm.
- Las **horas solo cuentan si gira** (≥ 300 rpm).
- Si el relé está activado y a los 3 s no gira → **MOTOR BLOQUEADO**: el ESP32 corta el relé (protección), LED rojo parpadeando rápido, aviso en Node-RED y se suma 1 a "bloqueos". Para rearmar: PARO y otra vez MARCHA.
- Es el puente entre preventivo (contar horas) y predictivo (vigilar cómo funciona).

## Wokwi
1. Nuevo proyecto ESP32 → pega `sketch.ino` y `diagram.json`, añade `libraries.txt`.
2. Arranca el Private IoT Gateway (`wokwigw`), como en los otros proyectos, y Mosquitto.
3. Ejecuta en una **ventana aparte** (no pestaña en segundo plano).
4. Mueve el interruptor → el relé se activa, el LED azul (motor) se enciende y cuentan las horas.
   **Tiempo acelerado: 1 s real = 1 h de motor** (`ESCALA 3600`). En ~4 min llega el aviso de engrase.
   Para probar el contactor: sube y baja el interruptor 27-30 veces.
5. **Segundo interruptor = BLOQUEO.** Wokwi no tiene motor DC, así que el ESP32 genera en GPIO 18 los pulsos del "ventilador" (1200 rpm) cuando el relé está activado.
   - Interruptor hacia GPIO 18 → los pulsos llegan a GPIO 19: el motor gira.
   - Interruptor hacia GND → no llegan pulsos: rotor bloqueado. Con el motor en marcha, en 3 s salta el fallo.

## Node-RED
1. Menú → Importar → `flow-preventivo.json` → Importar. Si avisa de que algunos nodos ya existen (broker, ui-base, tema) elige **no importar esos / usar los existentes**: son los mismos que en ENV III, deriva y motor.
2. Deploy. Dashboard: `http://localhost:1880/dashboard/preventivo`
3. Libro de mantenimiento: `/data/mantetwin_motor1_preventivo.csv` (dentro del contenedor; en el volumen de datos de Node-RED).

## Topics MQTT
| Topic | Dirección | Contenido |
|---|---|---|
| `donbosco/mantetwin/motor1/prev/datuak` | ESP32 → | JSON cada 2 s: `on`, `rpm`, `fallo`, `h`, `arranques`, `bloqueos`, `tareas{engrase, rodamientos, electrica}` |
| `.../prev/egoera` | → ESP32 (retain) | `OK;Todo al dia`, `PROXIMO;Engrase proximo`, `VENCIDO;...`, `FALLO;Motor bloqueado...` |
| `.../prev/cmd` | → ESP32 | `reset:engrase`, `reset:rodamientos`, `reset:electrica`, `borrar_contadores` |
| `.../prev/status` | ESP32 → (LWT) | `online` / `offline` |

## Montaje real (protoboard)
- `#define WOKWI 0`, pon tu WiFi y la IP del servidor Mosquitto. `ESCALA` pasa a 1.0 (tiempo real).
- Interruptor: GPIO 4 a 3V3 = marcha (el pin usa pull-down interno).
- Relé 5 V (disparo alto): VCC 5V, GND, IN GPIO 23. **COM/NO conmutan el +12 V del ventilador** (en Wokwi el COM va a 3V3 solo para encender el LED azul).
- **Tacómetro** (ventilador de 3 o 4 cables): cable **amarillo → GPIO 19** con **resistencia de 10 kΩ a 3V3** (pull-up; es colector abierto). **GND de la fuente de 12 V unido al GND del ESP32**, si no, no llegan los pulsos. Nunca conectes el amarillo a 5 V/12 V. El segundo interruptor y GPIO 18 no se usan en el montaje real.
- Si el ventilador es de 2 cables (sin tacómetro) no se puede: habría que poner un sensor (p. ej. óptico o de efecto Hall).
- Prueba de bloqueo real: con el ventilador en marcha, frena las aspas con un lápiz (¡no con el dedo!).
- OLED SSD1306: SDA 21, SCL 22, 3V3. LEDs 25/26/27 con 220 Ω.
- El NVS se graba al parar el motor y cada 60 s en marcha.
