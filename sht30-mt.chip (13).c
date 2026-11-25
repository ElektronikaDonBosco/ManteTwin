// ============================================================================
//  SHT30 ManteTwin - Wokwi custom chip
//  CIFP Don Bosco LHII - ManteTwin (Tknika 2026-27)
//
//  M5Stack ENV III-ko SHT30 sentsorea emulatzen du (I2C, 0x44 / 0x45),
//  akatsak injektatzeko aukerarekin. Simulazioan txipean klik eginda
//  kontrolak agertzen dira:
//
//    temperature  : tenperatura "erreala" (°C)
//    humidity     : hezetasun erlatiboa (%)
//    driftRate    : deriba-abiadura (°C/min). Offseta metatu egiten da eta
//                   ez da berreskuratzen (sentsore errealetan bezala).
//    faultMode    : 0 = normala
//                   1 = blokeatuta (azken balioa izoztuta)
//                   2 = hilda (ez du I2C ACK-rik ematen)
//                   3 = intermitentea (konektore solte: ~%20 NACK)
//                   4 = bus-zarata (CRC okerrak ~%50)
//
//  FLT pina: HIGH akatsen bat aktibo dagoenean (driftRate > 0 edo
//  faultMode != 0). Dataseta etiketatzeko "ground truth" gisa erabiltzen da.
//
//  diagram.json-eko "address" atributua: 68 (0x44) edo 69 (0x45).
// ============================================================================

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

enum {
  FAULT_NONE = 0,
  FAULT_STUCK = 1,
  FAULT_DEAD = 2,
  FAULT_INTERMITTENT = 3,
  FAULT_CRC = 4
};

typedef struct {
  pin_t pin_flt;
  uint32_t attr_temp;
  uint32_t attr_hum;
  uint32_t attr_drift;
  uint32_t attr_fault;
  float drift_offset;      // metatutako deriba (°C)
  uint8_t cmd[2];
  uint8_t cmd_len;
  uint8_t out[6];          // T_MSB T_LSB CRC H_MSB H_LSB CRC
  uint8_t out_idx;
  uint16_t last_raw_t;
  uint16_t last_raw_h;
  bool has_last;
  uint32_t rng;
} chip_state_t;

// Ausazko zenbaki sinplea (0..1)
static float rnd(chip_state_t *c) {
  c->rng = c->rng * 1103515245u + 12345u;
  return (float)((c->rng >> 8) & 0xFFFF) / 65535.0f;
}

// SHT3x CRC-8 (polinomioa 0x31, hasiera 0xFF)
static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

static int fault_mode(chip_state_t *c) {
  return (int)(attr_read_float(c->attr_fault) + 0.5f);
}

static uint16_t to_raw(float v) {
  if (v < 0) v = 0;
  if (v > 65535) v = 65535;
  return (uint16_t)(v + 0.5f);
}

static void prepare_measurement(chip_state_t *c) {
  int f = fault_mode(c);
  uint16_t rt, rh;

  if (f == FAULT_STUCK && c->has_last) {
    // Blokeatuta: azken neurketa errepikatu, zaratarik gabe
    rt = c->last_raw_t;
    rh = c->last_raw_h;
  } else {
    // Sentsore errealek zarata txikia dute: horri esker detektatzen da blokeoa
    float t = attr_read_float(c->attr_temp) + c->drift_offset + (rnd(c) - 0.5f) * 0.10f;
    float h = attr_read_float(c->attr_hum) + (rnd(c) - 0.5f) * 0.40f;
    if (h < 0) h = 0;
    if (h > 100) h = 100;
    rt = to_raw((t + 45.0f) * 65535.0f / 175.0f);
    rh = to_raw(h * 65535.0f / 100.0f);
    c->last_raw_t = rt;
    c->last_raw_h = rh;
    c->has_last = true;
  }

  c->out[0] = rt >> 8;
  c->out[1] = rt & 0xFF;
  c->out[2] = crc8(&c->out[0], 2);
  c->out[3] = rh >> 8;
  c->out[4] = rh & 0xFF;
  c->out[5] = crc8(&c->out[3], 2);

  if (f == FAULT_CRC && rnd(c) < 0.5f) {
    c->out[2] ^= 0x5A;   // bus-zarata: CRC hondatua
  }
  c->out_idx = 0;
}

// ---------------- I2C ----------------
static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  chip_state_t *c = user_data;
  int f = fault_mode(c);
  if (f == FAULT_DEAD) return false;                              // NACK beti
  if (f == FAULT_INTERMITTENT && rnd(c) < 0.20f) return false;    // NACK batzuetan
  if (!read) c->cmd_len = 0;
  return true;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *c = user_data;
  if (c->cmd_len < 2) c->cmd[c->cmd_len++] = data;
  if (c->cmd_len == 2) {
    uint8_t msb = c->cmd[0];
    // 0x24xx = single shot (clock stretching gabe), 0x2Cxx = clock stretchingarekin
    if (msb == 0x24 || msb == 0x2C) prepare_measurement(c);
  }
  return true;
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *c = user_data;
  uint8_t v = (c->out_idx < 6) ? c->out[c->out_idx] : 0xFF;
  c->out_idx++;
  return v;
}

static void on_i2c_disconnect(void *user_data) {
  (void)user_data;
}

// ---------------- Denbora (100 ms) ----------------
static void on_tick(void *user_data) {
  chip_state_t *c = user_data;
  float rate = attr_read_float(c->attr_drift);   // °C/min
  if (rate > 0.0f) c->drift_offset += rate / 600.0f;
  bool active = (fault_mode(c) != FAULT_NONE) || (rate > 0.001f);
  pin_write(c->pin_flt, active ? HIGH : LOW);
}

void chip_init(void) {
  chip_state_t *c = calloc(1, sizeof(chip_state_t));

  c->attr_temp  = attr_init_float("temperature", 24.0f);
  c->attr_hum   = attr_init_float("humidity", 45.0f);
  c->attr_drift = attr_init_float("driftRate", 0.0f);
  c->attr_fault = attr_init_float("faultMode", 0.0f);
  uint32_t address = attr_read(attr_init("address", 0x44));

  c->pin_flt = pin_init("FLT", OUTPUT_LOW);
  c->rng = 0xC0FFEEu + address;

  const i2c_config_t i2c_config = {
    .user_data = c,
    .address = address,
    .scl = pin_init("SCL", INPUT),
    .sda = pin_init("SDA", INPUT),
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
  };
  i2c_init(&i2c_config);

  const timer_config_t timer_config = {
    .callback = on_tick,
    .user_data = c,
  };
  timer_t t = timer_init(&timer_config);
  timer_start(t, 100000, true);   // 100 ms

  printf("SHT30 ManteTwin prest, helbidea 0x%02X\n", (unsigned)address);
}
