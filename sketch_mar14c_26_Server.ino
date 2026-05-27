#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <SD.h>
#include <BME280Spi.h>
#include <time.h>
#include <ArduinoJson.h>
#include <sqlite3.h>

// ==================== E-PAPER БИБЛИОТЕКИ ====================
#include "epd1in54_V2.h"
#include "imagedata.h"
#include "epdpaint.h"

#define SERIAL_BAUD 115200

// ==================== Wi-Fi НАСТРОЙКИ ====================
#define STA_SSID "HONOR X7"
#define STA_PASS "ixw2yybbz5e926"
#define AP_SSID  "netSensorModule-01"
#define AP_PASS  "188B0E14E2C1"
#define MDNS_NAME "SensorModule-C3"
IPAddress ap_ip(192, 168, 10, 1);
IPAddress ap_mask(255, 255, 255, 0);

// ==================== Пины ====================
#define SOIL_PIN       0
#define CS_BME280_PIN  8
#define CS_SD_PIN      SS  // Пин CS для SD-карты

// ==================== E-PAPER НАСТРОЙКИ ====================
#define COLORED     0
#define UNCOLORED   1
Epd epd;
unsigned char image[1024];
Paint paint(image, 0, 0);
char strData[10];

// ==================== Объекты ====================
BME280Spi::Settings settings(CS_BME280_PIN);
BME280Spi bme(settings);
WebServer server(80);

// ==================== SQLite БАЗА ДАННЫХ на SD-карте ====================
#define DB_FILE "/sd/sensor_log.db"
#define MAX_LOG_ENTRIES 1000

sqlite3 *db = nullptr;
sqlite3_stmt *res = nullptr;
int rc;
char *zErrMsg = 0;

// ==================== Глобальные переменные ====================
unsigned long lastLogTime = 0;
int currentSoil = 0, currentHum = 0, currentTemp = 0, currentPres = 0;
time_t bootTime = 0;
bool timeSynced = false;

// ==================== ФОРМАТИРОВАНИЕ ВРЕМЕНИ ====================
String formatTime(time_t t, bool showSeconds = false) {
  if (!timeSynced || t < 1700000000) {
    if (bootTime == 0) bootTime = millis() / 1000;
    unsigned long uptime = (millis() / 1000) - bootTime;
    int days = uptime / 86400;
    int hours = (uptime % 86400) / 3600;
    int mins = (uptime % 3600) / 60;
    int secs = uptime % 60;
    char buf[32];
    if (days > 0) sprintf(buf, "UP %dd %02d:%02d", days, hours, mins);
    else if (showSeconds) sprintf(buf, "UP %02d:%02d:%02d", hours, mins, secs);
    else sprintf(buf, "UP %02d:%02d", hours, mins);
    return String(buf);
  }
  struct tm timeinfo;
  if (localtime_r(&t, &timeinfo)) {
    char buf[20];
    if (showSeconds) strftime(buf, sizeof(buf), "%d.%m %H:%M:%S", &timeinfo);
    else strftime(buf, sizeof(buf), "%d.%m %H:%M", &timeinfo);
    return String(buf);
  }
  return "??.?? ??:??";
}

// ==================== ИНИЦИАЛИЗАЦИЯ SD-КАРТЫ ====================
bool initSDCard() {
  SPI.begin();
  
  if (!SD.begin(CS_SD_PIN)) {
    Serial.printf("Ошибка: SD-модуль не обнаружен\n");
    return false;
  }
  
  Serial.printf("SD-модуль обнаружен\n");
  
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("Ошибка: карта памяти отсутствует");
    return false;
  }
  
  Serial.printf("Карта памяти:\n\tтип - ");
  switch(cardType) {
    case CARD_MMC:
      Serial.printf("MMC\n");
      break;
    case CARD_SD:
      Serial.printf("SDSC\n");
      break;
    case CARD_SDHC:
      Serial.printf("SDHC\n");
      break;
    default:
      Serial.print("неизвестен\n");
  }
  
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("\tРазмер - %llu MB\n", cardSize);
  
  return true;
}

// ==================== ИНИЦИАЛИЗАЦИЯ БАЗЫ ДАННЫХ ====================
void initDB() {
  Serial.println("=== Инициализация SQLite БД ===");
  
  if (!initSDCard()) {
    Serial.println("Ошибка: SD-карта не доступна!");
    return;
  }
  
  sqlite3_initialize();
  
  const char* dbPath = DB_FILE;
  Serial.printf("Попытка открытия БД: %s\n", dbPath);
  
  rc = sqlite3_open(dbPath, &db);
  
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка открытия БД: %d\n", rc);
    Serial.printf("Сообщение: %s\n", sqlite3_errmsg(db));
    db = nullptr;
    return;
  }
  
  Serial.println("БД успешно открыта");
  
  String sql = "CREATE TABLE IF NOT EXISTS SensorLog ("
               "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
               "ts INTEGER NOT NULL, "
               "time TEXT, "
               "temp REAL, "
               "hum REAL, "
               "soil REAL, "
               "pressure REAL);";
  
  Serial.println("Создание таблицы SensorLog...");
  rc = sqlite3_exec(db, sql.c_str(), NULL, NULL, &zErrMsg);
  
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания таблицы: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  } else {
    Serial.println("Таблица создана/проверена");
  }
  
  testDBWrite();
}

void testDBWrite() {
  if (!db) return;
  
  String sql = "INSERT INTO SensorLog (ts, time, temp, hum, soil, pressure) VALUES (?,?,?,?,?,?);";
  rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &res, NULL);
  
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(res, 1, (int)time(nullptr));
    sqlite3_bind_text(res, 2, "TEST", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(res, 3, 25.0);
    sqlite3_bind_double(res, 4, 50.0);
    sqlite3_bind_double(res, 5, 75.0);
    sqlite3_bind_double(res, 6, 760.0);
    
    rc = sqlite3_step(res);
    if (rc == SQLITE_DONE) {
      Serial.println("Тестовая запись успешно добавлена");
    } else {
      Serial.printf("Ошибка тестовой записи: %d\n", rc);
    }
    sqlite3_finalize(res);
    
    sqlite3_exec(db, "DELETE FROM SensorLog WHERE time='TEST';", NULL, NULL, NULL);
  } else {
    Serial.printf("Ошибка подготовки тестового запроса: %s\n", sqlite3_errmsg(db));
  }
}

// ==================== ЗАПИСЬ В БАЗУ ДАННЫХ ====================
void logToDB(int soil, int hum, int temp, int press) {
  if (!db) {
    Serial.println("БД недоступна, запись пропущена");
    return;
  }
  
  time_t now = time(nullptr);
  String timeStr = formatTime(now, true);
  
  String sql = "INSERT INTO SensorLog (ts, time, temp, hum, soil, pressure) VALUES (?,?,?,?,?,?);";
  rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &res, NULL);
  
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(res, 1, (int)now);
    sqlite3_bind_text(res, 2, timeStr.c_str(), timeStr.length(), SQLITE_TRANSIENT);
    sqlite3_bind_double(res, 3, temp);
    sqlite3_bind_double(res, 4, hum);
    sqlite3_bind_double(res, 5, soil);
    sqlite3_bind_double(res, 6, press);
    
    rc = sqlite3_step(res);
    if (rc != SQLITE_DONE) {
      Serial.printf("Ошибка записи в БД: %d\n", rc);
    }
    sqlite3_finalize(res);
  } else {
    Serial.printf("Ошибка подготовки запроса: %s\n", sqlite3_errmsg(db));
  }
}

// ==================== ЧТЕНИЕ ИЗ БАЗЫ ДАННЫХ ====================
String readHistoryFromDB(int periodSeconds, int limitCount, time_t customStart = 0, time_t customEnd = 0) {
  if (!db) return "[]";
  
  DynamicJsonDocument doc(32768);
  JsonArray arr = doc.to<JsonArray>();
  
  String sql = "SELECT ts, time, temp, hum, soil, pressure FROM SensorLog";
  
  if (periodSeconds > 0) {
    time_t now = time(nullptr);
    time_t cutoff = now - periodSeconds;
    sql += " WHERE ts >= " + String(cutoff);
  } else if (customStart > 0 && customEnd > 0) {
    sql += " WHERE ts >= " + String(customStart) + " AND ts <= " + String(customEnd);
  }
  
  sql += " ORDER BY ts ASC";
  
  if (limitCount > 0) {
    sql += " LIMIT " + String(limitCount);
  }
  
  rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &res, NULL);
  if (rc == SQLITE_OK) {
    int count = 0;
    while (sqlite3_step(res) == SQLITE_ROW) {
      JsonObject entry = arr.createNestedObject();
      entry["ts"] = sqlite3_column_int(res, 0);
      const char* t = (const char*)sqlite3_column_text(res, 1);
      entry["time"] = t ? String(t) : "";
      entry["temp"] = sqlite3_column_double(res, 2);
      entry["hum"] = sqlite3_column_double(res, 3);
      entry["soil"] = sqlite3_column_double(res, 4);
      entry["pressure"] = sqlite3_column_double(res, 5);
      count++;
    }
    sqlite3_finalize(res);
    Serial.printf("Прочитано %d записей из БД\n", count);
  } else {
    Serial.printf("Ошибка чтения из БД: %s\n", sqlite3_errmsg(db));
  }
  
  String out;
  serializeJson(arr, out);
  return out;
}

void clearDB() {
  if (db) {
    sqlite3_exec(db, "DELETE FROM SensorLog;", NULL, NULL, &zErrMsg);
    sqlite3_free(zErrMsg);
    Serial.println("БД очищена");
  }
}

// ==================== E-PAPER ФУНКЦИИ ====================
void updateEpaper(int soil, int hum, int temp, int press) {
  paint.Clear(UNCOLORED);
  
  // Влажность почвы
  sprintf(strData, "%d", soil);
  paint.DrawStringAt(0, 0, strData, &Font24, COLORED);
  epd.SetFrameMemoryPartial(paint.GetImage(), 150, 116, 56, 24);
  
  paint.Clear(UNCOLORED);
  
  // Влажность воздуха
  sprintf(strData, "%d", hum);
  paint.DrawStringAt(0, 0, strData, &Font24, COLORED);
  epd.SetFrameMemoryPartial(paint.GetImage(), 150, 60, 56, 24);
  
  paint.Clear(UNCOLORED);
  
  // Температура
  sprintf(strData, "%d", temp);
  paint.DrawStringAt(0, 0, strData, &Font24, COLORED);
  epd.SetFrameMemoryPartial(paint.GetImage(), 20, 60, 56, 24);
  
  paint.Clear(UNCOLORED);
  
  // Давление
  sprintf(strData, "%d", press);
  paint.DrawStringAt(0, 0, strData, &Font24, COLORED);
  epd.SetFrameMemoryPartial(paint.GetImage(), 20, 116, 56, 24);
  
  epd.DisplayPartFrame();
}

// ==================== ЧТЕНИЕ ДАТЧИКОВ И ЛОГИРОВАНИЕ ====================
void readAndLog() {
  // Чтение датчика влажности почвы
  int raw = analogRead(SOIL_PIN);
  int soil = 100 - (raw - 1200) / 19;
  soil = constrain(soil, 0, 100);
  
  // Чтение BME280
  float pressure_pa = 0, temp_c = 0, humidity_pct = 0;
  BME280::TempUnit tu(BME280::TempUnit_Celsius);
  BME280::PresUnit pu(BME280::PresUnit_Pa);
  bme.read(pressure_pa, temp_c, humidity_pct, tu, pu);
  
  // Сохранение значений
  currentSoil = soil;
  currentHum = round(humidity_pct);
  currentTemp = round(temp_c);
  currentPres = round(pressure_pa * 0.00750062);  // Перевод в мм рт.ст.
  
  // Логирование в БД
  logToDB(currentSoil, currentHum, currentTemp, currentPres);
  
  // Обновление e-paper дисплея
  updateEpaper(currentSoil, currentHum, currentTemp, currentPres);
}

