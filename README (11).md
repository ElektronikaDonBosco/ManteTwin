# ManteTwin – Akats-eszenatokiak Wokwi-n (1, 2 eta 5)

CIFP Don Bosco LHII · ManteTwin (Tknika 2026-27)

Hiru zirkuitu, firmware bera (`_common/firmware_template.ino`) eta custom chip bera (`sht30-mt`): M5Stack ENV III-ko **SHT30** sentsorea, akatsak injektatzeko kontrolekin. Firmware hori bera erabil daiteke gero ESP32 + ENV III errealarekin (hilo digitala: nodo birtuala eta fisikoa telemetria berdinarekin).

| Karpeta | Hutsegitea | Akatsa nola injektatu | Nodoak (Edge) zer detektatzen du |
|---|---|---|---|
| `01-deriba` | Sentsorearen deriba (kalibrazio-galera) | SHT30 nagusian `Deriba (°C/min)` > 0 | `DRIFT_WARN` (>0,5 °C) / `DRIFT_ALARM` (>1,5 °C), erreferentziako sentsorearekin konparatuta |
| `02-sentsore-blokeatua` | Sentsore blokeatua / I2C errorea | `Akatsa`: 1 blokeatuta, 2 hilda, 3 intermitentea, 4 CRC | `STUCK`, `SENSOR_DEAD`, `INTERMITTENT`, `BUS_NOISE` |
| `05-komunikazioak` | RSSI baxua, pakete-galera, lotura etena | Potentziometroak (ahultzea 0-40 dB, galera 0-50 %) eta etengailua (lotura moztu) | `WEAK_SIGNAL`, `PACKET_LOSS`, `HIGH_LATENCY`; lotura etenean brokerrak `offline` (LWT) |

LEDak: **berdea** = dena ondo · **gorria** = nodoak akatsa detektatu du · **horia** = akatsa injektatuta dago (egiazko etiketa).

## 1. Wokwi-n kargatzea

1. wokwi.com → *New project* → **ESP32**.
2. Karpeta bateko fitxategiak kopiatu: `sketch.ino` eta `diagram.json` ordeztu, eta gehitu (*+ → New file*) `libraries.txt`, `sht30-mt.chip.json` eta `sht30-mt.chip.c`. Fitxategi-izenek berdin-berdinak izan behar dute.
3. *Start*. Chip-aren gainean klik eginda agertzen dira kontrolak (tenperatura, deriba, akatsa).

## 2. MQTT brokerra

Firmwareak `host.wokwi.internal:1883` erabiltzen du; hori zure ordenagailua da **Wokwi Private IoT Gateway**-rekin (Pro lizentzia):

1. Gateway-a deskargatu eta exekutatu (`wokwigw`), Wokwi-ren dokumentazioaren arabera.
2. Editorean F1 → *Enable Private Wokwi IoT Gateway*.

Mosquitto Docker-en (Node-RED-en ondoan). `mosquitto.conf`:

```
listener 1883
allow_anonymous true
```

```
docker run -d --name mosquitto -p 1883:1883 -v ${PWD}/mosquitto.conf:/mosquitto/config/mosquitto.conf eclipse-mosquitto:2
```

Node-RED-en: `mqtt in` nodoa → zerbitzaria `host.docker.internal:1883` (Node-RED edukiontzi batean badago) → topikoa `mantetwin/#` → `json` → `debug`.

> Gateway-a martxan jarri aurretik probatzeko, `sketch.ino`-n jarri `MQTT_HOST = "broker.hivemq.com"` (publikoa, ez erabili datu errealekin).

## 3. Topikoak

| Topikoa | Norabidea | Edukia |
|---|---|---|
| `mantetwin/<nodo>/telemetry` | nodo → zerbitzari | JSON (behean) |
| `mantetwin/<nodo>/status` | nodo → zerbitzari (retained) | `online` / `offline` (LWT) |
| `mantetwin/<nodo>/cmd` | zerbitzari → nodo | `interval=5000`, `reset` |
| `mantetwin/<nodo>/event` | nodo → zerbitzari | komandoen ACK-a, `link_restored` + etenaldiaren iraupena |
| `mantetwin/<nodo>/ping` | nodo ↔ nodo | latentzia neurtzeko (5. eszenatokia) |

Nodoak: `mt-drift`, `mt-stuck`, `mt-comms`.

Telemetria-adibidea (1. eszenatokia):

```json
{"node":"mt-drift","scn":1,"seq":42,"up_ms":84000,"t":25.31,"h":45.1,"st":"DRIFT_WARN",
 "i2c_nack":0,"i2c_crc":0,"reconn":1,"t_ref":24.02,"dT":1.29,"dT_ema":0.87,"label":1}
```

`label` eremua akatsa injektatuta dagoen ala ez da (chip-aren FLT pina edo 5. eszenatokian potentziometroak). Horri esker, InfluxDB-ra doan telemetria **zuzenean etiketatuta** geratzen da 3.1 ekintzako datasetarako (Edge Impulse).

## 4. Proba-prozedura

**01 – Deriba**
1. Bi sentsoreak 24 °C-tan utzi. Egoera `OK`.
2. Nagusian `Deriba` = 0,5 °C/min. 1-2 minutu barru `DRIFT_WARN`, gero `DRIFT_ALARM`.
3. Zerbitzarian `dT_ema`-ren joera erabil daiteke RUL kalkulatzeko (noiz iritsiko den 1,5 °C-ra).
4. ⚠️ Giro-tenperatura aldatu nahi baduzu, **bi sentsoreetan** aldatu, bestela deriba faltsua agertuko da.
5. Deriba metatu egiten da eta ez da 0-ra itzultzen (sentsore errealetan bezala); simulazioa berrabiarazi berrezartzeko.

**02 – Sentsore blokeatua / I2C**
- `Akatsa` = 1 → ~16 s barru `STUCK` (8 neurketa berdin, zaratarik gabe).
- `Akatsa` = 2 → `SENSOR_DEAD` (`t` = null, `i2c_nack` gora).
- `Akatsa` = 3 → `INTERMITTENT` (%20 inguru NACK; konektore solte bat).
- `Akatsa` = 4 → `BUS_NOISE` (CRC okerrak; kable luzea edo interferentziak).
- `cmd` topikora `reset` bidali kontagailuak garbitzeko.

**05 – Komunikazioak**
- Ahultze-potentziometroa > 20 dB → RSSI jaitsi eta latentzia igo (`WEAK_SIGNAL`, `HIGH_LATENCY`).
- Galera-potentziometroa > %10 → `PACKET_LOSS`.
- Etengailua → lotura etena: ~7 s barru brokerrak `offline` argitaratzen du `status`-en. Itzultzean, `event` topikoan `link_restored` + `outage_ms`.

## 5. Hardware errealera pasatzea

ESP32 + ENV III (Grove → SDA 21 / SCL 22, edo zure plakaren pinak). `sketch.ino`-n aldatu `WIFI_SSID`, `WIFI_PASS` eta `MQTT_HOST`. Akatsak fisikoki: SDA kablea deskonektatu (2), haize beroa sentsore batean (1), antena urrundu edo routerra itzali (5). Kasu horretan `label` ez da automatikoa: FLT pina (GPIO 34) etengailu batekin jar daiteke akatsa eragitean.

## Fitxategiak

- `_common/` – jatorrizko txantiloia eta custom chip-a (aldaketak hemen egin).
- `0X-*/` – Wokwi-ra igotzeko prest dauden proiektuak.
