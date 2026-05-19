# SmartBin Dashboard

IoT web application for the Smart Waste Sorting Bin — ISBAT University Diploma in Software Engineering, IoT Unit.

---

## Stack
| Layer       | Technology                          |
|-------------|-------------------------------------|
| Frontend    | Vue 3 (CDN, no build step)          |
| Backend     | Node.js + Express                   |
| Database    | MySQL 8.0                           |
| Messaging   | MQTT (Mosquitto broker)             |
| SMS Alerts  | Africa's Talking API (optional)     |

---

## Prerequisites
- Node.js 18+
- MySQL 8.0
- Mosquitto MQTT broker (`sudo apt install mosquitto mosquitto-clients`)

---

## Setup

### 1. Database
```bash
mysql -u root -p < schema.sql
```

### 2. Environment
```bash
cp .env.example .env
# Edit .env with your MySQL password and optional AT credentials
```

### 3. Dependencies
```bash
npm install
```

### 4. Start Mosquitto (if not running as a service)
```bash
mosquitto -v
```

### 5. Start the backend
```bash
npm start
# or for development with auto-reload:
npm run dev
```

### 6. Open the dashboard
Place `index.html` inside a folder called `public/` next to `server.js`, then visit:
```
http://localhost:3000
```

---

## MQTT Topics

The ESP32 should publish to these topics:

| Topic                        | Payload (JSON)                                      | Description                    |
|------------------------------|-----------------------------------------------------|--------------------------------|
| `smartbin/fill/metal`        | `{ "fill_pct": 45, "distance_cm": 27.5 }`          | Metal compartment fill level   |
| `smartbin/fill/organic`      | `{ "fill_pct": 72, "distance_cm": 14.0 }`          | Organic compartment fill level |
| `smartbin/fill/inorganic`    | `{ "fill_pct": 30, "distance_cm": 35.0 }`          | Inorganic fill level           |
| `smartbin/classify`          | `{ "category": "metal", "confidence": 0.95 }`      | Item classification event      |
| `smartbin/sensor/status`     | `{ "sensor": "metal", "ok": true }`                | Sensor health report           |
| `smartbin/system/heartbeat`  | `{ "uptime_s": 3600, "free_heap": 120000 }`        | ESP32 heartbeat                |

---

## REST API Endpoints

| Method | Endpoint                            | Description                         |
|--------|-------------------------------------|-------------------------------------|
| GET    | `/api/status`                       | Current in-memory state (fast)      |
| GET    | `/api/events?limit=30`              | Recent classification events        |
| GET    | `/api/fill-history?category=metal&hours=24` | Fill level trend data       |
| GET    | `/api/composition?date=2025-01-01`  | Daily waste composition breakdown   |
| POST   | `/api/simulate`                     | Inject test data (dev mode only)    |

### POST /api/simulate examples

Simulate a fill level update:
```json
{ "type": "fill", "category": "metal", "fill_pct": 85 }
```

Simulate a classification event:
```json
{ "type": "classify", "category": "organic", "confidence": 0.92 }
```

---

## Testing Without Hardware

While the ESP32 is not yet ready, use the simulate endpoint to test the dashboard:

```bash
# Trigger a classification event
curl -X POST http://localhost:3000/api/simulate \
  -H "Content-Type: application/json" \
  -d '{"type":"classify","category":"metal"}'

# Set fill level
curl -X POST http://localhost:3000/api/simulate \
  -H "Content-Type: application/json" \
  -d '{"type":"fill","category":"organic","fill_pct":78}'
```

---

## ESP32 Firmware Notes

The ESP32 should:
1. Connect to Wi-Fi
2. Connect to the MQTT broker at `mqtt://[laptop-ip]:1883`
3. Publish fill readings every 60 seconds per compartment
4. Publish a `smartbin/classify` event immediately when an item is sorted
5. Publish `smartbin/system/heartbeat` every 30 seconds

Use the `PubSubClient` library for Arduino MQTT support.