// ==================== HTML + JS (без изменений) ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>Мониторинг растений</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
:root {
--bg: #f0f2f5; --card: #ffffff; --text: #1a1a2e; --text2: #6b7280;
--border: #e5e7eb; --shadow: 0 4px 20px rgba(0,0,0,0.08);
--primary: #10b981; --primary-dark: #059669; --accent: #3b82f6;
--warning: #f59e0b; --danger: #ef4444; --success: #22c55e;
}
[data-theme="dark"] {
--bg: #0f172a; --card: #1e293b; --text: #f1f5f9; --text2: #94a3b8;
--border: #334155; --shadow: 0 4px 20px rgba(0,0,0,0.4);
--primary: #22c55e; --primary-dark: #16a34a; --accent: #60a5fa;
}
body {
font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
background: var(--bg); color: var(--text);
transition: background 0.3s ease, color 0.3s ease;
min-height: 100vh; line-height: 1.5;
}
.theme-toggle {
position: fixed; top: 12px; right: 12px; z-index: 100;
width: 40px; height: 40px; border: none; border-radius: 12px;
background: var(--card); box-shadow: var(--shadow);
cursor: pointer; font-size: 18px; display: flex; align-items: center; justify-content: center;
transition: transform 0.2s, background 0.3s ease, color 0.3s ease;
color: var(--text);
}
.theme-toggle:active { transform: scale(0.95); }
header {
padding: 16px 20px; text-align: center;
font-size: 18px; font-weight: 600; color: var(--primary);
background: var(--card); box-shadow: var(--shadow);
position: sticky; top: 0; z-index: 50;
display: flex; align-items: center; justify-content: center; gap: 8px;
transition: background 0.3s ease;
}
header .status { font-size: 12px; color: var(--text2); font-weight: 400; }
header .sync-status { font-size: 10px; color: var(--success); margin-left: 4px; }
header .sync-status.error { color: var(--danger); }
.tabs {
display: flex; justify-content: center; gap: 6px; padding: 10px;
background: var(--card); border-bottom: 1px solid var(--border);
position: sticky; top: 56px; z-index: 45;
transition: background 0.3s ease;
}
.tab {
padding: 8px 16px; border: none; background: transparent;
color: var(--text2); cursor: pointer; border-radius: 10px;
font-size: 14px; font-weight: 500; transition: all 0.2s;
}
.tab:hover { background: rgba(16,185,129,0.1); color: var(--primary); }
.tab.active { background: var(--primary); color: #fff; }
.container { max-width: 800px; margin: 0 auto; padding: 14px; }
.content { display: none; animation: fadeIn 0.25s ease-out; }
.content.active { display: block; }
@keyframes fadeIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
.metrics-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; }
.metric-card {
background: var(--card); padding: 16px 14px; border-radius: 16px;
text-align: center; box-shadow: var(--shadow); border: 1px solid var(--border);
transition: transform 0.2s, box-shadow 0.2s, background 0.3s ease;
}
.metric-card:hover { transform: translateY(-2px); box-shadow: 0 8px 30px rgba(0,0,0,0.12); }
.metric-label { font-size: 12px; color: var(--text2); text-transform: uppercase; letter-spacing: 0.5px; }
.metric-value { font-size: 28px; font-weight: 700; color: var(--primary); margin: 6px 0 2px; }
.metric-unit { font-size: 13px; color: var(--text2); font-weight: 500; }
.history-panel, .chart-panel {
margin-top: 16px; background: var(--card); padding: 14px;
border-radius: 16px; box-shadow: var(--shadow); border: 1px solid var(--border);
transition: background 0.3s ease;
}
.history-header, .chart-header {
display: flex; justify-content: space-between; align-items: center;
margin-bottom: 12px; flex-wrap: wrap; gap: 8px;
}
.history-title, .chart-title { font-weight: 600; font-size: 15px; }
.filter-group, .chart-filters { display: flex; gap: 4px; flex-wrap: wrap; }
.filter-btn, .chart-filter {
padding: 5px 11px; border: 1px solid var(--border); background: transparent;
color: var(--text2); cursor: pointer; border-radius: 20px;
font-size: 11px; font-weight: 500; transition: all 0.2s;
}
.filter-btn:hover, .chart-filter:hover { border-color: var(--primary); color: var(--primary); }
.filter-btn.active, .chart-filter.active { background: var(--primary); color: #fff; border-color: var(--primary); }
.filter-btn.danger { color: var(--danger); border-color: var(--danger); }
.filter-btn.danger:hover { background: var(--danger); color: #fff; }
.custom-range {
display: flex; gap: 5px; align-items: center; margin-left: 5px;
}
.custom-range input {
padding: 4px; border: 1px solid var(--border); border-radius: 4px;
background: var(--bg); color: var(--text); font-size: 11px;
}
.history-list {
max-height: 280px; overflow-y: auto; border-radius: 10px;
background: rgba(16,185,129,0.03); padding: 8px;
}
.history-item {
padding: 10px 12px; border-bottom: 1px solid var(--border);
font-size: 13px; display: flex; justify-content: space-between; align-items: center;
}
.history-item:last-child { border-bottom: none; }
.history-time { font-weight: 600; color: var(--text); min-width: 105px; font-family: 'Courier New', monospace; font-size: 13px; }
.history-values { display: flex; gap: 12px; flex-wrap: wrap; font-size: 12px; color: var(--text2); }
.history-values span { display: flex; align-items: center; gap: 3px; }
.history-values .val-temp { color: #ef4444; }
.history-values .val-hum { color: #3b82f6; }
.history-values .val-soil { color: #22c55e; }
.history-values .val-press { color: #a855f7; }
.history-empty { text-align: center; padding: 20px; color: var(--text2); font-style: italic; }
.chart-container {
height: 280px; position: relative; background: rgba(16,185,129,0.04);
border-radius: 12px; padding: 12px; margin-bottom: 10px;
}
canvas { width: 100%; height: 100%; display: block; }
.chart-legend {
display: flex; justify-content: center; gap: 10px; flex-wrap: wrap;
font-size: 12px; color: var(--text2);
}
.legend-item {
display: flex; align-items: center; gap: 5px; cursor: pointer;
padding: 4px 8px; border-radius: 16px; transition: background 0.2s;
}
.legend-item:hover { background: rgba(16,185,129,0.1); }
.legend-item.active {
background: var(--accent); color: #fff;
box-shadow: 0 2px 8px rgba(59,130,246,0.3);
}
.legend-dot { width: 12px; height: 12px; border-radius: 3px; }
.chart-tooltip {
position: absolute; padding: 8px 12px; background: var(--card);
border: 1px solid var(--border); border-radius: 8px; box-shadow: var(--shadow);
font-size: 12px; pointer-events: none; z-index: 20; display: none;
max-width: 200px; transition: background 0.3s ease, border-color 0.3s ease;
}
.chart-tooltip.visible { display: block; }
.tooltip-time { font-weight: 600; margin-bottom: 4px; color: var(--text); }
.tooltip-row { display: flex; justify-content: space-between; gap: 12px; margin: 2px 0; }
.tooltip-color { width: 10px; height: 10px; border-radius: 2px; display: inline-block; margin-right: 4px; }
.loading {
display: flex; align-items: center; justify-content: center;
padding: 20px; color: var(--text2); font-style: italic;
}
.loading::after {
content: ''; width: 16px; height: 16px; border: 2px solid var(--border);
border-top-color: var(--primary); border-radius: 50%;
animation: spin 0.8s linear infinite; margin-left: 8px;
}
@keyframes spin { to { transform: rotate(360deg); } }
.chart-mode-indicator {
font-size: 11px; color: var(--text2); margin-left: 8px;
padding: 2px 8px; background: rgba(16,185,129,0.1);
border-radius: 12px; display: none;
}
.chart-mode-indicator.visible { display: inline; }
.y-axis-label {
position: absolute; left: 5px; top: 50%; transform: translateY(-50%) rotate(-90deg);
font-size: 11px; font-weight: 600; color: var(--text); pointer-events: none; white-space: nowrap; letter-spacing: 1px;
}
.y-axis-values {
position: absolute; left: 15px; top: 0; bottom: 40px; display: flex;
flex-direction: column; justify-content: space-between; padding: 25px 0; pointer-events: none;
}
.y-axis-value {
font-size: 10px; color: var(--text2); text-align: right; font-family: 'Courier New', monospace;
}
@media (max-width: 500px) {
.metrics-grid { grid-template-columns: 1fr; }
.history-header, .chart-header { flex-direction: column; align-items: flex-start; }
.filter-group, .chart-filters { width: 100%; justify-content: center; }
.history-values { flex-direction: column; gap: 4px; }
.history-time { min-width: 95px; font-size: 12px; }
}
</style>
</head>
<body>
<button class="theme-toggle" id="themeBtn" title="Переключить тему">◑</button>
<header>
Мониторинг растений
<span class="status" id="timeStatus">--:--</span>
<span class="sync-status" id="syncStatus">●</span>
</header>
<div class="tabs">
<button class="tab active" data-tab="main">Показатели</button>
<button class="tab" data-tab="charts">Графики</button>
</div>
<div class="container">
<div id="tab-main" class="content active">
<div class="metrics-grid">
<div class="metric-card">
<div class="metric-label">Температура</div>
<div class="metric-value" id="temp">--</div>
<div class="metric-unit">°C</div>
</div>
<div class="metric-card">
<div class="metric-label">Влажность воздуха</div>
<div class="metric-value" id="hum">--</div>
<div class="metric-unit">%</div>
</div>
<div class="metric-card">
<div class="metric-label">Влажность почвы</div>
<div class="metric-value" id="soil">--</div>
<div class="metric-unit">%</div>
</div>
<div class="metric-card">
<div class="metric-label">Атм. давление</div>
<div class="metric-value" id="pressure">--</div>
<div class="metric-unit">мм рт.ст.</div>
</div>
</div>
<div class="history-panel">
<div class="history-header">
<div class="history-title">История измерений</div>
<div class="filter-group">
<button class="filter-btn active" data-period="0" data-limit="20">Посл. 20</button>
<button class="filter-btn" data-period="86400" data-limit="0">1 день</button>
<button class="filter-btn" data-period="604800" data-limit="0">7 дней</button>
<button class="filter-btn" data-period="2592000" data-limit="0">Месяц</button>
<button class="filter-btn" id="historyDateBtn">Дата</button>
<button class="filter-btn danger" id="clearBtn" title="Очистить историю">X</button>
</div>
</div>
<div class="custom-range" id="historyCustomRange" style="display:none; margin-bottom:10px;">
<input type="datetime-local" id="historyStartDate">
<input type="datetime-local" id="historyEndDate">
<button class="filter-btn active" id="historyApplyBtn">Применить</button>
<button class="filter-btn" id="historyCancelBtn">Отмена</button>
</div>
<div class="history-list" id="historyList">
<div class="loading">Загрузка данных...</div>
</div>
</div>
</div>
<div id="tab-charts" class="content">
<div class="chart-panel">
<div class="chart-header">
<div>
<span class="chart-title">Динамика показателей</span>
<span class="chart-mode-indicator" id="chartModeIndicator">Режим: один параметр</span>
</div>
<div class="chart-filters">
<button class="chart-filter active" data-period="0" data-limit="20">Посл. 20</button>
<button class="chart-filter" data-period="86400" data-limit="0">1 день</button>
<button class="chart-filter" data-period="604800" data-limit="0">7 дней</button>
<button class="chart-filter" data-period="2592000" data-limit="0">Месяц</button>
<button class="chart-filter" id="chartDateBtn">Дата</button>
</div>
</div>
<div class="custom-range" id="chartCustomRange" style="display:none; margin-bottom:10px;">
<input type="datetime-local" id="chartStartDate">
<input type="datetime-local" id="chartEndDate">
<button class="filter-btn active" id="chartApplyBtn">Применить</button>
<button class="filter-btn" id="chartCancelBtn">Отмена</button>
</div>
<div class="chart-container">
<canvas id="chart"></canvas>
<div class="chart-tooltip" id="tooltip">
<div class="tooltip-time"></div>
</div>
<div class="y-axis-label" id="yAxisLabel"></div>
<div class="y-axis-values" id="yAxisValues"></div>
</div>
<div class="chart-legend" id="legend">
<div class="legend-item active" data-metric="all">
<span class="legend-dot" style="background:linear-gradient(135deg,#ef4444,#3b82f6,#22c55e,#a855f7)"></span>
Все параметры
</div>
<div class="legend-item" data-metric="temp"><span class="legend-dot" style="background:#ef4444"></span>Температура</div>
<div class="legend-item" data-metric="hum"><span class="legend-dot" style="background:#3b82f6"></span>Влажность</div>
<div class="legend-item" data-metric="soil"><span class="legend-dot" style="background:#22c55e"></span>Почва</div>
<div class="legend-item" data-metric="pressure"><span class="legend-dot" style="background:#a855f7"></span>Давление</div>
</div>
</div>
</div>
</div>
<script>
let chartData = [];
let currentHistoryPeriod = 0;
let currentHistoryLimit = 20;
let currentChartPeriod = 0;
let currentChartLimit = 20;
let visibleMetrics = { temp: true, hum: true, soil: true, pressure: true };
let currentChartMode = 'all';
let serverTimeOffset = 0;
let historyCustomStart = 0, historyCustomEnd = 0;
let chartCustomStart = 0, chartCustomEnd = 0;
const UPDATE_INTERVAL = 10000;

document.addEventListener('DOMContentLoaded', () => {
if (!localStorage.getItem('auth_token')) {
  window.location.href = '/';
  return;
}
initTheme();
initTabs();
initHistoryFilters();
initChartFilters();
initLegend();
initClearButton();
initHistoryCustomRange();
initChartCustomRange();
initChart();
syncTimeWithServer();
startAutoUpdate();
loadData();
});

async function syncTimeWithServer() {
try {
const res = await fetch('/time?t=' + Date.now());
const serverTimeStr = await res.text();
const now = new Date();
if (serverTimeStr && !serverTimeStr.startsWith('UP')) {
const parts = serverTimeStr.split(' ');
if (parts.length >= 2) {
const [datePart, timePart] = parts;
const [day, month, year] = datePart.split('.');
const [hours, minutes, seconds] = timePart.split(':');
const serverDate = new Date('20'+year+'-'+month+'-'+day+'T'+hours+':'+minutes+':'+seconds);
serverTimeOffset = serverDate.getTime() - now.getTime();
document.getElementById('timeStatus').textContent = serverTimeStr;
document.getElementById('syncStatus').className = 'sync-status';
document.getElementById('syncStatus').title = 'Время синхронизировано';
}
} else {
document.getElementById('syncStatus').className = 'sync-status error';
document.getElementById('syncStatus').title = 'Время не синхронизировано';
}
} catch (e) {
document.getElementById('syncStatus').className = 'sync-status error';
}
}

function initTheme() {
const saved = localStorage.getItem('theme');
if (saved === 'dark') document.documentElement.setAttribute('data-theme', 'dark');
document.getElementById('themeBtn').addEventListener('click', () => {
const html = document.documentElement;
const isDark = html.getAttribute('data-theme') === 'dark';
if (isDark) {
html.removeAttribute('data-theme');
localStorage.setItem('theme', 'light');
} else {
html.setAttribute('data-theme', 'dark');
localStorage.setItem('theme', 'dark');
}
if (document.getElementById('tab-charts').classList.contains('active')) drawChart();
});
}

function initTabs() {
document.querySelectorAll('.tab').forEach(btn => {
btn.addEventListener('click', () => {
document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
document.querySelectorAll('.content').forEach(c => c.classList.remove('active'));
btn.classList.add('active');
document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
if (btn.dataset.tab === 'charts') setTimeout(drawChart, 100);
});
});
}

function initHistoryFilters() {
document.querySelectorAll('.history-panel .filter-btn[data-period]').forEach(btn => {
btn.addEventListener('click', () => {
document.querySelectorAll('.history-panel .filter-btn[data-period]').forEach(b => b.classList.remove('active'));
btn.classList.add('active');
currentHistoryPeriod = parseInt(btn.dataset.period);
currentHistoryLimit = parseInt(btn.dataset.limit);
historyCustomStart = 0; historyCustomEnd = 0;
document.getElementById('historyCustomRange').style.display = 'none';
loadHistory();
});
});
}

function initChartFilters() {
document.querySelectorAll('.chart-panel .chart-filter[data-period]').forEach(btn => {
btn.addEventListener('click', () => {
document.querySelectorAll('.chart-panel .chart-filter[data-period]').forEach(b => b.classList.remove('active'));
btn.classList.add('active');
currentChartPeriod = parseInt(btn.dataset.period);
currentChartLimit = parseInt(btn.dataset.limit);
chartCustomStart = 0; chartCustomEnd = 0;
document.getElementById('chartCustomRange').style.display = 'none';
loadChartData();
});
});
}

function initHistoryCustomRange() {
document.getElementById('historyDateBtn').addEventListener('click', () => {
document.querySelectorAll('.history-panel .filter-btn[data-period]').forEach(b => b.classList.remove('active'));
document.getElementById('historyDateBtn').classList.add('active');
document.getElementById('historyCustomRange').style.display = 'flex';
const now = new Date();
now.setMinutes(now.getMinutes() - now.getTimezoneOffset());
document.getElementById('historyEndDate').value = now.toISOString().slice(0,16);
const yesterday = new Date(now.getTime() - 24*60*60*1000);
document.getElementById('historyStartDate').value = yesterday.toISOString().slice(0,16);
});
document.getElementById('historyApplyBtn').addEventListener('click', () => {
const startVal = document.getElementById('historyStartDate').value;
const endVal = document.getElementById('historyEndDate').value;
if (startVal && endVal) {
historyCustomStart = Math.floor(new Date(startVal).getTime() / 1000);
historyCustomEnd = Math.floor(new Date(endVal).getTime() / 1000);
currentHistoryPeriod = 0; currentHistoryLimit = 0;
loadHistory();
document.getElementById('historyCustomRange').style.display = 'none';
}
});
document.getElementById('historyCancelBtn').addEventListener('click', () => {
document.getElementById('historyCustomRange').style.display = 'none';
document.querySelectorAll('.history-panel .filter-btn[data-period]').forEach(b => b.classList.remove('active'));
document.querySelector('.history-panel .filter-btn[data-period="0"]').classList.add('active');
currentHistoryPeriod = 0; currentHistoryLimit = 20;
historyCustomStart = 0; historyCustomEnd = 0;
loadHistory();
});
}

function initChartCustomRange() {
document.getElementById('chartDateBtn').addEventListener('click', () => {
document.querySelectorAll('.chart-panel .chart-filter[data-period]').forEach(b => b.classList.remove('active'));
document.getElementById('chartDateBtn').classList.add('active');
document.getElementById('chartCustomRange').style.display = 'flex';
const now = new Date();
now.setMinutes(now.getMinutes() - now.getTimezoneOffset());
document.getElementById('chartEndDate').value = now.toISOString().slice(0,16);
const yesterday = new Date(now.getTime() - 24*60*60*1000);
document.getElementById('chartStartDate').value = yesterday.toISOString().slice(0,16);
});
document.getElementById('chartApplyBtn').addEventListener('click', () => {
const startVal = document.getElementById('chartStartDate').value;
const endVal = document.getElementById('chartEndDate').value;
if (startVal && endVal) {
chartCustomStart = Math.floor(new Date(startVal).getTime() / 1000);
chartCustomEnd = Math.floor(new Date(endVal).getTime() / 1000);
currentChartPeriod = 0; currentChartLimit = 0;
loadChartData();
document.getElementById('chartCustomRange').style.display = 'none';
}
});
document.getElementById('chartCancelBtn').addEventListener('click', () => {
document.getElementById('chartCustomRange').style.display = 'none';
document.querySelectorAll('.chart-panel .chart-filter[data-period]').forEach(b => b.classList.remove('active'));
document.querySelector('.chart-panel .chart-filter[data-period="0"]').classList.add('active');
currentChartPeriod = 0; currentChartLimit = 20;
chartCustomStart = 0; chartCustomEnd = 0;
loadChartData();
});
}

function initLegend() {
document.querySelectorAll('.legend-item').forEach(item => {
item.addEventListener('click', () => {
const metric = item.dataset.metric;
document.querySelectorAll('.legend-item').forEach(i => i.classList.remove('active'));
item.classList.add('active');
if (metric === 'all') {
currentChartMode = 'all';
visibleMetrics = { temp: true, hum: true, soil: true, pressure: true };
document.getElementById('chartModeIndicator').classList.remove('visible');
document.getElementById('yAxisLabel').style.display = 'none';
document.getElementById('yAxisValues').innerHTML = '';
} else {
currentChartMode = metric;
visibleMetrics = { temp: false, hum: false, soil: false, pressure: false };
visibleMetrics[metric] = true;
document.getElementById('chartModeIndicator').classList.add('visible');
const labels = { temp: '°C', hum: '%', soil: '%', pressure: 'мм' };
const labelEl = document.getElementById('yAxisLabel');
labelEl.textContent = labels[metric] || '';
labelEl.style.color = getMetricStyle(metric).color;
labelEl.style.display = 'block';
}
drawChart();
});
});
}

function initClearButton() {
document.getElementById('clearBtn').addEventListener('click', async () => {
if (!confirm('Удалить всю историю измерений?')) return;
try {
await fetch('/clear', { method: 'POST' });
loadHistory();
if (document.getElementById('tab-charts').classList.contains('active')) loadChartData();
} catch (e) { alert('Ошибка при очистке'); }
});
}

async function loadData() {
await Promise.all([fetchMetrics(), loadHistory()]);
if (document.getElementById('tab-charts').classList.contains('active')) await loadChartData();
}

async function fetchMetrics() {
try {
const res = await fetch('/data?t=' + Date.now());
const data = await res.json();
updateMetrics(data);
syncTimeWithServer();
} catch (e) { console.log('Ошибка загрузки показателей'); }
}

function updateMetrics(d) {
document.getElementById('temp').textContent = d.temp ?? '--';
document.getElementById('hum').textContent = d.humidity ?? '--';
document.getElementById('soil').textContent = d.soil ?? '--';
document.getElementById('pressure').textContent = d.pressure ?? '--';
}

async function loadHistory() {
const list = document.getElementById('historyList');
list.innerHTML = '<div class="loading">Загрузка...</div>';
try {
let url = '/hist?period='+currentHistoryPeriod+'&limit='+currentHistoryLimit;
if (historyCustomStart > 0 && historyCustomEnd > 0) {
url += '&start='+historyCustomStart+'&end='+historyCustomEnd;
}
url += '&t='+Date.now();
const res = await fetch(url);
chartData = await res.json();
renderHistory(chartData);
} catch (e) {
list.innerHTML = '<div class="history-empty">Ошибка загрузки</div>';
}
}

function renderHistory(data) {
const list = document.getElementById('historyList');
if (!data || data.length === 0) {
list.innerHTML = '<div class="history-empty">Нет данных за выбранный период</div>';
return;
}
let html = '';
data.slice().reverse().forEach(item => {
let timeDisplay = item.time || '--:--:--';
html += '<div class="history-item">' +
'<span class="history-time">' + timeDisplay + '</span>' +
'<div class="history-values">' +
'<span class="val-temp">T:' + (item.temp!=null?item.temp:'-') + '°C</span>' +
'<span class="val-hum">H:' + (item.hum!=null?item.hum:'-') + '%</span>' +
'<span class="val-soil">S:' + (item.soil!=null?item.soil:'-') + '%</span>' +
'<span class="val-press">P:' + (item.pressure!=null?item.pressure:'-') + 'мм</span>' +
'</div></div>';
});
list.innerHTML = html;
}

async function loadChartData() {
try {
let url = '/hist?period='+currentChartPeriod+'&limit='+currentChartLimit;
if (chartCustomStart > 0 && chartCustomEnd > 0) {
url += '&start='+chartCustomStart+'&end='+chartCustomEnd;
}
url += '&t='+Date.now();
const res = await fetch(url);
chartData = await res.json();
drawChart();
} catch (e) { console.log('Ошибка загрузки графика'); }
}

let chartCtx, chartCanvas;
function initChart() {
chartCanvas = document.getElementById('chart');
chartCtx = chartCanvas.getContext('2d');
chartCanvas.addEventListener('mousemove', handleChartHover);
chartCanvas.addEventListener('mouseleave', hideTooltip);
chartCanvas.addEventListener('touchmove', (e) => { e.preventDefault(); handleChartHover(e); }, {passive: false});
window.addEventListener('resize', () => {
if (document.getElementById('tab-charts').classList.contains('active')) drawChart();
});
}

function drawChart() {
if (!chartCtx || !chartData || chartData.length < 2) {
drawEmptyChart();
return;
}
const dpr = window.devicePixelRatio || 1;
const rect = chartCanvas.getBoundingClientRect();
chartCanvas.width = rect.width * dpr;
chartCanvas.height = rect.height * dpr;
chartCtx.scale(dpr, dpr);
const W = rect.width, H = rect.height;
const P = { t: 25, r: 40, b: 40, l: 45 };
const cW = W - P.l - P.r, cH = H - P.t - P.b;
chartCtx.clearRect(0, 0, W, H);
const activeMetrics = currentChartMode === 'all'
? Object.keys(visibleMetrics).filter(k => visibleMetrics[k])
: [currentChartMode];
if (activeMetrics.length === 0) {
drawEmptyChart();
return;
}
const ranges = calculateRanges(chartData, activeMetrics);
drawGrid(P, cW, cH, ranges, W, H, activeMetrics);
drawYAxisValues(P, cH, ranges, activeMetrics);
const lines = {};
activeMetrics.forEach(key => {
lines[key] = drawLine(chartData, key, ranges[key], P, cW, cH, getMetricStyle(key));
});
chartCanvas._lines = lines;
chartCanvas._params = { P, cW, cH, ranges, activeMetrics };
}

function calculateRanges(data, activeMetrics) {
const ranges = {};
const metricInfo = {
temp: { key: 'temp', label: '°C', defaultMin: 10, defaultMax: 35, color: '#ef4444' },
hum: { key: 'hum', label: '%', defaultMin: 0, defaultMax: 100, color: '#3b82f6' },
soil: { key: 'soil', label: '%', defaultMin: 0, defaultMax: 100, color: '#22c55e' },
pressure: { key: 'pressure', label: 'мм', defaultMin: 740, defaultMax: 770, color: '#a855f7' }
};
activeMetrics.forEach(metric => {
const info = metricInfo[metric];
let min = Infinity, max = -Infinity;
data.forEach(d => {
const val = d[info.key];
if (val != null) {
min = Math.min(min, val);
max = Math.max(max, val);
}
});
if (min === Infinity) {
min = info.defaultMin;
max = info.defaultMax;
}
if (min === max) { min -= 1; max += 1; }
const pad = (max - min) * 0.1;
ranges[metric] = {
min: min - pad,
max: max + pad,
key: info.key,
label: info.label,
color: info.color
};
});
return ranges;
}

function getMetricStyle(key) {
const styles = {
temp: { color: '#ef4444', width: 2.5 },
hum: { color: '#3b82f6', width: 2.5 },
soil: { color: '#22c55e', width: 2.5 },
pressure: { color: '#a855f7', width: 2, dash: [5, 3] }
};
return styles[key] || { color: '#6b7280', width: 2 };
}

function drawGrid(P, cW, cH, ranges, W, H, activeMetrics) {
const ctx = chartCtx;
const gridColor = getComputedStyle(document.body).getPropertyValue('--border').trim();
const textColor = getComputedStyle(document.body).getPropertyValue('--text2').trim();
const textPrimary = getComputedStyle(document.body).getPropertyValue('--text').trim();
ctx.strokeStyle = gridColor;
ctx.lineWidth = 1;
ctx.font = '10px sans-serif';
ctx.fillStyle = textColor;
ctx.textAlign = 'right';
for (let i = 0; i <= 4; i++) {
const y = P.t + (cH / 4) * i;
ctx.beginPath();
ctx.moveTo(P.l, y);
ctx.lineTo(W - P.r, y);
ctx.stroke();
}
ctx.textAlign = 'center';
ctx.fillStyle = textColor;
const step = Math.max(1, Math.floor(chartData.length / 6));
for (let i = 0; i < chartData.length; i += step) {
const x = P.l + (cW / Math.max(1, chartData.length - 1)) * i;
const timeStr = chartData[i].time || '';
const timeParts = timeStr.split(' ');
const timeOnly = timeParts.length > 1 ? timeParts[1] : timeStr;
const [hours, minutes] = timeOnly.split(':');
const label = hours && minutes ? hours+':'+minutes : timeOnly;
ctx.fillText(label, x, P.t + cH + 18);
}
ctx.fillStyle = textPrimary;
ctx.fillText('Время', W / 2, H - 5);
ctx.strokeStyle = gridColor;
ctx.beginPath();
ctx.moveTo(P.l, P.t + cH);
ctx.lineTo(W - P.r, P.t + cH);
ctx.stroke();
}

function drawYAxisValues(P, cH, ranges, activeMetrics) {
const valuesContainer = document.getElementById('yAxisValues');
const labelEl = document.getElementById('yAxisLabel');
if (!valuesContainer || activeMetrics.length === 0) {
if (valuesContainer) valuesContainer.innerHTML = '';
return;
}
if (currentChartMode === 'all') {
valuesContainer.innerHTML = '';
labelEl.style.display = 'none';
return;
}
const firstMetric = activeMetrics[0];
const range = ranges[firstMetric];
let html = '';
for (let i = 0; i <= 4; i++) {
const val = Math.round(range.max - (range.max - range.min) * (i / 4));
html += '<div class="y-axis-value">'+val+'</div>';
}
valuesContainer.innerHTML = html;
}

function drawLine(data, key, range, P, cW, cH, style) {
const ctx = chartCtx;
ctx.strokeStyle = style.color;
ctx.lineWidth = style.width;
ctx.lineCap = 'round';
ctx.lineJoin = 'round';
if (style.dash) ctx.setLineDash(style.dash);
const points = [];
const n = Math.max(1, data.length - 1);
data.forEach((d, i) => {
const val = d[range.key];
if (val == null) return;
const x = P.l + (cW / n) * i;
const y = P.t + cH - ((val - range.min) / (range.max - range.min)) * cH;
points.push({ x, y, val, time: d.time });
});
if (points.length > 1) {
ctx.beginPath();
ctx.moveTo(points[0].x, points[0].y);
for (let i = 0; i < points.length - 1; i++) {
const p0 = points[Math.max(0, i - 1)];
const p1 = points[i];
const p2 = points[i + 1];
const p3 = points[Math.min(points.length - 1, i + 2)];
const cp1x = p1.x + (p2.x - p0.x) * 0.3 / 3;
const cp1y = p1.y + (p2.y - p0.y) * 0.3 / 3;
const cp2x = p2.x - (p3.x - p1.x) * 0.3 / 3;
const cp2y = p2.y - (p3.y - p1.y) * 0.3 / 3;
ctx.bezierCurveTo(cp1x, cp1y, cp2x, cp2y, p2.x, p2.y);
}
ctx.stroke();
}
ctx.setLineDash([]);
return points;
}

function drawEmptyChart() {
const ctx = chartCtx;
const rect = chartCanvas.getBoundingClientRect();
ctx.clearRect(0, 0, rect.width, rect.height);
ctx.fillStyle = getComputedStyle(document.body).getPropertyValue('--text2').trim();
ctx.font = '14px sans-serif';
ctx.textAlign = 'center';
ctx.fillText('Выберите период для отображения графика', rect.width / 2, rect.height / 2);
}

function handleChartHover(e) {
e.preventDefault();
if (!chartCanvas._lines || !chartCanvas._params) return;
const rect = chartCanvas.getBoundingClientRect();
const mx = (e.clientX || (e.touches && e.touches[0].clientX)) - rect.left;
const my = (e.clientY || (e.touches && e.touches[0].clientY)) - rect.top;
const { P } = chartCanvas._params;
let closest = null, minDist = 30;
Object.entries(chartCanvas._lines).forEach(([key, points]) => {
points.forEach(p => {
const d = Math.hypot(p.x - mx, p.y - my);
if (d < minDist) {
minDist = d;
closest = { ...p, key, color: getMetricStyle(key).color };
}
});
});
const tooltip = document.getElementById('tooltip');
if (closest && minDist < 30) {
let html = '<div class="tooltip-time">' + (closest.time || '') + '</div>';
const idx = chartData.findIndex(d => d.time === closest.time);
if (idx >= 0) {
const d = chartData[idx];
const metrics = [
{k:'temp', l:'Темп:', u:'°C', c:'#ef4444'},
{k:'hum', l:'Влаж:', u:'%', c:'#3b82f6'},
{k:'soil', l:'Почва:', u:'%', c:'#22c55e'},
{k:'pressure', l:'Давл:', u:'мм', c:'#a855f7'}
];
metrics.forEach(m => {
const show = currentChartMode === 'all' ? visibleMetrics[m.k] : (m.k === currentChartMode);
if (d[m.k] != null && show) {
html += '<div class="tooltip-row"><span><span class="tooltip-color" style="background:'+m.c+'"></span>'+m.l+'</span><strong>'+d[m.k]+m.u+'</strong></div>';
}
});
}
tooltip.innerHTML = html;
tooltip.classList.add('visible');
const tx = mx + 15 > rect.width - 220 ? mx - 230 : mx + 15;
const ty = Math.max(10, Math.min(rect.height - 150, my - 60));
tooltip.style.left = tx + 'px';
tooltip.style.top = ty + 'px';
chartCtx.save();
chartCtx.strokeStyle = closest.color;
chartCtx.lineWidth = 1;
chartCtx.setLineDash([4, 4]);
chartCtx.beginPath();
chartCtx.moveTo(closest.x, P.t);
chartCtx.lineTo(closest.x, P.t + chartCanvas._params.cH);
chartCtx.stroke();
chartCtx.restore();
} else {
tooltip.classList.remove('visible');
}
}

function hideTooltip() {
document.getElementById('tooltip').classList.remove('visible');
if (chartCanvas._params) drawChart();
}

function startAutoUpdate() {
setInterval(async () => {
await fetchMetrics();
if (document.getElementById('tab-main').classList.contains('active')) await loadHistory();
if (document.getElementById('tab-charts').classList.contains('active')) await loadChartData();
}, UPDATE_INTERVAL);
}
</script>
</body>
</html>
)rawliteral";

// Страница входа
const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Вход</title>
    <style>
        :root {
            --page-bg: #EEF0F4;
            --card-bg:  #0FB881;
            --brand-color:  #0FB881;
            --text-white: #111111;
            --text-on-green: #ffffff;
            --input-bg: #ffffff;
            --input-text: #333333;
            --btn-bg: #ffffff;
            --btn-text: #0FB881;
            --shadow: 0 12px 24px rgba(0, 0, 0, 0.12);
            --placeholder-color: #6CD3B2;
        }

        .theme-light {
            --page-bg: #0F182B;
            --card-bg: #21C85F;
            --brand-color: #21C85F;
            --text-white: #ffffff;
            --text-on-green: #ffffff;
            --input-bg: #ffffff;
            --input-text: #333333;
            --btn-bg: #ffffff;
            --btn-text: #21C85F;
            --shadow: 0 12px 24px rgba(0, 0, 0, 0.35);
            --placeholder-color: #77DD9D;
        }

        * { box-sizing: border-box; margin: 0; padding: 0; }

        body {
            font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--page-bg);
            color: var(--text-white);
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            transition: background-color 0.4s ease;
        }

        .theme-switcher {
            position: absolute;
            top: 32px;
            right: 32px;
            cursor: pointer;
            width: 60px;
            height: 60px;
            background-color: #F1F3F6;
            border-radius: 18px;
            display: flex;
            align-items: center;
            justify-content: center;
            transition: transform 0.2s, background-color 0.3s;
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.08);
        }

        .theme-switcher:hover {
            transform: scale(1.05);
        }

        .theme-switcher span {
            font-size: 28px;
            user-select: none;
        }

        body.theme-light .theme-switcher {
            background-color: #1B263B;
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.35);
        }

        .wrapper {
            width: 100%;
            padding: 20px;
            text-align: center;
            animation: fadeInUp 0.6s cubic-bezier(0.2, 0.8, 0.2, 1) forwards;
        }

        @keyframes fadeInUp {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }

        .auth-card {
            width: 90%;
            max-width: 500px;
            background-color: var(--card-bg);
            border-radius: 65px;
            padding: 40px 32px;
            box-shadow: var(--shadow);
            transition: background-color 0.4s ease;
            margin: 0 auto;
        }

        .auth-card h1 {
            font-size: 38px;
            font-weight: 700;
            margin-bottom: 24px;
            color: var(--text-on-green);
            letter-spacing: 0.5px;
        }

        .form-group {
            margin-bottom: 14px;
            position: relative;
        }

        .form-group input {
            width: 100%;
            padding: 14px 16px;
            border: none;
            border-radius: 12px;
            font-size: 18px;
            font-weight: 500;
            background: var(--input-bg);
            color: var(--input-text);
            transition: box-shadow 0.2s;
        }

        .form-group input::placeholder {
            color: var(--placeholder-color);
            opacity: 1;
            font-weight: 400;
        }

        .form-group input:focus {
            outline: none;
            box-shadow: 0 0 0 2px rgba(255, 255, 255, 0.6);
        }

        .toggle-password {
            position: absolute;
            right: 14px;
            top: 50%;
            transform: translateY(-50%);
            width: 32px;
            height: 32px;
            cursor: pointer;
            opacity: 0.85;
            transition: opacity 0.2s;
            font-size: 20px;
            background: none;
            border: none;
        }
        .toggle-password:hover { opacity: 1; }

        .register-link {
            margin-bottom: 18px;
            font-size: 16px;
            color: var(--text-on-green);
            opacity: 0.95;
        }

        .register-link a {
            color: var(--text-on-green);
            font-weight: 600;
            text-decoration: underline;
            text-underline-offset: 3px;
        }

        .btn {
            width: 60%;
            padding: 14px;
            background-color: var(--btn-bg);
            color: var(--btn-text);
            border: none;
            border-radius: 12px;
            font-size: 19px;
            font-weight: 700;
            cursor: pointer;
            transition: transform 0.15s, box-shadow 0.2s;
            letter-spacing: 0.5px;
        }

        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 14px rgba(0,0,0,0.15);
        }

        .btn:active { transform: scale(0.98); }

        @media (max-width: 480px) {
            .auth-card {
                width: 92%;
                padding: 24px 20px;
            }
        }
    </style>
</head>
<body class="theme-dark">
    <div class="theme-switcher" id="themeSwitcher">
        <span>◑</span>
    </div>
    <div class="wrapper">
        <div class="auth-card">
            <h1>Авторизация</h1>
            <form id="loginForm"> 
                <div class="form-group">
                    <input type="text" id="loginInput" name="login" placeholder="Логин" required autocomplete="username">
                </div>
                
                <div class="form-group">
                    <input type="password" id="passwordInput" name="password" placeholder="Пароль" required autocomplete="current-password">
                    <button type="button" class="toggle-password" id="togglePassword">👁</button>
                </div>

                <div class="register-link">
                    Нет аккаунта? <a href="/register">Зарегистрируйся!</a>
                </div>

                <button type="submit" class="btn">Вход</button>
            </form>
        </div>
    </div>

    <script>
        const savedTheme = localStorage.getItem('theme');
        if (savedTheme) {
            document.body.classList.remove('theme-dark', 'theme-light');
            document.body.classList.add(savedTheme);
        }

        document.getElementById('themeSwitcher').addEventListener('click', function() {
            const isDark = document.body.classList.contains('theme-dark');
            document.body.classList.remove('theme-dark', 'theme-light');
            document.body.classList.add(isDark ? 'theme-light' : 'theme-dark');
            localStorage.setItem('theme', isDark ? 'theme-light' : 'theme-dark');
        });

        const passwordInput = document.getElementById('passwordInput');
        const togglePassword = document.getElementById('togglePassword');

        togglePassword.addEventListener('click', function() {
            const isPassword = passwordInput.type === 'password';
            passwordInput.type = isPassword ? 'text' : 'password';
            this.textContent = isPassword ? '🙈' : '👁';
        });

        document.getElementById('loginForm').addEventListener('submit', function(e) {
            e.preventDefault();
            const login = document.getElementById('loginInput').value.trim();
            const password = document.getElementById('passwordInput').value.trim();
            
            // Отправка на сервер для проверки
            fetch('/api/login', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'login=' + encodeURIComponent(login) + '&password=' + encodeURIComponent(password)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    localStorage.setItem('auth_token', data.token);
                    window.location.href = '/profile';
                } else {
                    alert('Неверный логин или пароль');
                }
            })
            .catch(() => {
                // Если сервер недоступен, проверяем локально
                if (login === 'admin' && password === 'admin') {
                    localStorage.setItem('auth_token', 'local_admin');
                    window.location.href = '/profile';
                } else {
                    alert('Неверный логин или пароль');
                }
            });
        });
    </script>
