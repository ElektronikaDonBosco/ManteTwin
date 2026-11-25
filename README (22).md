# ManteTwin · Laboratorio IoT (Wokwi + Node-RED)

**CIFP Don Bosco LHII · Errenteria** · Departamento de Electrónica y Telecomunicaciones  
Proyecto de innovación **ManteTwin** (Tknika, curso 2026-27): mantenimiento predictivo y preventivo con ESP32, MQTT y Node-RED, simulado primero en Wokwi y montado después en protoboard.

🌐 **Web didáctica:** `https://<usuario>.github.io/mantetwin-iot/` (castellano) · `.../eu.html` (euskera)

## Proyectos

| | Proyecto | Tipo de mantenimiento | Hardware | Carpeta |
|---|---|---|---|---|
| 1 | Estación ENV III: temperatura, humedad y presión | — (base IoT) | ESP32 + ENV III (DHT22 + BMP180 en Wokwi) | [`proyectos/1-env3`](proyectos/1-env3) |
| 2 | Deriva de un sensor de temperatura y RUL | Predictivo | 2× SHT30 (principal + referencia) | [`proyectos/2-deriva`](proyectos/2-deriva) |
| 3 | Vibración de un motor (RMS, pico, factor de cresta) | Predictivo | MPU6050 | [`proyectos/3-motor`](proyectos/3-motor) |
| 4 | Contador de horas y arranques + tacómetro | Preventivo | Relé, OLED, ventilador 12 V | [`proyectos/4-preventivo`](proyectos/4-preventivo) |
| + | Escenarios de fallo: sensor bloqueado y comunicaciones | Predictivo | SHT30 (custom chip) | [`proyectos/escenarios-fallo`](proyectos/escenarios-fallo) |

Cada carpeta tiene lo necesario para replicarlo:

- `diagram.json`, `sketch.ino`, `libraries.txt` (y `*.chip.*` si hay custom chip) → **Wokwi**
- `flow-*.json` → **Node-RED** (flujo + dashboard en un solo archivo)

## Qué hace falta

- [Wokwi](https://wokwi.com) con licencia **Pro** (Private IoT Gateway `wokwigw`).
- **Docker** con Mosquitto (puerto 1883) y Node-RED (puerto 1880) con `@flowfuse/node-red-dashboard`.
- En Wokwi el ESP32 usa la WiFi `Wokwi-GUEST` y el broker `host.wokwi.internal`.

El paso a paso completo (instalación, problemas típicos y explicación de cada proyecto) está en la web, carpeta [`docs`](docs).

## Topics MQTT

| Proyecto | Topic base |
|---|---|
| 1 | `mantetwin/env3/...` |
| 2 | `mantetwin/mt-drift/...` |
| 3 | `donbosco/mantetwin/motor1/...` |
| 4 | `donbosco/mantetwin/motor1/prev/...` |

## KiCad

[`kicad`](kicad): esquemas del nodo de deriva (zócalos DevKitC, Grove, LEDs, jumper de fallo) y de la placa SHT30. Versión KiCad 8 y copia para KiCad 7 en `kicad/kicad7`.

## Publicar la web con GitHub Pages

Settings → Pages → *Deploy from a branch* → rama `main`, carpeta **`/docs`** → Save. En un par de minutos estará en `https://<usuario>.github.io/mantetwin-iot/`.

## ⚠️ Seguridad

- No subas contraseñas WiFi reales: en los sketch para hardware real deja `TU_WIFI` / `TU_CONTRASENA` y cámbialos solo en tu copia local.
- No expongas Mosquitto ni Node-RED a Internet: red del aula separada o VPN.

---

### Euskaraz

**ManteTwin IoT laborategia** — CIFP Don Bosco LHII, Elektronika eta Telekomunikazioak saila (Tknika 2026-27). Lau proiektu (ENV III, sentsorearen deriba, motorraren bibrazioa eta mantentze prebentiboa) Wokwi-n simulatuta eta Node-RED-ekin. Web didaktikoa euskaraz: `docs/eu.html`.
