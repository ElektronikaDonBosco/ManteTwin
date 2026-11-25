// =====================================================================
//  ManteTwin · Escenario 1 – Deriva de sensor → tendencia y RUL
//  Entrada : msg.payload = telemetría JSON del nodo mt-drift
//  Salida 1: analítica (cada muestra)        → mantetwin/<nodo>/analytics
//  Salida 2: alerta (solo cambios de estado) → mantetwin/<nodo>/alert
// =====================================================================

// ---- Parámetros (ajustables) ----
const WINDOW_MIN = 5;      // ventana de la regresión lineal (min)
const MIN_POINTS = 15;     // muestras mínimas para estimar la tendencia
const WARN  = 0.5;         // °C (mismo umbral que el firmware)
const ALARM = 1.5;         // °C → fin de vida útil del sensor (recalibrar)
const MIN_RATE = 0.01;     // °C/min: por debajo se considera estable
const MIN_R2 = 0.6;        // calidad mínima del ajuste para fiarse del RUL

const d = msg.payload;
if (!d || typeof d.dT_ema !== "number") {
    node.status({ fill: "grey", shape: "ring", text: "sin dT_ema (" + (d && d.st) + ")" });
    return null;
}
const nodeId = d.node || "mt-drift";
const now = Date.now();

// ---- Reinicio del nodo (seq vuelve a empezar) → vaciar historial ----
let buf = context.get("buf") || [];
const lastSeq = context.get("lastSeq") || 0;
if (typeof d.seq === "number" && d.seq < lastSeq) buf = [];
context.set("lastSeq", d.seq || 0);

// ---- Ventana deslizante ----
buf.push({ t: now, y: d.dT_ema });
buf = buf.filter(p => p.t >= now - WINDOW_MIN * 60000);
context.set("buf", buf);

// ---- Regresión lineal y = a + b·x  (x en minutos) ----
let slope = 0, r2 = 0;
const n = buf.length;
if (n >= 3) {
    const t0 = buf[0].t;
    let sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (const p of buf) {
        const x = (p.t - t0) / 60000;
        sx += x; sy += p.y; sxx += x * x; sxy += x * p.y; syy += p.y * p.y;
    }
    const den = n * sxx - sx * sx;
    if (den > 0) {
        slope = (n * sxy - sx * sy) / den;
        const num = n * sxy - sx * sy;
        const dy = n * syy - sy * sy;
        r2 = dy > 0 ? (num * num) / (den * dy) : 0;
    }
}

// ---- Deriva "hacia fuera" (sirve para deriva positiva o negativa) ----
const absDev = Math.abs(d.dT_ema);
const dir = Math.sign(d.dT_ema) || Math.sign(slope) || 1;
const rate = slope * dir;                         // °C/min alejándose de 0
const trendOk = n >= MIN_POINTS && rate > MIN_RATE && r2 >= MIN_R2;

// ---- RUL (minutos hasta llegar al umbral de alarma) ----
let rulMin = null, warnMin = null;
if (absDev >= ALARM) rulMin = 0;
else if (trendOk) rulMin = (ALARM - absDev) / rate;
if (absDev >= WARN) warnMin = 0;
else if (trendOk) warnMin = (WARN - absDev) / rate;

// ---- Salud del sensor (100 % = calibrado, 0 % = umbral de alarma) ----
const health = Math.max(0, Math.min(100, 100 * (1 - absDev / ALARM)));

// ---- Estado según el servidor (se adelanta al Edge gracias a la tendencia) ----
let state = "OK";
if (absDev >= ALARM) state = "ALARM";
else if (absDev >= WARN) state = "WARN";
else if (trendOk && rulMin !== null && rulMin < 30) state = "TREND";   // deriva incipiente

const fmt = m => m === null ? "—" : m < 1 ? Math.round(m * 60) + " s" : m.toFixed(1) + " min";

const analytics = {
    node: nodeId,
    ts: now,
    t: d.t, t_ref: d.t_ref,
    dT: d.dT, dT_ema: d.dT_ema,
    slope_c_min: +slope.toFixed(4),
    r2: +r2.toFixed(3),
    n_points: n,
    rul_min: rulMin === null ? null : +rulMin.toFixed(2),
    warn_in_min: warnMin === null ? null : +warnMin.toFixed(2),
    health_pct: +health.toFixed(1),
    state_server: state,
    state_edge: d.st,
    label: d.label
};

// ---- Estado del nodo en el editor ----
const colors = { OK: "green", TREND: "yellow", WARN: "yellow", ALARM: "red" };
node.status({ fill: colors[state], shape: "dot",
              text: `${state} | dT=${d.dT_ema.toFixed(2)} °C | ${rate.toFixed(3)} °C/min | RUL ${fmt(rulMin)}` });

const out1 = { topic: `mantetwin/${nodeId}/analytics`, payload: analytics };

// ---- Alerta solo cuando cambia el estado ----
let out2 = null;
const prev = context.get("state") || "OK";
if (state !== prev) {
    context.set("state", state);
    const txt = {
        OK:    "Sensor de nuevo dentro de calibración",
        TREND: `Deriva detectada: alcanzará la alarma en ~${fmt(rulMin)}`,
        WARN:  `Desviación ${absDev.toFixed(2)} °C (aviso). RUL ~${fmt(rulMin)}`,
        ALARM: `Desviación ${absDev.toFixed(2)} °C: RECALIBRAR / sustituir sensor`
    }[state];
    out2 = { topic: `mantetwin/${nodeId}/alert`,
             payload: { node: nodeId, ts: now, from: prev, to: state, msg: txt, rul_min: analytics.rul_min } };
}
return [out1, out2];
