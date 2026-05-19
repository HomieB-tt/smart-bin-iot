// ============================================================
//  SmartBin Backend — server.js
//  Node.js + Express + MQTT + MySQL
//  Run: node server.js
// ============================================================

const express  = require('express');
const cors     = require('cors');
const mqtt     = require('mqtt');
const mysql    = require('mysql2/promise');
const dotenv   = require('dotenv');

dotenv.config();

const app  = express();
const PORT = process.env.PORT || 3000;

app.use(cors());
app.use(express.json());
app.use(express.static('public')); // serves index.html from /public

// ── Config ──────────────────────────────────────────────────
const DB_CONFIG = {
  host:     process.env.DB_HOST     || 'localhost',
  port:     process.env.DB_PORT     || 3306,
  user:     process.env.DB_USER     || 'root',
  password: process.env.DB_PASS     || '',
  database: process.env.DB_NAME     || 'smartbin',
};

const MQTT_BROKER = process.env.MQTT_BROKER || 'mqtt://localhost:1883';

// ── MQTT Topic Map ───────────────────────────────────────────
// Topics the ESP32 publishes to:
//   smartbin/fill/metal        → { fill_pct: 45, distance_cm: 27.5 }
//   smartbin/fill/organic      → { fill_pct: 72, distance_cm: 14.0 }
//   smartbin/fill/inorganic    → { fill_pct: 30, distance_cm: 35.0 }
//   smartbin/classify          → { category: "metal"|"organic"|"inorganic", confidence: 0.95 }
//   smartbin/sensor/status     → { sensor: "metal", ok: true }
//   smartbin/system/heartbeat  → { uptime_s: 3600, free_heap: 120000 }

const TOPICS = [
  'smartbin/fill/+',
  'smartbin/classify',
  'smartbin/sensor/status',
  'smartbin/system/heartbeat',
];

// ── In-memory state (fast reads for dashboard) ────────────────
const state = {
  mqtt_connected: false,
  bins: {
    metal:     { fill_pct: 0, distance_cm: null, items_today: 0, last_item: null, sensor_ok: true },
    organic:   { fill_pct: 0, distance_cm: null, items_today: 0, last_item: null, sensor_ok: true },
    inorganic: { fill_pct: 0, distance_cm: null, items_today: 0, last_item: null, sensor_ok: true },
  },
  last_heartbeat: null,
};

// ── DB pool ──────────────────────────────────────────────────
let db;

async function initDB() {
  db = await mysql.createPool(DB_CONFIG);
  console.log('[DB] Connected to MySQL');
}

// ── MQTT Client ──────────────────────────────────────────────
let mqttClient;

function initMQTT() {
  mqttClient = mqtt.connect(MQTT_BROKER, {
    clientId:  `smartbin-server-${Date.now()}`,
    clean:     true,
    reconnectPeriod: 3000,
  });

  mqttClient.on('connect', () => {
    console.log('[MQTT] Connected to broker:', MQTT_BROKER);
    state.mqtt_connected = true;
    TOPICS.forEach(t => mqttClient.subscribe(t, { qos: 1 }));
  });

  mqttClient.on('reconnect', () => {
    console.log('[MQTT] Reconnecting...');
    state.mqtt_connected = false;
  });

  mqttClient.on('error', (err) => {
    console.error('[MQTT] Error:', err.message);
    state.mqtt_connected = false;
  });

  mqttClient.on('message', async (topic, payload) => {
    let msg;
    try { msg = JSON.parse(payload.toString()); }
    catch { console.warn('[MQTT] Bad JSON on topic:', topic); return; }

    // ── Fill level update ──
    if (topic.startsWith('smartbin/fill/')) {
      const category = topic.split('/')[2]; // metal | organic | inorganic
      if (!state.bins[category]) return;

      state.bins[category].fill_pct    = Math.min(100, Math.max(0, msg.fill_pct ?? 0));
      state.bins[category].distance_cm = msg.distance_cm ?? null;

      // Persist fill reading
      try {
        await db.execute(
          'INSERT INTO fill_readings (category, fill_pct, distance_cm) VALUES (?, ?, ?)',
          [category, state.bins[category].fill_pct, state.bins[category].distance_cm]
        );
      } catch (e) { console.error('[DB] fill insert error:', e.message); }

      // Full-bin alert (log once per crossing)
      if (state.bins[category].fill_pct >= 90) {
        console.warn(`[ALERT] ${category} bin is full (${state.bins[category].fill_pct}%)`);
        // If Africa's Talking SMS is configured, fire it here
        sendSMSAlert(category, state.bins[category].fill_pct);
      }
    }

    // ── Classification event ──
    else if (topic === 'smartbin/classify') {
      const { category, confidence } = msg;
      if (!state.bins[category]) return;

      state.bins[category].items_today++;
      state.bins[category].last_item = new Date().toISOString();

      try {
        await db.execute(
          'INSERT INTO classification_events (category, confidence) VALUES (?, ?)',
          [category, confidence ?? null]
        );
      } catch (e) { console.error('[DB] classify insert error:', e.message); }

      console.log(`[CLASSIFY] ${category} (confidence: ${confidence ?? 'N/A'})`);
    }

    // ── Sensor status ──
    else if (topic === 'smartbin/sensor/status') {
      const { sensor, ok } = msg;
      if (state.bins[sensor] !== undefined) {
        state.bins[sensor].sensor_ok = ok;
      }
    }

    // ── Heartbeat ──
    else if (topic === 'smartbin/system/heartbeat') {
      state.last_heartbeat = new Date().toISOString();
    }
  });
}