</body>
</html>
)rawliteral";

// Страница регистрации
const char REGISTER_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Регистрация</title>
    <style>
        :root {
            --page-bg: #EEF0F4;
            --card-bg: #0FB881;
            --brand-color: #0FB881;
            --text-white: #111111;
            --text-on-green: #ffffff;
            --input-bg: #ffffff;
            --input-text: #333333;
            --btn-bg: #ffffff;
            --btn-text: #0FB881;
            --shadow: 0 12px 24px rgba(0, 0, 0, 0.12);
            --placeholder-color: #88DDB8;
            --label-color: #ffffff;
        }

        .theme-light {
            --page-bg: #0F182B;
            --card-bg: #21C85F;
            --brand-color: #21C85F;
            --text-white: #ffffff;
            --text-on-green: #ffffff;
            --input-bg: #ffffff;
            --input-text: #333333;
            --btn-bg: #ffffff;
            --btn-text: #21C85F;
            --shadow: 0 12px 24px rgba(0, 0, 0, 0.35);
            --placeholder-color: #77DD9D;
            --label-color: #ffffff;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--page-bg);
            color: var(--text-white);
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            transition: background-color 0.4s ease;
        }

        .theme-switcher {
            position: absolute;
            top: 32px;
            right: 32px;
            cursor: pointer;
            width: 60px;
            height: 60px;
            background-color: #F1F3F6;
            border-radius: 18px;
            display: flex;
            align-items: center;
            justify-content: center;
            transition: transform 0.2s, background-color 0.3s;
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.08);
        }

        .theme-switcher:hover {
            transform: scale(1.05);
        }

        .theme-switcher span {
            font-size: 28px;
            user-select: none;
        }

        body.theme-light .theme-switcher {
            background-color: #1B263B;
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.35);
        }

        .wrapper {
            width: 100%;
            padding: 20px;
            text-align: center;
            animation: fadeInUp 0.6s cubic-bezier(0.2, 0.8, 0.2, 1) forwards;
        }

        @keyframes fadeInUp {
            from {
                opacity: 0;
                transform: translateY(20px);
            }
            to {
                opacity: 1;
                transform: translateY(0);
            }
        }

        .auth-card {
            width: 90%;
            max-width: 500px;
            background-color: var(--card-bg);
            border-radius: 65px;
            padding: 32px 36px 36px;
            box-shadow: var(--shadow);
            transition: background-color 0.4s ease;
            margin: 0 auto;
        }

        .auth-card h1 {
            font-size: 38px;
            font-weight: 700;
            margin-bottom: 24px;
            color: var(--text-on-green);
            letter-spacing: 0.5px;
        }

        .form-group {
            margin-bottom: 14px;
            position: relative;
            text-align: left;
        }

        .field-label {
            display: block;
            font-size: 18px;
            font-weight: 600;
            color: var(--label-color);
            margin-bottom: 6px;
        }

        .required {
            color: #ff4757;
            margin-left: 2px;
        }

        .form-group input {
            width: 100%;
            padding: 12px 16px;
            border: none;
            border-radius: 10px;
            font-size: 16px;
            font-weight: 500;
            background: var(--input-bg);
            color: var(--input-text);
            transition: box-shadow 0.2s;
        }

        .form-group input::placeholder {
            color: var(--placeholder-color);
            opacity: 1;
            font-weight: 400;
        }

        .form-group input:focus {
            outline: none;
            box-shadow: 0 0 0 2px rgba(255, 255, 255, 0.6);
        }

        .btn {
            width: 70%;
            padding: 14px;
            margin-top: 10px;
            background-color: var(--btn-bg);
            color: var(--btn-text);
            border: none;
            border-radius: 14px;
            font-size: 20px;
            font-weight: 700;
            cursor: pointer;
            transition: transform 0.15s, box-shadow 0.2s;
            letter-spacing: 0.5px;
        }

        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 14px rgba(0,0,0,0.15);
        }

        .btn:active {
            transform: scale(0.98);
        }

        .register-link {
            margin-top: 16px;
            font-size: 17px;
            color: var(--text-on-green);
            opacity: 0.95;
        }

        .register-link a {
            color: var(--text-on-green);
            font-weight: 600;
            text-decoration: underline;
            text-underline-offset: 3px;
        }

        .warning-icon {
            display: none;
            color: #ff4757;
            font-size: 14px;
            margin-top: 4px;
        }

        .warning-icon.show {
            display: block;
        }

        @media (max-width: 480px) {
            .auth-card {
                width: 92%;
                padding: 24px 20px;
            }
            .auth-card h1 {
                font-size: 28px;
            }
            .btn {
                width: 85%;
            }
        }
    </style>
