-- ============================================================
--  SmartBin Database Schema
--  Run once: mysql -u root -p < schema.sql
-- ============================================================

CREATE DATABASE IF NOT EXISTS smartbin
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE smartbin;

-- ── Classification Events ────────────────────────────────────
-- One row per item disposed into the bin
CREATE TABLE IF NOT EXISTS classification_events (
  id           INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  category     ENUM('metal','organic','inorganic') NOT NULL,
  confidence   FLOAT NULL COMMENT 'Sensor confidence 0.0–1.0, NULL if not applicable',
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_category    (category),
  INDEX idx_created_at  (created_at)
) ENGINE=InnoDB;

-- ── Fill Level Readings ──────────────────────────────────────
-- One row per HC-SR04 reading, per compartment, on each MQTT publish
CREATE TABLE IF NOT EXISTS fill_readings (
  id           INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  category     ENUM('metal','organic','inorganic') NOT NULL,
  fill_pct     TINYINT UNSIGNED NOT NULL COMMENT '0–100',
  distance_cm  FLOAT NULL COMMENT 'Raw ultrasonic distance in cm',
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_category_time (category, created_at)
) ENGINE=InnoDB;

-- ── Alert Log ────────────────────────────────────────────────
-- Records every triggered alert (full bin, sensor error, SMS sent)
CREATE TABLE IF NOT EXISTS alert_log (
  id           INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  alert_type   ENUM('bin_full','sensor_error','sms_sent') NOT NULL,
  category     ENUM('metal','organic','inorganic') NULL,
  message      VARCHAR(255) NOT NULL,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_created_at (created_at)
) ENGINE=InnoDB;

-- ── System Events ────────────────────────────────────────────
-- Heartbeat and connection state changes for diagnostics
CREATE TABLE IF NOT EXISTS system_events (
  id           INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  event_type   ENUM('mqtt_connect','mqtt_disconnect','server_start','daily_reset') NOT NULL,
  detail       VARCHAR(255) NULL,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- ── Useful Views ─────────────────────────────────────────────

-- Today's composition summary
CREATE OR REPLACE VIEW v_today_composition AS
  SELECT
    category,
    COUNT(*) AS item_count,
    ROUND(COUNT(*) * 100.0 / SUM(COUNT(*)) OVER (), 1) AS pct
  FROM classification_events
  WHERE DATE(created_at) = CURDATE()
  GROUP BY category;

-- Latest fill level per compartment
CREATE OR REPLACE VIEW v_latest_fill AS
  SELECT
    f1.category,
    f1.fill_pct,
    f1.distance_cm,
    f1.created_at
  FROM fill_readings f1
  INNER JOIN (
    SELECT category, MAX(created_at) AS max_ts
    FROM fill_readings
    GROUP BY category
  ) f2 ON f1.category = f2.category AND f1.created_at = f2.max_ts;

-- Daily item counts for the last 7 days
CREATE OR REPLACE VIEW v_weekly_activity AS
  SELECT
    DATE(created_at)  AS day,
    category,
    COUNT(*)           AS item_count
  FROM classification_events
  WHERE created_at >= CURDATE() - INTERVAL 7 DAY
  GROUP BY DATE(created_at), category
  ORDER BY day DESC, category;