// ── SMS via Africa's Talking (optional) ─────────────────────
const AT_API_KEY  = process.env.AT_API_KEY;
const AT_USERNAME = process.env.AT_USERNAME;
const AT_FROM     = process.env.AT_FROM     || 'SmartBin';
const AT_TO       = process.env.AT_PHONE    || '';          // e.g. +256700000000

const alertCooldowns = {}; // prevent repeated SMS for same bin

async function sendSMSAlert(category, fillPct) {
  if (!AT_API_KEY || !AT_USERNAME || !AT_TO) return; // SMS not configured

  const now  = Date.now();
  const last = alertCooldowns[category] || 0;
  if (now - last < 30 * 60 * 1000) return; // cooldown: 30 min between alerts
  alertCooldowns[category] = now;

  try {
    const AfricasTalking = require('africastalking');
    const at  = AfricasTalking({ apiKey: AT_API_KEY, username: AT_USERNAME });
    const sms = at.SMS;
    await sms.send({
      to:      [AT_TO],
      from:    AT_FROM,
      message: `SmartBin Alert: ${category.toUpperCase()} bin is ${fillPct}% full. Please empty it soon.`,
    });
    console.log(`[SMS] Alert sent for ${category} bin`);
  } catch (e) {
    console.error('[SMS] Failed to send:', e.message);
  }
}

// ── Reset daily counters at midnight ────────────────────────
function scheduleDailyReset() {
  const now  = new Date();
  const next = new Date(now);
  next.setHours(24, 0, 0, 0); // next midnight
  const msUntilMidnight = next - now;

  setTimeout(() => {
    Object.values(state.bins).forEach(b => {
      b.items_today = 0;
      b.last_item   = null;
    });
    console.log('[RESET] Daily counters reset at midnight');
    scheduleDailyReset(); // reschedule
  }, msUntilMidnight);
}

// ── REST API ─────────────────────────────────────────────────

// GET /api/status — current in-memory state (fast, no DB hit)
app.get('/api/status', (req, res) => {
  res.json(state);
});

// GET /api/events?limit=30 — recent classification events
app.get('/api/events', async (req, res) => {
  const limit = Math.min(parseInt(req.query.limit) || 30, 100);
  try {
    const [rows] = await db.execute(
      'SELECT id, category, confidence, created_at FROM classification_events ORDER BY created_at DESC LIMIT ?',
      [limit]
    );
    res.json(rows);
  } catch (e) {
    console.error('[API] /events error:', e.message);
    res.status(500).json({ error: 'Database error' });
  }
});

// GET /api/fill-history?category=metal&hours=24 — fill trend data
app.get('/api/fill-history', async (req, res) => {
  const { category = 'metal', hours = 24 } = req.query;
  try {
    const [rows] = await db.execute(
      `SELECT fill_pct, created_at
       FROM fill_readings
       WHERE category = ? AND created_at >= NOW() - INTERVAL ? HOUR
       ORDER BY created_at ASC`,
      [category, parseInt(hours)]
    );
    res.json(rows);
  } catch (e) {
    console.error('[API] /fill-history error:', e.message);
    res.status(500).json({ error: 'Database error' });
  }
});

// GET /api/composition?date=2025-01-01 — daily waste composition
app.get('/api/composition', async (req, res) => {
  const date = req.query.date || new Date().toISOString().slice(0, 10);
  try {
    const [rows] = await db.execute(
      `SELECT category, COUNT(*) AS count
       FROM classification_events
       WHERE DATE(created_at) = ?
       GROUP BY category`,
      [date]
    );
    res.json(rows);
  } catch (e) {
    console.error('[API] /composition error:', e.message);
    res.status(500).json({ error: 'Database error' });
  }
});

// POST /api/simulate — inject a fake MQTT message for testing without hardware
app.post('/api/simulate', async (req, res) => {
  if (process.env.NODE_ENV === 'production') {
    return res.status(403).json({ error: 'Simulation disabled in production' });
  }
  const { type, category, fill_pct, confidence } = req.body;

  if (type === 'fill' && category && fill_pct !== undefined) {
    mqttClient.publish(`smartbin/fill/${category}`, JSON.stringify({ fill_pct, distance_cm: null }));
  } else if (type === 'classify' && category) {
    mqttClient.publish('smartbin/classify', JSON.stringify({ category, confidence: confidence ?? 0.9 }));
  }

  res.json({ ok: true, message: 'Simulated message published to MQTT' });
});

// ── Start ────────────────────────────────────────────────────
async function start() {
  await initDB();
  initMQTT();
  scheduleDailyReset();

  app.listen(PORT, () => {
    console.log(`[SERVER] SmartBin backend running at http://localhost:${PORT}`);
    console.log(`[SERVER] Dashboard: http://localhost:${PORT}`);
  });
}

start().catch(err => {
  console.error('[FATAL] Failed to start:', err.message);
  process.exit(1);
});