</head>
<body class="theme-dark">
    <div class="theme-switcher" id="themeSwitcher">
        <span>◑</span>
    </div>

    <div class="wrapper">
        <div class="auth-card">
            <h1>Регистрация</h1>

            <form id="registerForm" onsubmit="return false;">
                
                <div class="form-group">
                    <label class="field-label">Логин<span class="required">*</span></label>
                    <input type="text" name="login" id="loginInput" placeholder="от 3 до 50 символов" required minlength="3" maxlength="50">
                </div>

                <div class="form-group">
                    <label class="field-label">Пароль<span class="required">*</span></label>
                    <input type="password" name="password" id="passwordInput" placeholder="от 6 символов" required minlength="6">
                </div>

                <div class="form-group">
                    <label class="field-label">Повторите пароль<span class="required">*</span></label>
                    <input type="password" name="password_confirm" id="passwordConfirmInput" placeholder="" required>
                    <div class="warning-icon" id="passwordWarning">⚠ Пароли не совпадают</div>
                </div>

                <div class="form-group">
                    <label class="field-label">Почта<span class="required">*</span></label>
                    <input type="email" name="email" id="emailInput" placeholder="your@email.com" required>
                </div>
                
                <button type="submit" class="btn">Зарегистрироваться</button>
                
                <div class="register-link">
                    Есть аккаунт? <a href="/login">Заходи!</a>
                </div>
            </form>
        </div>
    </div>

    <script>
        const savedTheme = localStorage.getItem('theme');
        if (savedTheme) {
            document.body.classList.remove('theme-dark', 'theme-light');
            document.body.classList.add(savedTheme);
        }
        
        document.getElementById('themeSwitcher').addEventListener('click', function() {
            const isDark = document.body.classList.contains('theme-dark');
            document.body.classList.remove('theme-dark', 'theme-light');
            document.body.classList.add(isDark ? 'theme-light' : 'theme-dark');
            localStorage.setItem('theme', isDark ? 'theme-light' : 'theme-dark');
        });

        const passwordInput = document.getElementById('passwordInput');
        const passwordConfirmInput = document.getElementById('passwordConfirmInput');
        const passwordWarning = document.getElementById('passwordWarning');

        function checkPasswordsMatch() {
            const password = passwordInput.value;
            const confirmPassword = passwordConfirmInput.value;
            
            if (confirmPassword.length > 0 && password !== confirmPassword) {
                passwordWarning.classList.add('show');
            } else {
                passwordWarning.classList.remove('show');
            }
        }

        passwordInput.addEventListener('input', checkPasswordsMatch);
        passwordConfirmInput.addEventListener('input', checkPasswordsMatch);

        document.getElementById('registerForm').addEventListener('submit', function(e) {
            e.preventDefault();
            
            const login = document.getElementById('loginInput').value.trim();
            const pass = document.getElementById('passwordInput').value.trim();
            const passRepeat = document.getElementById('passwordConfirmInput').value.trim();
            const email = document.getElementById('emailInput').value.trim();

            if (pass !== passRepeat) {
                alert('Пароли не совпадают!');
                return;
            }
            if (pass.length < 4) {
                alert('Пароль слишком короткий (мин. 4 символа)');
                return;
            }

            // Отправка на сервер
            fetch('/api/register', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'login=' + encodeURIComponent(login) + 
                      '&password=' + encodeURIComponent(pass) + 
                      '&email=' + encodeURIComponent(email)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    localStorage.setItem('auth_token', data.token);
                    window.location.href = '/profile';
                } else {
                    alert(data.message || 'Ошибка регистрации');
                }
            })
            .catch(() => {
                // Локальная регистрация если сервер недоступен
                let users = JSON.parse(localStorage.getItem('users') || '{}');
                if (users[login]) {
                    alert('Такой логин уже занят');
                    return;
                }
                users[login] = { pass: pass, email: email };
                localStorage.setItem('users', JSON.stringify(users));
                localStorage.setItem('auth_token', 'local_' + login);
                window.location.href = '/profile';
            });
        });
    </script>
</body>
</html>
)rawliteral";

const char EDIT_PROFILE_HTML[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Редактирование профиля</title>
    <style>
        :root {
            /* ТЁМНАЯ ТЕМА */
            --page-bg: #0F182B;
            --text-main: #ffffff;
            --accent-green: #21C85F;
            --card-bg: rgba(255, 255, 255, 0.95);
            --card-text: #333333;
            --glow-color: rgba(33, 200, 95, 0.6);
            --glow-soft: rgba(33, 200, 95, 0.3);
            --user-pill-bg: #1a2d45;
            --user-pill-text: #a0aab5;
            --nav-bg: #21C85F;
            --nav-btn-bg: rgba(255, 255, 255, 0.25);
            --label-bg: rgba(33, 200, 95, 0.15);
            --label-text: #21C85F;
            --footer-bg: #21C85F;
            --footer-text: #ffffff;
            --footer-column-title: #ffffff;
            --footer-column-text: #ffffff;
            --footer-copy: #ffffff;
            --switcher-bg: #1a2d45;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.35);
        }

        .theme-light {
            /* СВЕТЛАЯ ТЕМА */
            --page-bg: #E8F0F2;
            --text-main: #333333;
            --accent-green: #10B981;
            --card-bg: #ffffff;
            --card-text: #333333;
            --glow-color: rgba(16, 185, 129, 0.4);
            --glow-soft: rgba(16, 185, 129, 0.2);
            --user-pill-bg: #ffffff;
            --user-pill-text: #8899aa;
            --nav-bg: #10B981;
            --nav-btn-bg: rgba(255, 255, 255, 0.3);
            --label-bg: rgba(16, 185, 129, 0.15);
            --label-text: #10B981;
            --footer-bg: #10B981;
            --footer-text: #ffffff;
            --footer-column-title: #ffffff;
            --footer-column-text: #ffffff;
            --footer-copy: #ffffff;
            --switcher-bg: #ffffff;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.1);
        }

        *, *::before, *::after {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--page-bg);
            color: var(--text-main);
            transition: background-color 0.4s ease, color 0.4s ease;
            min-height: 100vh;
            overflow-x: hidden;
            display: flex;
            flex-direction: column;
        }

        .user-avatar {
            width: 32px;
            height: 32px;
            border-radius: 50%;
            object-fit: cover;
            display: none;
        }
        body.theme-dark .avatar-dark { display: block; }
        body.theme-light .avatar-light { display: block; }

        .logo-container {
            position: absolute;
            left: 50%;
            transform: translateX(-50%);
            display: flex;
            align-items: center;
        }
        .main-logo {
            height: 100px;
            display: none;
            transition: height 0.3s ease;
        }
        body.theme-dark .logo-dark { display: block; }
        body.theme-light .logo-light { display: block; }

        .user-pill {
            width: 145px; 
        }

        .mode-toggle {
            position: absolute;
            opacity: 0;
            width: 0;
            height: 0;
            margin: 0;
            padding: 0;
            pointer-events: none;
        }

        .theme-switcher label {
            display: flex;
            align-items: center;
            justify-content: center;
            width: 100%;
            height: 100%;
            cursor: pointer;
        }

        .theme-icon-img {
            display: block; 
            margin: 0 auto;
            width: 24px;
            height: 24px;
            filter: invert(1);
        }
        .theme-light .theme-icon-img { filter: invert(0); }

        .top-sticky-wrapper {
            position: sticky;
            top: 0;
            z-index: 1000;
            width: 100%;
            background: transparent;
            pointer-events: none;
        }
        .top-sticky-wrapper > * { pointer-events: auto; }

        .site-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 20px 40px;
            background: transparent;
            backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px);
            pointer-events: auto;
            position: relative;
            z-index: 2;
        }

        .user-pill {
            display: flex;
            align-items: center;
            text-decoration: none;
            background: var(--user-pill-bg);
            border-radius: 50px;
            padding: 8px 16px 8px 8px;
            gap: 10px;
            width: auto;
            height: 44px;
            box-shadow: 0 0 8px var(--glow-soft);
            animation: userPulse 3s infinite alternate;
            flex-shrink: 0;
        }
        @keyframes userPulse {
            0% { box-shadow: 0 0 6px var(--glow-soft); }
            100% { box-shadow: 0 0 16px var(--glow-color); }
        }
        .user-avatar { width: 32px; height: 32px; border-radius: 50%; object-fit: cover; }
        .user-text { font-size: 14px; font-weight: 400; color: var(--user-pill-text); letter-spacing: 0.5px; }

        .logo-container {
            position: absolute;
            left: 50%;
            transform: translateX(-50%);
            display: flex;
            align-items: center;
        }

        .theme-switcher {
            width: 48px; height: 48px; background: var(--switcher-bg); border-radius: 14px;
            display: flex; align-items: center; justify-content: center; cursor: pointer;
            box-shadow: 0 0 10px var(--glow-soft); animation: switcherPulse 3s infinite alternate; flex-shrink: 0;
        }
        @keyframes switcherPulse {
            0% { box-shadow: 0 0 8px var(--glow-soft); }
            100% { box-shadow: 0 0 18px var(--glow-color); }
        }
        .theme-icon-img { width: 24px; height: 24px; filter: invert(1); }
        .theme-light .theme-icon-img { filter: invert(0); }

        .navigation {
            display: flex; justify-content: center; align-items: center; gap: 8px;
            background: var(--nav-bg); border-radius: 40px; padding: 6px;
            margin: 0px 30px 35px 30px; height: 52px; pointer-events: auto;
            box-shadow: 0 6px 18px rgba(0, 0, 0, 0.3);
        }
        .nav-btn {
            color: #ffffff; text-decoration: none; font-size: 18px; font-weight: 600;
            padding: 8px 22px; border-radius: 30px; transition: all 0.3s ease;
            white-space: nowrap; position: relative; background: rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(4px); -webkit-backdrop-filter: blur(4px);
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        .nav-btn:hover, .nav-btn.active {
            background: rgba(255, 255, 255, 0.35); backdrop-filter: blur(10px);
            -webkit-backdrop-filter: blur(10px); border: 1px solid rgba(255, 255, 255, 0.25);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }

        .page-wrapper { flex: 1; padding: 0 20px 50px 20px; display: flex; justify-content: center; }

        .card {
            background: var(--card-bg); width: 100%; max-width: 840px; border-radius: 22px;
            padding: 36px 48px; box-shadow: 0 0 24px var(--glow-color), var(--card-internal-shadow);
            position: relative; color: var(--card-text);
        }

        .card-header {
            text-align: left; font-size: 32px; font-weight: 700; color: var(--accent-green); margin-bottom: 24px;
        }

        .logout-icon {
            position: absolute;
            top: 28px;
            right: 36px;
            cursor: pointer;
            opacity: 0.7;
            transition: opacity 0.2s ease;
            width: 26px;
            height: 26px;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .logout-icon:hover { opacity: 1; }

        .logout-img {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: block;
            pointer-events: none;
            position: absolute;
            top: 0;
            left: 0;
            transition: opacity 0.2s ease;
            opacity: 0;
        }

        body.theme-dark .logout-dark-default { opacity: 1; }
        body.theme-dark .logout-icon:hover .logout-dark-default { opacity: 0; }
        body.theme-dark .logout-icon:hover .logout-dark-hover { opacity: 1; }

        body.theme-light .logout-light-default { opacity: 1; }
        body.theme-light .logout-icon:hover .logout-light-default { opacity: 0; }
        body.theme-light .logout-icon:hover .logout-light-hover { opacity: 1; }

        .field-icon {
            position: absolute;
            right: 12px;
            top: 50%;
            transform: translateY(-50%);
            width: 18px;
            height: 18px;
            opacity: 0; 
            transition: opacity 0.2s ease;
            pointer-events: none;
        }

        .input-wrapper {
            position: relative;
        }

        .profile-layout {
            display: grid; grid-template-columns: 150px 1fr; gap: 32px 40px; align-items: start; margin-bottom: 32px;
        }
        .avatar { width: 150px; height: 150px; border-radius: 18px; object-fit: cover; }
        .timezone-section { margin-top: 14px; }

        .gender-label {
            font-size: 18px; font-weight: 700; color: var(--accent-green); display: flex; align-items: center; gap: 6px; margin-bottom: 10px;
        }
        .timezone-label {
            font-size: 22px; font-weight: 700; color: var(--accent-green); display: flex; align-items: center; gap: 6px; margin-bottom: 10px;
        }
        .lock-icon { font-size: 13px; opacity: 0.45; }

        .timezone-box {
            display: inline-block; border: 1.5px solid rgba(128, 128, 128, 0.3); border-radius: 22px;
            padding: 10px 22px; font-size: 16.5px; color: #888; white-space: nowrap; background: rgba(0,0,0,0.03);
        }
        .profile-info { display: flex; flex-direction: column; gap: 4px; }
        .profile-name { font-size: 20px; font-weight: 700; color: var(--accent-green); }
        .profile-email { font-size: 16px; color: var(--accent-green); opacity: 0.7; margin-bottom: 15px; }

        .gender-options { display: flex; gap: 18px; }
        .gender-opt {
            display: flex; align-items: center; gap: 9px; padding: 7px 20px;
            border: 1.5px solid rgba(128, 128, 128, 0.3); border-radius: 22px; cursor: pointer;
            font-size: 15px; color: var(--card-text); background: transparent;
            transition: all 0.3s ease;
        }
        .gender-opt.selected {
            border-color: var(--accent-green);
            background: #ffffff;
            box-shadow: 0 0 15px var(--glow-soft);
            color: var(--accent-green);
        }

        .radio-dot {
            width: 16px; height: 16px; border: 2px solid rgba(128, 128, 128, 0.4);
            border-radius: 50%; position: relative;
        }
        .gender-opt.selected .radio-dot { border-color: var(--accent-green); }
        .gender-opt.selected .radio-dot::after {
            content: ''; position: absolute; top: 2px; left: 2px; width: 8px; height: 8px;
            background: var(--accent-green); border-radius: 50%;
        }

        .card-actions { display: flex; justify-content: center; gap: 36px; margin-top: 20px; }

        .btn {
            padding: 11px 36px; border-radius: 22px; font-size: 16px; font-weight: 600;
            cursor: pointer; border: none; transition: all 0.3s ease;
        }

        .btn-edit {
            background: var(--accent-green);
            color: #ffffff;
            border: 1.5px solid var(--accent-green);
            box-shadow: 0 0 15px var(--glow-soft);
        }
        .btn-edit:hover {
            transform: scale(1.03);
            box-shadow: 0 0 25px var(--glow-color), 0 0 10px var(--glow-color);
            filter: brightness(1.1);
        }

        .btn-delete {
            background: #ffffff;
            color: #e55a5a;
            border: 1.5px solid #ffb3b3;
            box-shadow: 0 0 15px rgba(229, 90, 90, 0.25);
        }
        .btn-delete:hover {
            transform: scale(1.03);
            background: #fff5f5;
            color: #d32f2f;
            border-color: #ff8a8a;
            box-shadow: 0 0 25px rgba(229, 90, 90, 0.6), 0 0 10px rgba(229, 90, 90, 0.4);
        }

        .site-footer {
            background: var(--footer-bg); color: var(--footer-text); padding: 36px 40px 0;
            transition: background 0.4s ease; margin-top: auto;
        }
        .footer-columns {
            display: grid; grid-template-columns: 1.3fr 1fr 0.8fr; gap: 30px;
            max-width: 900px; margin: 0 auto; padding-bottom: 24px; border-bottom: 1px solid rgba(255,255,255,0.2);
        }
        .footer-column-title { font-size: 20px; font-weight: 700; margin-bottom: 12px; color: var(--footer-column-title); }
        .footer-column-text { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-column-list { list-style: none; padding: 0; }
        .footer-column-list li { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-copyright { text-align: center; padding: 16px 0 12px; font-size: 14px; color: var(--footer-copy); max-width: 900px; margin: 0 auto; }

        .modal-overlay {
            position: fixed;
            top: 0; left: 0; width: 100%; height: 100%;
            background: rgba(0, 0, 0, 0.4);
            backdrop-filter: blur(5px);
            -webkit-backdrop-filter: blur(5px);
            display: flex; align-items: center; justify-content: center;
            z-index: 2000;
            opacity: 0; visibility: hidden; transition: all 0.3s ease;
        }
        .modal-overlay.active { opacity: 1; visibility: visible; }

        .avatar-modal {
            background: var(--card-bg);
            padding: 24px 32px;
            border-radius: 24px;
            box-shadow: 0 10px 40px rgba(0, 0, 0, 0.2);
            text-align: center;
            position: relative;
            max-width: 320px;
            width: 90%;
            transition: transform 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275);
            transform: scale(0.9);
        }
        .modal-overlay.active .avatar-modal { transform: scale(1); }

        .modal-close-btn {
            position: absolute; top: 12px; right: 12px;
            background: var(--accent-green); color: white;
            border: none; border-radius: 50%; width: 24px; height: 24px;
            font-size: 16px; line-height: 24px; cursor: pointer;
            display: flex; align-items: center; justify-content: center;
        }
        .modal-title { font-size: 20px; font-weight: 700; color: var(--accent-green); margin-bottom: 16px; }

        .avatar-preview-area { display: none; margin-bottom: 16px; }
        .modal-overlay.has-preview .avatar-preview-area { display: block; }
        .modal-overlay.has-preview .modal-select-link { display: none; }
        .modal-overlay.has-preview .modal-save-btn { display: inline-block; }

        .modal-preview-img {
            width: 120px; height: 120px; object-fit: cover;
            border-radius: 18px; margin-bottom: 8px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.1);
        }
        .preview-label { font-size: 13px; color: #888; display: block; margin-bottom: 12px; }

        .modal-select-link {
            font-size: 18px; font-weight: 700; color: var(--accent-green);
            text-decoration: underline; cursor: pointer; display: block; margin-bottom: 10px;
        }
        .modal-select-link:hover { color: #1a9e4a; }

        .modal-save-btn {
            display: none;
            background: var(--accent-green); color: white;
            border: none; padding: 10px 32px; border-radius: 22px;
            font-size: 16px; font-weight: 600; cursor: pointer;
            box-shadow: 0 4px 15px var(--glow-soft);
            transition: all 0.2s ease;
        }
        .modal-save-btn:hover { transform: scale(1.03); filter: brightness(1.1); }

        #hidden-file-input { display: none; }

        @media (max-width: 768px) {
            .site-header { padding: 12px 16px; flex-wrap: wrap; gap: 12px; }
            .logo-container { position: relative !important; left: auto !important; transform: none !important; order: -1; width: 100%; justify-content: center; margin-bottom: 0; }
            .user-pill { width: auto; padding: 6px 12px 6px 6px; height: 38px; order: 1; margin-right: auto; animation: none; }
            .user-avatar { width: 26px; height: 26px; }
            .theme-switcher { position: relative !important; top: auto !important; right: auto !important; width: 42px; height: 42px; order: 2; animation: none; }
            .navigation { order: 3; margin: 8px 12px 20px 12px; flex-wrap: wrap; height: auto; gap: 6px; padding: 6px; }
            .nav-btn { padding: 8px 16px; font-size: 15px; }
            .page-wrapper { padding: 0 12px 40px 12px; }
            .card { padding: 24px 20px; }
            .profile-layout { grid-template-columns: 1fr; }
            .avatar { width: 130px; height: 130px; margin: 0 auto; display: block; }
            .timezone-box { white-space: normal; width: 100%; text-align: center; }
            .gender-options { justify-content: center; }
            .card-actions { flex-direction: column; align-items: center; gap: 16px; }
            .footer-columns { grid-template-columns: 1fr; text-align: center; }
            .logout-icon {
            top: 20px;
            right: 20px;
            width: 22px;
            height: 22px;
        }
        }
    </style>
    <style>
        .edit-profile-header {
            display: flex;
            align-items: flex-start;
            gap: 40px;
            margin-bottom: 30px;
        }
        
        .edit-avatar-section {
            display: flex;
            flex-direction: column;
            align-items: center;
            gap: 10px;
        }
        
        .edit-avatar-section .avatar {
            width: 150px;
            height: 150px;
            border-radius: 18px;
            object-fit: cover;
        }
        
        .edit-avatar-section .btn-edit {
            padding: 8px 20px;
            font-size: 14px;
        }
        
        .edit-avatar-section .upload-limit {
            font-size: 12px;
            color: #888;
            text-align: center;
        }
        
        .edit-user-info {
            flex: 1;
            display: flex;
            flex-direction: column;
            gap: 15px;
        }
        
        .edit-field-row {
            display: flex;
            align-items: center;
            gap: 10px;
        }
        
        .edit-field-row input {
            flex: 1;
            padding: 12px 45px 12px 18px;
            border: 1.5px solid rgba(128, 128, 128, 0.3);
            border-radius: 22px;
            font-size: 16px;
            background: rgba(0,0,0,0.03);
            outline: none;
        }
        
        .edit-field-row input:focus {
            border-color: var(--accent-green);
        }
        
        .edit-field-row .field-icon {
            position: static;
            opacity: 0.6;
            width: 20px;
            height: 20px;
            cursor: pointer;
            flex-shrink: 0;
        }
        
        .edit-section {
            margin-bottom: 25px;
        }
        
        .edit-section-title {
            font-size: 20px;
            font-weight: 700;
            color: var(--accent-green);
            margin-bottom: 15px;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .edit-section-title .lock-icon {
            font-size: 14px;
            opacity: 0.5;
        }
        
        .edit-password-inputs {
            display: flex;
            flex-direction: column;
            gap: 12px;
        }
        
        .password-input-wrapper {
            position: relative;
        }
        
        .password-input-wrapper input {
            width: 100%;
            padding: 12px 45px 12px 18px;
            border: 1.5px solid rgba(128, 128, 0, 0.3);
            border-radius: 22px;
            font-size: 16px;
            background: rgba(0,0,0,0.03);
            outline: none;
        }
        
        .password-input-wrapper input:focus {
            border-color: var(--accent-green);
        }
        
        .password-input-wrapper .warning-icon {
            position: absolute;
            right: 15px;
            top: 50%;
        }
        
        .edit-gender-row {
            display: flex;
            align-items: center;
            gap: 15px;
        }
        
        .edit-gender-row .gender-label {
            font-size: 18px;
            font-weight: 700;
            color: var(--accent-green);
            display: flex;
            align-items: center;
            gap: 6px;
        }
        
        .edit-timezone-row {
            display: flex;
            align-items: center;
            gap: 15px;
        }
        
        .edit-timezone-row .timezone-label {
            font-size: 20px;
            font-weight: 700;
            color: var(--accent-green);
            display: flex;
            align-items: center;
            gap: 6px;
        }
        
        .edit-timezone-row select {
            flex: 1;
            max-width: 350px;
            padding: 12px 45px 12px 18px;
            border: 1.5px solid rgba(128, 128, 0, 0.3);
            border-radius: 22px;
            font-size: 16px;
            background: rgba(0,0,0,0.03);
            outline: none;
            cursor: pointer;
        }
        
        .edit-timezone-row select:focus {
            border-color: var(--accent-green);
        }
        
        @media (max-width: 768px) {
            .edit-profile-header {
                flex-direction: column;
                align-items: center;
            }
            
            .edit-field-row,
            .edit-gender-row,
            .edit-timezone-row {
                flex-direction: column;
                align-items: stretch;
            }
            
            .edit-timezone-row select {
                max-width: 100%;
            }
        }
    </style>
</head>
<body class="theme-light">
    <div class="top-sticky-wrapper">
        <header class="site-header">
            <div class="user-pill">
                <img src="Т.пол.png" alt="User" class="user-avatar avatar-dark" id="header-avatar-dark">
                <img src="Св.пол.png" alt="User" class="user-avatar avatar-light" id="header-avatar-light">
                <span class="user-text" id="header-username">User_login</span>
            </div>
            
            <div class="logo-container">
                <img src="Светлая.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="Тёмная.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>
            
            <div class="theme-switcher" onclick="toggleTheme()">
                <input type="checkbox" id="theme-toggle" class="mode-toggle">
                <label for="theme-toggle">
                    <img src="тема.png" alt="Theme" class="theme-icon-img">
                </label>
            </div>
        </header>
        
        <nav class="navigation">
            <a href="#" class="nav-btn">Главная</a>
            <a href="#" class="nav-btn">Мои растения</a>
            <a href="#" class="nav-btn">Настройки</a>
        </nav>
    </div>

    <div class="page-wrapper">
        <div class="card">
            <h1 class="card-header">Редактирование профиля</h1>
            <div class="edit-profile-header">
                <div class="edit-avatar-section">
                    <img src="https://i.imgur.com/8KxHqJp.jpg" alt="Avatar" class="avatar" id="profile-avatar">
                    <button class="btn btn-edit" onclick="openAvatarModal()">Изменить фото</button>
                    <div class="upload-limit">Изображение до 1МБ</div>
                </div>
                <div class="edit-user-info">
                    <div class="edit-field-row">
                        <input type="text" id="edit-username" value="User_login" placeholder="Имя пользователя">
                    </div>
                    <div class="edit-field-row">
                        <input type="email" id="edit-email" value="name_mail@email.com" placeholder="Email">
                    </div>
                    <div class="edit-gender-row" style="margin-top: 10px;">
                        <div class="gender-label">
                            Пол
                        </div>
                        <div class="gender-options">
                            <div class="gender-opt selected" id="edit-gender-female" onclick="selectGender('female')">
                                <span class="radio-dot"></span>
                                <span>Женский</span>
                            </div>
                            <div class="gender-opt" id="edit-gender-male" onclick="selectGender('male')">
                                <span class="radio-dot"></span>
                                <span>Мужской</span>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
            <div class="edit-section">
                <div class="edit-section-title">Сменить пароль</div>
                <div class="edit-password-inputs">
                    <div class="password-input-wrapper">
                        <input type="password" id="new-password" placeholder="Новый пароль">
                        <img src="warning-icon.png" alt="Warning" class="warning-icon" id="pass-warn-1" style="display:none;">
                    </div>
                    <div class="password-input-wrapper">
                        <input type="password" id="confirm-password" placeholder="Подтверждение нового пароль">
                        <img src="warning-icon.png" alt="Warning" class="warning-icon" id="pass-warn-2" style="display:none;">
                    </div>
                </div>
            </div>
            <div class="edit-section">
                <div class="edit-timezone-row">
                    <div class="timezone-label">
                        Часовой пояс
                    </div>
                    <select id="edit-timezone">
                        <option value="(UTC+05:00) Asian/Yekaterinburg" selected>(UTC+05:00) Asian/Yekaterinburg</option>
                        <option value="(UTC+03:00) Europe/Moscow">(UTC+03:00) Europe/Moscow</option>
                        <option value="(UTC+07:00) Asia/Novosibirsk">(UTC+07:00) Asia/Novosibirsk</option>
                    </select>
                </div>
            </div>
            <div class="card-actions" style="margin-top: 35px;">
                <button class="btn btn-edit" onclick="saveChanges()">Сохранить изменения</button>
                <button class="btn btn-delete" onclick="window.location.href='/profile'">Отмена</button>
            </div>
        </div>
    </div>

    <div class="modal-overlay" id="avatar-modal">
        <div class="avatar-modal">
            <button class="modal-close-btn" onclick="closeAvatarModal()">×</button>
            <div class="modal-title">Загрузить изображение</div>
            <div class="avatar-preview-area" id="preview-area">
                <img src="" alt="Preview" class="modal-preview-img" id="modal-preview-img">
                <span class="preview-label">Предпросмотр</span>
            </div>
            <input type="file" id="hidden-file-input" accept="image/*">
            <div class="modal-select-link" onclick="document.getElementById('hidden-file-input').click()">Выбрать фото</div>
            <button class="modal-save-btn" id="modal-save-btn" onclick="confirmAvatarUpload()">Сохранить</button>
        </div>
    </div>

    <footer class="site-footer">
        <div class="footer-columns">
            <div class="footer-column">
                <div class="footer-column-title">Зелёная полка</div>
                <div class="footer-column-text">
                    Система контроля микроклимата растений соответствует приоритетам техлидерства РФ, IoT и импортозамещению.
                </div>
            </div>
            <div class="footer-column">
                <div class="footer-column-title">Модули</div>
                <ul class="footer-column-list">
                    <li>Освещения растений</li>
                    <li>Сбор данных о растениях</li>
                    <li>Полив растений</li>
                </ul>
            </div>
            <div class="footer-column">
                <div class="footer-column-title">Контакты</div>
                <ul class="footer-column-list">
                    <li>Почта</li>
                    <li>Наши сети</li>
                </ul>
            </div>
        </div>
        <div class="footer-copyright">2026 Зелёная полка</div>
    </footer>

    <script>
        let tempAvatarData = null;

        function toggleTheme() {
            document.body.classList.toggle('theme-light');
            document.body.classList.toggle('theme-dark');
            const theme = document.body.classList.contains('theme-dark') ? 'dark' : 'light';
            localStorage.setItem('theme', theme);
        }
        function loadTheme() {
            const savedTheme = localStorage.getItem('theme') || 'light';
            if (savedTheme === 'dark') {
                document.body.classList.remove('theme-light');
                document.body.classList.add('theme-dark');
            }
        }
        loadTheme();

        function selectGender(g) {
            document.getElementById('edit-gender-female').classList.toggle('selected', g === 'female');
            document.getElementById('edit-gender-male').classList.toggle('selected', g === 'male');
        }

        document.addEventListener('DOMContentLoaded', function() {
            const defaultData = {
                username: 'User_login',
                email: 'name_mail@email.com',
                avatar: 'https://i.imgur.com/8KxHqJp.jpg',
                gender: 'female',
                timezone: '(UTC+05:00) Asian/Yekaterinburg'
            };
            const userData = JSON.parse(localStorage.getItem('userData')) || defaultData;

            document.getElementById('header-avatar-dark').src = userData.avatar;
            document.getElementById('header-avatar-light').src = userData.avatar;
            document.getElementById('header-username').textContent = userData.username;

            document.getElementById('profile-avatar').src = userData.avatar;
            document.getElementById('edit-username').value = userData.username;
            document.getElementById('edit-email').value = userData.email;
            document.getElementById('edit-timezone').value = userData.timezone;
            selectGender(userData.gender);
        });

        function openAvatarModal() {
            document.getElementById('avatar-modal').classList.add('active');
            document.getElementById('avatar-modal').classList.remove('has-preview');
            document.getElementById('hidden-file-input').value = '';
            tempAvatarData = null;
        }
        function closeAvatarModal() {
            document.getElementById('avatar-modal').classList.remove('active');
        }

        document.getElementById('hidden-file-input').addEventListener('change', function(e) {
            const file = e.target.files[0];
            if (!file) return;
            if (!file.type.startsWith('image/')) { alert('Выберите изображение'); return; }
            if (file.size > 1048576) { alert('Размер не должен превышать 1МБ'); return; }
            
            const reader = new FileReader();
            reader.onload = function(ev) {
                tempAvatarData = ev.target.result;
                document.getElementById('modal-preview-img').src = tempAvatarData;
                document.getElementById('avatar-modal').classList.add('has-preview');
            };
            reader.readAsDataURL(file);
        });

        function confirmAvatarUpload() {
            if (tempAvatarData) {
                document.getElementById('profile-avatar').src = tempAvatarData;
                document.getElementById('header-avatar-dark').src = tempAvatarData;
                document.getElementById('header-avatar-light').src = tempAvatarData;
                closeAvatarModal();
            }
        }

        function saveChanges() {
            const username = document.getElementById('edit-username').value.trim();
            const email = document.getElementById('edit-email').value.trim();
            const pass = document.getElementById('new-password').value;
            const conf = document.getElementById('confirm-password').value;
            const timezone = document.getElementById('edit-timezone').value;
            const gender = document.getElementById('edit-gender-female').classList.contains('selected') ? 'female' : 'male';
            const avatar = tempAvatarData || document.getElementById('profile-avatar').src;

            if (!username) { alert('Введите имя'); return; }
            if (!email.includes('@')) { alert('Введите корректный email'); return; }
            if (pass && pass.length < 6) {
                alert('Пароль минимум 6 символов');
                document.getElementById('pass-warn-1').style.display = 'block';
                return;
            }
            if (pass && pass !== conf) {
                alert('Пароли не совпадают');
                document.getElementById('pass-warn-2').style.display = 'block';
                return;
            }

            localStorage.setItem('userData', JSON.stringify({ username, email, avatar, gender, timezone }));
            alert('Сохранено!');
            window.location.href = '/profile';
        }
    </script>
</body>
</html>
)rawliteral"; 

// Страница профиля
const char PROFILE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Профиль</title>
    <style>
        :root {
            /* ТЁМНАЯ ТЕМА */
            --page-bg: #0F182B;
            --text-main: #ffffff;
            --accent-green: #21C85F;
            --card-bg: rgba(255, 255, 255, 0.95);
            --card-text: #333333;
            --glow-color: rgba(33, 200, 95, 0.6);
            --glow-soft: rgba(33, 200, 95, 0.3);
            --user-pill-bg: #1a2d45;
            --user-pill-text: #a0aab5;
            --nav-bg: #21C85F;
            --nav-btn-bg: rgba(255, 255, 255, 0.25);
            --label-bg: rgba(33, 200, 95, 0.15);
            --label-text: #21C85F;
            --footer-bg: #21C85F;
            --footer-text: #ffffff;
            --footer-column-title: #ffffff;
            --footer-column-text: #ffffff;
            --footer-copy: #ffffff;
            --switcher-bg: #1a2d45;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.35);
        }

        .theme-light {
            /* СВЕТЛАЯ ТЕМА */
            --page-bg: #E8F0F2;
            --text-main: #333333;
            --accent-green: #10B981;
            --card-bg: #ffffff;
            --card-text: #333333;
            --glow-color: rgba(16, 185, 129, 0.4);
            --glow-soft: rgba(16, 185, 129, 0.2);
            --user-pill-bg: #ffffff;
            --user-pill-text: #8899aa;
            --nav-bg: #10B981;
            --nav-btn-bg: rgba(255, 255, 255, 0.3);
            --label-bg: rgba(16, 185, 129, 0.15);
            --label-text: #10B981;
            --footer-bg: #10B981;
            --footer-text: #ffffff;
            --footer-column-title: #ffffff;
            --footer-column-text: #ffffff;
            --footer-copy: #ffffff;
            --switcher-bg: #ffffff;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.1);
        }

        *, *::before, *::after {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--page-bg);
            color: var(--text-main);
            transition: background-color 0.4s ease, color 0.4s ease;
            min-height: 100vh;
            overflow-x: hidden;
            display: flex;
            flex-direction: column;
        }

        .user-avatar {
            width: 32px;
            height: 32px;
            border-radius: 50%;
            object-fit: cover;
            display: none;
        }
        body.theme-dark .avatar-dark { display: block; }
        body.theme-light .avatar-light { display: block; }

        .logo-container {
            position: absolute;
            left: 50%;
            transform: translateX(-50%);
            display: flex;
            align-items: center;
        }
        .main-logo {
            height: 100px;
            display: none;
            transition: height 0.3s ease;
        }
        body.theme-dark .logo-dark { display: block; }
        body.theme-light .logo-light { display: block; }

        .user-pill {
            width: 145px; 
        }

        .mode-toggle {
            position: absolute;
            opacity: 0;
            width: 0;
            height: 0;
            margin: 0;
            padding: 0;
            pointer-events: none;
        }

        .theme-switcher label {
            display: flex;
            align-items: center;
            justify-content: center;
            width: 100%;
            height: 100%;
            cursor: pointer;
        }

        .theme-icon-img {
            display: block; 
            margin: 0 auto;
            width: 24px;
            height: 24px;
            filter: invert(1);
        }
        .theme-light .theme-icon-img { filter: invert(0); }

        .top-sticky-wrapper {
            position: sticky;
            top: 0;
            z-index: 1000;
            width: 100%;
            background: transparent;
            pointer-events: none;
        }
        .top-sticky-wrapper > * { pointer-events: auto; }

        .site-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 20px 40px;
            background: transparent;
            backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px);
            pointer-events: auto;
            position: relative;
            z-index: 2;
        }

        .user-pill {
            display: flex;
            align-items: center;
            text-decoration: none;
            background: var(--user-pill-bg);
            border-radius: 50px;
            padding: 8px 16px 8px 8px;
            gap: 10px;
            width: auto;
            height: 44px;
            box-shadow: 0 0 8px var(--glow-soft);
            animation: userPulse 3s infinite alternate;
            flex-shrink: 0;
        }
        @keyframes userPulse {
            0% { box-shadow: 0 0 6px var(--glow-soft); }
            100% { box-shadow: 0 0 16px var(--glow-color); }
        }
        .user-avatar { width: 32px; height: 32px; border-radius: 50%; object-fit: cover; }
        .user-text { font-size: 14px; font-weight: 400; color: var(--user-pill-text); letter-spacing: 0.5px; }

        .logo-container {
            position: absolute;
            left: 50%;
            transform: translateX(-50%);
            display: flex;
            align-items: center;
        }

        .theme-switcher {
            width: 48px; height: 48px; background: var(--switcher-bg); border-radius: 14px;
            display: flex; align-items: center; justify-content: center; cursor: pointer;
            box-shadow: 0 0 10px var(--glow-soft); animation: switcherPulse 3s infinite alternate; flex-shrink: 0;
        }
        @keyframes switcherPulse {
            0% { box-shadow: 0 0 8px var(--glow-soft); }
            100% { box-shadow: 0 0 18px var(--glow-color); }
        }
        .theme-icon-img { width: 24px; height: 24px; filter: invert(1); }
        .theme-light .theme-icon-img { filter: invert(0); }

        .navigation {
            display: flex; justify-content: center; align-items: center; gap: 8px;
            background: var(--nav-bg); border-radius: 40px; padding: 6px;
            margin: 0px 30px 35px 30px; height: 52px; pointer-events: auto;
            box-shadow: 0 6px 18px rgba(0, 0, 0, 0.3);
        }
        .nav-btn {
            color: #ffffff; text-decoration: none; font-size: 18px; font-weight: 600;
            padding: 8px 22px; border-radius: 30px; transition: all 0.3s ease;
            white-space: nowrap; position: relative; background: rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(4px); -webkit-backdrop-filter: blur(4px);
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        .nav-btn:hover, .nav-btn.active {
            background: rgba(255, 255, 255, 0.35); backdrop-filter: blur(10px);
            -webkit-backdrop-filter: blur(10px); border: 1px solid rgba(255, 255, 255, 0.25);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }

        .page-wrapper { flex: 1; padding: 0 20px 50px 20px; display: flex; justify-content: center; }

        .card {
            background: var(--card-bg); width: 100%; max-width: 840px; border-radius: 22px;
            padding: 36px 48px; box-shadow: 0 0 24px var(--glow-color), var(--card-internal-shadow);
            position: relative; color: var(--card-text);
        }

        .card-header {
            text-align: left; font-size: 32px; font-weight: 700; color: var(--accent-green); margin-bottom: 24px;
        }

        .logout-icon {
            position: absolute;
            top: 28px;
            right: 36px;
            cursor: pointer;
            opacity: 0.7;
            transition: opacity 0.2s ease;
            width: 26px;
            height: 26px;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .logout-icon:hover { opacity: 1; }

        .logout-img {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: block;
            pointer-events: none;
            position: absolute;
            top: 0;
            left: 0;
            transition: opacity 0.2s ease;
            opacity: 0;
        }

        body.theme-dark .logout-dark-default { opacity: 1; }
        body.theme-dark .logout-icon:hover .logout-dark-default { opacity: 0; }
        body.theme-dark .logout-icon:hover .logout-dark-hover { opacity: 1; }

        body.theme-light .logout-light-default { opacity: 1; }
        body.theme-light .logout-icon:hover .logout-light-default { opacity: 0; }
        body.theme-light .logout-icon:hover .logout-light-hover { opacity: 1; }

        .field-icon {
            position: absolute;
            right: 12px;
            top: 50%;
            transform: translateY(-50%);
            width: 18px;
            height: 18px;
            opacity: 0; 
            transition: opacity 0.2s ease;
            pointer-events: none;
        }

        .input-wrapper {
            position: relative;
        }

        .profile-layout {
            display: grid; grid-template-columns: 150px 1fr; gap: 32px 40px; align-items: start; margin-bottom: 32px;
        }
        .avatar { width: 150px; height: 150px; border-radius: 18px; object-fit: cover; }
        .timezone-section { margin-top: 14px; }

        .gender-label {
            font-size: 18px; font-weight: 700; color: var(--accent-green); display: flex; align-items: center; gap: 6px; margin-bottom: 10px;
        }
        .timezone-label {
            font-size: 22px; font-weight: 700; color: var(--accent-green); display: flex; align-items: center; gap: 6px; margin-bottom: 10px;
        }
        .lock-icon { font-size: 13px; opacity: 0.45; }

        .timezone-box {
            display: inline-block; border: 1.5px solid rgba(128, 128, 128, 0.3); border-radius: 22px;
            padding: 10px 22px; font-size: 16.5px; color: #888; white-space: nowrap; background: rgba(0,0,0,0.03);
        }
        .profile-info { display: flex; flex-direction: column; gap: 4px; }
        .profile-name { font-size: 20px; font-weight: 700; color: var(--accent-green); }
        .profile-email { font-size: 16px; color: var(--accent-green); opacity: 0.7; margin-bottom: 15px; }

        .gender-options { display: flex; gap: 18px; }
        .gender-opt {
            display: flex; align-items: center; gap: 9px; padding: 7px 20px;
            border: 1.5px solid rgba(128, 128, 128, 0.3); border-radius: 22px; cursor: pointer;
            font-size: 15px; color: var(--card-text); background: transparent;
            transition: all 0.3s ease;
        }
        .gender-opt.selected {
            border-color: var(--accent-green);
            background: #ffffff;
            box-shadow: 0 0 15px var(--glow-soft);
            color: var(--accent-green);
        }

        .radio-dot {
            width: 16px; height: 16px; border: 2px solid rgba(128, 128, 128, 0.4);
            border-radius: 50%; position: relative;
        }
        .gender-opt.selected .radio-dot { border-color: var(--accent-green); }
        .gender-opt.selected .radio-dot::after {
            content: ''; position: absolute; top: 2px; left: 2px; width: 8px; height: 8px;
            background: var(--accent-green); border-radius: 50%;
        }

        .card-actions { display: flex; justify-content: center; gap: 36px; margin-top: 20px; }

        .btn {
            padding: 11px 36px; border-radius: 22px; font-size: 16px; font-weight: 600;
            cursor: pointer; border: none; transition: all 0.3s ease;
        }

        .btn-edit {
            background: var(--accent-green);
            color: #ffffff;
            border: 1.5px solid var(--accent-green);
            box-shadow: 0 0 15px var(--glow-soft);
        }
        .btn-edit:hover {
            transform: scale(1.03);
            box-shadow: 0 0 25px var(--glow-color), 0 0 10px var(--glow-color);
            filter: brightness(1.1);
        }

        .btn-delete {
            background: #ffffff;
            color: #e55a5a;
            border: 1.5px solid #ffb3b3;
            box-shadow: 0 0 15px rgba(229, 90, 90, 0.25);
        }
        .btn-delete:hover {
            transform: scale(1.03);
            background: #fff5f5;
            color: #d32f2f;
            border-color: #ff8a8a;
            box-shadow: 0 0 25px rgba(229, 90, 90, 0.6), 0 0 10px rgba(229, 90, 90, 0.4);
        }

        .site-footer {
            background: var(--footer-bg); color: var(--footer-text); padding: 36px 40px 0;
            transition: background 0.4s ease; margin-top: auto;
        }
        .footer-columns {
            display: grid; grid-template-columns: 1.3fr 1fr 0.8fr; gap: 30px;
            max-width: 900px; margin: 0 auto; padding-bottom: 24px; border-bottom: 1px solid rgba(255,255,255,0.2);
        }
        .footer-column-title { font-size: 20px; font-weight: 700; margin-bottom: 12px; color: var(--footer-column-title); }
        .footer-column-text { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-column-list { list-style: none; padding: 0; }
        .footer-column-list li { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-copyright { text-align: center; padding: 16px 0 12px; font-size: 14px; color: var(--footer-copy); max-width: 900px; margin: 0 auto; }

        .modal-overlay {
            position: fixed;
            top: 0; left: 0; width: 100%; height: 100%;
            background: rgba(0, 0, 0, 0.4);
            backdrop-filter: blur(5px);
            -webkit-backdrop-filter: blur(5px);
            display: flex; align-items: center; justify-content: center;
            z-index: 2000;
            opacity: 0; visibility: hidden; transition: all 0.3s ease;
        }
        .modal-overlay.active { opacity: 1; visibility: visible; }

        .avatar-modal {
            background: var(--card-bg);
            padding: 24px 32px;
            border-radius: 24px;
            box-shadow: 0 10px 40px rgba(0, 0, 0, 0.2);
            text-align: center;
            position: relative;
            max-width: 320px;
            width: 90%;
            transition: transform 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275);
            transform: scale(0.9);
        }
        .modal-overlay.active .avatar-modal { transform: scale(1); }

        .modal-close-btn {
            position: absolute; top: 12px; right: 12px;
            background: var(--accent-green); color: white;
            border: none; border-radius: 50%; width: 24px; height: 24px;
            font-size: 16px; line-height: 24px; cursor: pointer;
            display: flex; align-items: center; justify-content: center;
        }
        .modal-title { font-size: 20px; font-weight: 700; color: var(--accent-green); margin-bottom: 16px; }

        .avatar-preview-area { display: none; margin-bottom: 16px; }
        .modal-overlay.has-preview .avatar-preview-area { display: block; }
        .modal-overlay.has-preview .modal-select-link { display: none; }
        .modal-overlay.has-preview .modal-save-btn { display: inline-block; }

        .modal-preview-img {
            width: 120px; height: 120px; object-fit: cover;
            border-radius: 18px; margin-bottom: 8px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.1);
        }
        .preview-label { font-size: 13px; color: #888; display: block; margin-bottom: 12px; }

        .modal-select-link {
            font-size: 18px; font-weight: 700; color: var(--accent-green);
            text-decoration: underline; cursor: pointer; display: block; margin-bottom: 10px;
        }
        .modal-select-link:hover { color: #1a9e4a; }

        .modal-save-btn {
            display: none;
            background: var(--accent-green); color: white;
            border: none; padding: 10px 32px; border-radius: 22px;
            font-size: 16px; font-weight: 600; cursor: pointer;
            box-shadow: 0 4px 15px var(--glow-soft);
            transition: all 0.2s ease;
        }
        .modal-save-btn:hover { transform: scale(1.03); filter: brightness(1.1); }

        #hidden-file-input { display: none; }

        @media (max-width: 768px) {
            .site-header { padding: 12px 16px; flex-wrap: wrap; gap: 12px; }
            .logo-container { position: relative !important; left: auto !important; transform: none !important; order: -1; width: 100%; justify-content: center; margin-bottom: 0; }
            .user-pill { width: auto; padding: 6px 12px 6px 6px; height: 38px; order: 1; margin-right: auto; animation: none; }
            .user-avatar { width: 26px; height: 26px; }
            .theme-switcher { position: relative !important; top: auto !important; right: auto !important; width: 42px; height: 42px; order: 2; animation: none; }
            .navigation { order: 3; margin: 8px 12px 20px 12px; flex-wrap: wrap; height: auto; gap: 6px; padding: 6px; }
            .nav-btn { padding: 8px 16px; font-size: 15px; }
            .page-wrapper { padding: 0 12px 40px 12px; }
            .card { padding: 24px 20px; }
            .profile-layout { grid-template-columns: 1fr; }
            .avatar { width: 130px; height: 130px; margin: 0 auto; display: block; }
            .timezone-box { white-space: normal; width: 100%; text-align: center; }
            .gender-options { justify-content: center; }
            .card-actions { flex-direction: column; align-items: center; gap: 16px; }
            .footer-columns { grid-template-columns: 1fr; text-align: center; }
            .logout-icon {
            top: 20px;
            right: 20px;
            width: 22px;
            height: 22px;
        }
        }
    </style>
</head>
<body class="theme-light">

    <div class="top-sticky-wrapper">
        <header class="site-header">
            <a href="/profile" class="user-pill">
                <img src="Т.пол.png" alt="Avatar Dark" class="user-avatar avatar-dark" id="header-avatar-dark">
                <img src="Св.пол.png" alt="Avatar Light" class="user-avatar avatar-light" id="header-avatar-light">
                <span class="user-text" id="header-username">User_login</span>
            </a>

            <div class="logo-container">
                <img src="Светлая.png" alt="Зелёная полка (Dark)" class="main-logo logo-dark">
                <img src="Тёмная.png" alt="Зелёная полка (Light)" class="main-logo logo-light">
            </div>

            <div class="theme-switcher" onclick="toggleTheme()">
                <input type="checkbox" id="theme-toggle" class="mode-toggle">
                <label for="theme-toggle">
                    <img src="тема.png" alt="Тема" class="theme-icon-img">
                </label>
            </div>
        </header>

        <nav class="navigation">
            <a href="/app" class="nav-btn">Главная</a>
            <a href="/app" class="nav-btn">Мои растения</a>
            <a href="/app" class="nav-btn">Настройки</a>
        </nav>
    </div>

    <main class="page-wrapper">
        <div class="card">
            <a href="/login" class="add-link" style="text-decoration: none;">
                <div class="logout-icon" onclick="logout()" title="Выйти">
                    <img src="ненаведдверьтем.png" alt="Выйти" class="logout-img logout-dark-default">
                    <img src="наведдверьтем.png" alt="Выйти при наведении" class="logout-img logout-dark-hover">
                    <img src="ненаведдверьсвет.png" alt="Выйти" class="logout-img logout-light-default">
                    <img src="наведдверьсвет.png" alt="Выйти при наведении" class="logout-img logout-light-hover">
                </div>
            </a>

            <div class="card-header">Профиль пользователя</div>

            <div class="profile-layout">
                <div>
                    <img class="avatar" src="https://i.imgur.com/8KxHqJp.jpg" alt="avatar" id="profile-avatar">
                    <div class="timezone-section">
                        <div class="timezone-label">Часовой пояс </div>
                        <div class="timezone-box" id="profile-timezone">(UTC+05:00)Asian/Yekaterinburg</div>
                    </div>
                </div>
                
                <div class="profile-info">
                    <div class="profile-name" id="profile-name">User_login</div>
                    <div class="profile-email" id="profile-email">name_mail@email.com</div>
                    
                    <div>
                        <div class="gender-label">Пол</div>
                        <div class="gender-options">
                            <div class="gender-opt selected" id="view-gender-female">
                                <div class="radio-dot"></div>
                                Женский
                            </div>
                            <div class="gender-opt" id="view-gender-male">
                                <div class="radio-dot"></div>
                                Мужской
                            </div>
                        </div>
                    </div>
                </div>
            </div>

            <div class="card-actions">
                <button class="btn btn-edit" onclick="window.location.href='/edit-profile'">Редактировать</button>
                <button class="btn btn-delete" onclick="deleteAccount()">Удалить аккаунт</button>
            </div>
        </div>
    </main>

    <footer class="site-footer">
        <div class="footer-columns">
            <div>
                <div class="footer-column-title">Зелёная полка</div>
                <div class="footer-column-text">Система контроля микроклимата растений соответствует приоритетам техлидерства РФ, IoT и импортозамещению.</div>
            </div>
            <div>
                <div class="footer-column-title">Модули</div>
                <ul class="footer-column-list">
                    <li>Освещения растений</li>
                    <li>Сбор данных о растениях</li>
                    <li>Полив растений</li>
                </ul>
            </div>
            <div>
                <div class="footer-column-title">Контакты</div>
                <ul class="footer-column-list">
                    <li>Почта</li>
                    <li>Наши сети</li>
                </ul>
            </div>
        </div>
        <div class="footer-copyright">2026 Зелёная полка</div>
    </footer>

    <script>
        function toggleTheme() {
            document.body.classList.toggle('theme-light');
            document.body.classList.toggle('theme-dark');
            const theme = document.body.classList.contains('theme-dark') ? 'dark' : 'light';
            localStorage.setItem('theme', theme);
        }
        function loadTheme() {
            const savedTheme = localStorage.getItem('theme') || 'light';
            if (savedTheme === 'dark') {
                document.body.classList.remove('theme-light');
                document.body.classList.add('theme-dark');
            }
        }
        loadTheme();

        document.addEventListener('DOMContentLoaded', function() {
            const defaultData = {
                username: 'User_login',
                email: 'name_mail@email.com',
                avatar: 'https://i.imgur.com/8KxHqJp.jpg',
                gender: 'female',
                timezone: '(UTC+05:00) Asian/Yekaterinburg'
            };

            const userData = JSON.parse(localStorage.getItem('userData')) || defaultData;

            document.getElementById('header-avatar-dark').src = userData.avatar;
            document.getElementById('header-avatar-light').src = userData.avatar;
            document.getElementById('header-username').textContent = userData.username;
            document.getElementById('profile-avatar').src = userData.avatar;
            document.getElementById('profile-name').textContent = userData.username;
            document.getElementById('profile-email').textContent = userData.email;
            document.getElementById('profile-timezone').textContent = userData.timezone;
            const femaleOpt = document.getElementById('view-gender-female');
            const maleOpt = document.getElementById('view-gender-male');
            
            femaleOpt.classList.remove('selected');
            maleOpt.classList.remove('selected');
            
            if (userData.gender === 'female') {
                femaleOpt.classList.add('selected');
            } else {
                maleOpt.classList.add('selected');
            }
        });

        function logout() {
            if (confirm('Вы уверены, что хотите выйти?')) {
                window.location.href = '/app';
            }
        }

        function deleteAccount() {
            if (confirm('Вы уверены, что хотите удалить аккаунт? Это действие нельзя отменить.')) {
                localStorage.removeItem('userData');
                window.location.href = '/register';
            }
        }
    </script>
</body>
</html>
)rawliteral";

// ====================== ВЕБ-ОБРАБОТЧИКИ ======================
void handleHist() {
  int p = server.hasArg("period") ? server.arg("period").toInt() : 0;
  int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 0;
  time_t start = server.hasArg("start") ? server.arg("start").toInt() : 0;
  time_t end = server.hasArg("end") ? server.arg("end").toInt() : 0;
  
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", readHistoryFromDB(p, limit, start, end));
}

void handleData() {
  DynamicJsonDocument doc(128);
  doc["temp"] = currentTemp;
  doc["humidity"] = currentHum;
  doc["soil"] = currentSoil;
  doc["pressure"] = currentPres;
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", out);
}

void handleTime() {
  time_t now = time(nullptr);
  String timeStr = formatTime(now, true);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "text/plain", timeStr);
}

void handleLogin() {
  server.send(200, "text/html", LOGIN_HTML);
}

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleRegister() {
  server.send(200, "text/html", REGISTER_HTML);
}

void handleProfile() {
  server.send(200, "text/html", PROFILE_HTML);
}
void handleEditProfile() {
  server.send(200, "text/html", EDIT_PROFILE_HTML);
}

// API для входа
void handleApiLogin() {
  if (!server.hasArg("login") || !server.hasArg("password")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing fields\"}");
    return;
  }
  
  String login = server.arg("login");
  String password = server.arg("password");
  
  // Простая проверка (замените на свою логику)
  if (login == "admin" && password == "admin") {
    server.send(200, "application/json", "{\"success\":true,\"token\":\"admin_token_123\"}");
  } else {
    server.send(401, "application/json", "{\"success\":false,\"message\":\"Invalid credentials\"}");
  }
}

// API для регистрации
void handleApiRegister() {
  if (!server.hasArg("login") || !server.hasArg("password") || !server.hasArg("email")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing fields\"}");
    return;
  }
  
  String login = server.arg("login");
  String password = server.arg("password");
  String email = server.arg("email");
  // Здесь должна быть проверка и сохранение в БД
  // Пока просто заглушка
  server.send(200, "application/json", "{\"success\":true,\"token\":\"user_token_456\",\"message\":\"Registration successful\"}");
}

void handleClear() {
  clearDB();
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  handleRoot();
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(800);
  Serial.println("\nESP32-C3 Sensor Module Starting...");
  
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(STA_SSID, STA_PASS);
  Serial.print("Connecting to WiFi");
  int attempts = 15;
  while (attempts-- && WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed, starting AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(ap_ip, ap_ip, ap_mask);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("AP: " + String(AP_SSID) + " | Pass: " + String(AP_PASS));
  } else {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  }
  
  if (MDNS.begin(MDNS_NAME)) {
    Serial.println("mDNS: http://" + String(MDNS_NAME) + ".local");
  }
  
  configTime(5 * 3600, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
  Serial.print("Syncing time");
  for (int i = 0; i < 40; i++) {
    time_t now = time(nullptr);
    if (now >= 1700000000) {
      timeSynced = true;
      Serial.println("\nTime: " + formatTime(now, true));
      break;
    }
    delay(300);
    Serial.print(".");
  }
  if (!timeSynced) {
    Serial.println("\nTime sync timeout, using uptime");
    bootTime = millis() / 1000;
  }
  
  // Инициализация SD-карты и БД
  initDB();
  
  if (!db) {
    Serial.println("Критическая ошибка: БД не инициализирована!");
    while (1) {
      delay(1000);
      Serial.println("Требуется SD-карта для работы!");
    }
  }
  
  // Инициализация SPI для BME280 и e-paper
  SPI.begin();
  
  // Инициализация BME280
  while (!bme.begin()) {
    Serial.println("BME280 not found, retrying...");
    delay(1000);
  }
  Serial.println("BME280 initialized");
  
  // Инициализация E-Paper
  epd.LDirInit();
  epd.Clear();
  paint.SetWidth(56);
  paint.SetHeight(24);
  epd.DisplayPartBaseImage(IMAGE_DATA);
  Serial.println("E-Paper ready");
  
  // Первое чтение датчиков и обновление дисплея
  readAndLog();
  lastLogTime = millis();
 
  // Настройка веб-сервера
  server.on("/", handleLogin);
  server.on("/login", handleLogin);
  server.on("/register", handleRegister);
  server.on("/app", handleRoot);
  server.on("/api/login", HTTP_POST, handleApiLogin);
  server.on("/api/register", HTTP_POST, handleApiRegister);
  server.on("/data", handleData);
  server.on("/hist", handleHist);
  server.on("/time", handleTime);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/profile", handleProfile);
  server.on("/edit-profile", handleEditProfile);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started");
  
  // Настройка OTA
  ArduinoOTA.setHostname("SensorC3");
  ArduinoOTA.onStart([]() {
    epd.Sleep();
    Serial.println("OTA update started");
  });
  ArduinoOTA.begin();
  
  Serial.println("\nSystem ready! Access via http://" +
    (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "192.168.10.1"));
}

// ====================== LOOP ======================
void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  
  if (millis() - lastLogTime >= 15000UL) {
    readAndLog();
    lastLogTime = millis();
  }
}
