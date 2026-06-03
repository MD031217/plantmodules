#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <ArduinoJson.h>
#include <sqlite3.h>

#define SERIAL_BAUD 115200

// ==================== Wi-Fi НАСТРОЙКИ ====================
#define STA_SSID "linksys"
#define STA_PASS ""
#define AP_SSID  "WateringModule-01"
#define AP_PASS  "12345678"
#define MDNS_NAME "WateringModule-C3"
IPAddress ap_ip(192, 168, 10, 1);
IPAddress ap_mask(255, 255, 255, 0);

// ==================== Пины клапанов ====================
#define VALVE_PINS {4, 5, 6, 7, 15, 16, 17, 18}
const int valvePins[8] = VALVE_PINS;

// ==================== Пины датчиков влажности ====================
#define SENSOR_PINS {34, 35, 32, 33, 25, 26, 27, 14}
const int sensorPins[8] = SENSOR_PINS;

#define CS_SD_PIN SS

// ==================== Объекты ====================
WebServer server(80);

// ==================== SQLite БАЗА ДАННЫХ на SD-карте ====================
#define DB_FILE "/sd/watering.db"
sqlite3 *db = nullptr;
sqlite3_stmt *res = nullptr;
int rc;
char *zErrMsg = 0;

// ==================== Глобальные переменные ====================
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
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("\tРазмер - %llu MB\n", cardSize);
  return true;
}

void createSDDirectories() {
  Serial.println("=== Создание папок на SD-карте ===");
  const char* folders[] = {"image", "avatar", "plant", "www"};
  for (int i = 0; i < 4; i++) {
    String path = String("/") + folders[i];
    if (!SD.exists(path)) {
      if (SD.mkdir(path)) {
        Serial.printf("Создана папка: %s\n", path.c_str());
      }
    }
  }
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
  
  // Включаем поддержку внешних ключей
  sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
  
  // === Таблица valves ===
  const char* sqlValves =
    "CREATE TABLE IF NOT EXISTS valves ("
    "  id INTEGER PRIMARY KEY,"
    "  pin_number INTEGER CHECK(pin_number BETWEEN 1 AND 40),"
    "  plant_name TEXT DEFAULT '',"
    "  display_name TEXT DEFAULT '',"
    "  flow_rate_ml_per_sec REAL DEFAULT 8.33,"
    "  max_duration_sec INTEGER DEFAULT 300 CHECK(max_duration_sec > 0),"
    "  daily_limit_ml REAL DEFAULT 0 CHECK(daily_limit_ml >= 0),"
    "  active INTEGER DEFAULT 1 CHECK(active IN (0,1)),"
    "  moisture_mode INTEGER DEFAULT 0 CHECK(moisture_mode IN (0,1)),"
    "  created_ts INTEGER CHECK(created_ts > 0)"
    ");";
  rc = sqlite3_exec(db, sqlValves, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания valves: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  } else {
    Serial.println("Таблица valves создана/проверена");
  }
  
  // === Таблица watering_tasks ===
  const char* sqlTasks =
    "CREATE TABLE IF NOT EXISTS watering_tasks ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  valve_id INTEGER CHECK(valve_id BETWEEN 1 AND 8),"
    "  schedule_type TEXT NOT NULL CHECK(schedule_type IN ('daily','once','interval','weekly','sunrise','sunset')),"
    "  schedule_time TEXT,"
    "  schedule_interval_min INTEGER DEFAULT 0,"
    "  schedule_days TEXT DEFAULT '',"
    "  weekly_days_config TEXT DEFAULT NULL,"
    "  last_executed_ts INTEGER DEFAULT 0,"
    "  next_execution_ts INTEGER DEFAULT 0,"
    "  priority INTEGER DEFAULT 5 CHECK(priority BETWEEN 1 AND 10),"
    "  cycle_repeat INTEGER DEFAULT 1 CHECK(cycle_repeat > 0),"
    "  max_duration_sec INTEGER DEFAULT 600 CHECK(max_duration_sec > 0),"
    "  max_volume_ml REAL DEFAULT 1000 CHECK(max_volume_ml > 0),"
    "  active INTEGER DEFAULT 1 CHECK(active IN (0,1)),"
    "  suspended INTEGER DEFAULT 0 CHECK(suspended IN (0,1)),"
    "  created_ts INTEGER CHECK(created_ts > 0),"
    "  updated_ts INTEGER CHECK(updated_ts > 0),"
    "  FOREIGN KEY (valve_id) REFERENCES valves(id) ON DELETE CASCADE"
    ");";
  rc = sqlite3_exec(db, sqlTasks, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания watering_tasks: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  } else {
    Serial.println("Таблица watering_tasks создана/проверена");
  }
  
  // === Таблица watering_operations ===
  const char* sqlOps =
    "CREATE TABLE IF NOT EXISTS watering_operations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  task_id INTEGER NOT NULL,"
    "  operation_order INTEGER DEFAULT 1 CHECK(operation_order > 0),"
    "  operation_type TEXT NOT NULL CHECK(operation_type IN ('WATER','PAUSE','SENSOR_CHECK','CUSTOM','TASK_SUMMARY')),"
    "  duration_sec INTEGER DEFAULT 0 CHECK(duration_sec >= 0),"
    "  volume_ml REAL DEFAULT 0 CHECK(volume_ml >= 0),"
    "  pause_duration_sec INTEGER DEFAULT 0 CHECK(pause_duration_sec >= 0),"
    "  flow_rate_override REAL DEFAULT 0,"
    "  target_moisture_percent REAL DEFAULT 65 CHECK(target_moisture_percent BETWEEN 0 AND 100),"
    "  repeat_times INTEGER DEFAULT 1 CHECK(repeat_times > 0),"
    "  wait_after_sec INTEGER DEFAULT 0 CHECK(wait_after_sec >= 0),"
    "  active INTEGER DEFAULT 1 CHECK(active IN (0,1)),"
    "  params_json TEXT DEFAULT NULL,"
    "  created_ts INTEGER CHECK(created_ts > 0),"
    "  updated_ts INTEGER CHECK(updated_ts > 0),"
    "  FOREIGN KEY (task_id) REFERENCES watering_tasks(id) ON DELETE CASCADE"
    ");";
  rc = sqlite3_exec(db, sqlOps, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания watering_operations: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  } else {
    Serial.println("Таблица watering_operations создана/проверена");
  }
  
  // === Таблица task_runs ===
  const char* sqlRuns =
    "CREATE TABLE IF NOT EXISTS task_runs ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  task_id INTEGER,"
    "  valve_id INTEGER CHECK(valve_id BETWEEN 1 AND 8),"
    "  state TEXT NOT NULL CHECK(state IN ('queued','running','completed','failed','skipped')),"
    "  priority INTEGER DEFAULT 5 CHECK(priority BETWEEN 1 AND 10),"
    "  queued_ts INTEGER DEFAULT 0,"
    "  started_ts INTEGER DEFAULT 0,"
    "  finished_ts INTEGER DEFAULT 0,"
    "  target_duration_sec INTEGER DEFAULT 0,"
    "  target_volume_ml REAL DEFAULT 0,"
    "  actual_duration_sec INTEGER DEFAULT 0,"
    "  actual_volume_ml REAL DEFAULT 0,"
    "  error_text TEXT DEFAULT '',"
    "  triggered_by TEXT CHECK(triggered_by IN ('schedule','manual','sensor')),"
    "  sent INTEGER DEFAULT 0 CHECK(sent IN (0,1)),"
    "  FOREIGN KEY (task_id) REFERENCES watering_tasks(id) ON DELETE SET NULL"
    ");";
  rc = sqlite3_exec(db, sqlRuns, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания task_runs: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  } else {
    Serial.println("Таблица task_runs создана/проверена");
  }
  
  // === Таблица watering_log ===
  const char* sqlLog =
    "CREATE TABLE IF NOT EXISTS watering_log ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts INTEGER NOT NULL CHECK(ts > 0),"
    "  run_id INTEGER,"
    "  valve_id INTEGER CHECK(valve_id BETWEEN 1 AND 8),"
    "  duration_sec INTEGER DEFAULT 0 CHECK(duration_sec >= 0),"
    "  volume_ml REAL DEFAULT 0 CHECK(volume_ml >= 0),"
    "  status TEXT DEFAULT 'completed' CHECK(status IN ('completed','failed','skipped')),"
    "  error_text TEXT DEFAULT '',"
    "  triggered_by TEXT CHECK(triggered_by IN ('schedule','manual','sensor')),"
    "  sent INTEGER DEFAULT 0 CHECK(sent IN (0,1)),"
    "  FOREIGN KEY (run_id) REFERENCES task_runs(id) ON DELETE SET NULL"
    ");";
  rc = sqlite3_exec(db, sqlLog, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания watering_log: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  } else {
    Serial.println("Таблица watering_log создана/проверена");
  }
  
  // === Таблица moisture_sensors ===
  const char* sqlSensors =
    "CREATE TABLE IF NOT EXISTS moisture_sensors ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  valve_id INTEGER NOT NULL CHECK(valve_id BETWEEN 1 AND 8),"
    "  target_moisture_percent REAL DEFAULT 65 CHECK(target_moisture_percent BETWEEN 0 AND 100),"
    "  max_value INTEGER DEFAULT 100 CHECK(max_value BETWEEN 0 AND 100),"
    "  min_value INTEGER DEFAULT 0 CHECK(min_value BETWEEN 0 AND 100),"
    "  check_interval_min INTEGER DEFAULT 30 CHECK(check_interval_min > 0),"
    "  last_check_ts INTEGER DEFAULT 0,"
    "  last_moisture_percent REAL DEFAULT 0 CHECK(last_moisture_percent BETWEEN 0 AND 100),"
    "  max_watering_duration_sec INTEGER DEFAULT 180 CHECK(max_watering_duration_sec > 0),"
    "  min_pause_hours INTEGER DEFAULT 2 CHECK(min_pause_hours >= 0),"
    "  last_watering_ts INTEGER DEFAULT 0,"
    "  active INTEGER DEFAULT 1 CHECK(active IN (0,1)),"
    "  sensor_ok INTEGER DEFAULT 1 CHECK(sensor_ok IN (0,1)),"
    "  failed_checks INTEGER DEFAULT 0 CHECK(failed_checks >= 0),"
    "  samples_to_avg INTEGER DEFAULT 3 CHECK(samples_to_avg > 0),"
    "  created_ts INTEGER CHECK(created_ts > 0),"
    "  FOREIGN KEY (valve_id) REFERENCES valves(id) ON DELETE CASCADE"
    ");";
  rc = sqlite3_exec(db, sqlSensors, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания moisture_sensors: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  } else {
    Serial.println("Таблица moisture_sensors создана/проверена");
  }
  
  // === Создание индексов ===
  sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_tasks_valve ON watering_tasks(valve_id);", NULL, NULL, NULL);
  sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_tasks_active_next ON watering_tasks(active, next_execution_ts);", NULL, NULL, NULL);
  sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_ops_task_order ON watering_operations(task_id, operation_order);", NULL, NULL, NULL);
  sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_runs_state ON task_runs(state, priority, queued_ts);", NULL, NULL, NULL);
  sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_log_ts ON watering_log(ts DESC);", NULL, NULL, NULL);
  sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_sensors_valve ON moisture_sensors(valve_id);", NULL, NULL, NULL);
  
  // === Инициализация 8 клапанов по умолчанию ===
  sqlite3_stmt* stmt;
  const char* insertValve = "INSERT OR IGNORE INTO valves (id, pin_number, plant_name, created_ts) VALUES (?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db, insertValve, -1, &stmt, NULL) == SQLITE_OK) {
    for (int i = 0; i < 8; i++) {
      sqlite3_bind_int(stmt, 1, i + 1);
      sqlite3_bind_int(stmt, 2, valvePins[i]);
      String name = "Клапан " + String(i + 1);
      sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 4, time(nullptr));
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    Serial.println("Клапаны инициализированы");
  }
  
  // === Инициализация датчиков для каждого клапана ===
  const char* insertSensor = "INSERT OR IGNORE INTO moisture_sensors (valve_id, created_ts) VALUES (?, ?);";
  if (sqlite3_prepare_v2(db, insertSensor, -1, &stmt, NULL) == SQLITE_OK) {
    for (int i = 0; i < 8; i++) {
      sqlite3_bind_int(stmt, 1, i + 1);
      sqlite3_bind_int(stmt, 2, time(nullptr));
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    Serial.println("Датчики инициализированы");
  }
  
  Serial.println("Все таблицы проверены и готовы");
}

// ==================== API: ПОЛУЧИТЬ ВСЕ КЛАПАНЫ С ЗАДАЧАМИ И ДАТЧИКАМИ ====================
String getAllValvesData() {
  if (!db) return "{}";
  
  DynamicJsonDocument doc(16384);
  JsonObject root = doc.to<JsonObject>();
  
  // Получаем все клапаны
  const char* sql = "SELECT * FROM valves ORDER BY id";
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      int valveId = sqlite3_column_int(stmt, 0);
      JsonObject valve = root.createNestedObject(String(valveId));
      
      valve["active"] = sqlite3_column_int(stmt, 7);
      valve["pin_number"] = sqlite3_column_int(stmt, 1);
      valve["max_duration_sec"] = sqlite3_column_int(stmt, 5);
      valve["daily_limit_ml"] = sqlite3_column_double(stmt, 6);
      valve["moisture_mode"] = sqlite3_column_int(stmt, 8);
      
      const char* plantName = (const char*)sqlite3_column_text(stmt, 2);
      valve["plant_name"] = plantName ? plantName : "";
      
      // Получаем задачи для этого клапана
      JsonObject task = valve.createNestedObject("task");
      const char* taskSql = "SELECT * FROM watering_tasks WHERE valve_id = ? ORDER BY id DESC LIMIT 1";
      sqlite3_stmt* taskStmt;
      if (sqlite3_prepare_v2(db, taskSql, -1, &taskStmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(taskStmt, 1, valveId);
        if (sqlite3_step(taskStmt) == SQLITE_ROW) {
          task["schedule_type"] = (const char*)sqlite3_column_text(taskStmt, 2) ? (const char*)sqlite3_column_text(taskStmt, 2) : "daily";
          task["schedule_time"] = (const char*)sqlite3_column_text(taskStmt, 3) ? (const char*)sqlite3_column_text(taskStmt, 3) : "";
          task["schedule_interval_min"] = sqlite3_column_int(taskStmt, 4);
          task["schedule_days"] = (const char*)sqlite3_column_text(taskStmt, 5) ? (const char*)sqlite3_column_text(taskStmt, 5) : "";
          task["priority"] = sqlite3_column_int(taskStmt, 8);
          task["cycle_repeat"] = sqlite3_column_int(taskStmt, 9);
          task["max_duration_sec"] = sqlite3_column_int(taskStmt, 10);
          task["max_volume_ml"] = sqlite3_column_double(taskStmt, 11);
          
          int taskId = sqlite3_column_int(taskStmt, 0);
          
          // Получаем weekly_days_config
          const char* weeklyConfig = (const char*)sqlite3_column_text(taskStmt, 6);
          if (weeklyConfig) {
            DynamicJsonDocument weeklyDoc(1024);
            deserializeJson(weeklyDoc, weeklyConfig);
            task["weekly_days"] = weeklyDoc.as<JsonObject>();
          } else {
            task["weekly_days"] = JsonObject();
          }
          
          // Получаем операции для этой задачи
          JsonArray operations = task.createNestedArray("operations");
          const char* opsSql = "SELECT * FROM watering_operations WHERE task_id = ? ORDER BY operation_order";
          sqlite3_stmt* opsStmt;
          if (sqlite3_prepare_v2(db, opsSql, -1, &opsStmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(opsStmt, 1, taskId);
            while (sqlite3_step(opsStmt) == SQLITE_ROW) {
              JsonObject op = operations.createNestedObject();
              op["type"] = (const char*)sqlite3_column_text(opsStmt, 3);
              op["duration_sec"] = sqlite3_column_int(opsStmt, 4);
              op["volume_ml"] = sqlite3_column_double(opsStmt, 5);
              
              const char* paramsJson = (const char*)sqlite3_column_text(opsStmt, 12);
              if (paramsJson) {
                DynamicJsonDocument paramsDoc(1024);
                deserializeJson(paramsDoc, paramsJson);
                op["_params"] = paramsDoc.as<JsonObject>();
              }
            }
            sqlite3_finalize(opsStmt);
          }
        } else {
          // Если задач нет, создаём пустую структуру
          task["schedule_type"] = "daily";
          task["schedule_time"] = "";
          task["schedule_interval_min"] = 0;
          task["schedule_days"] = "";
          task["priority"] = 5;
          task["cycle_repeat"] = 1;
          task["max_duration_sec"] = 600;
          task["max_volume_ml"] = 1000;
          task["weekly_days"] = JsonObject();
          task["operations"] = JsonArray();
        }
        sqlite3_finalize(taskStmt);
      }
      
      // Получаем датчик для этого клапана
      JsonObject sensor = valve.createNestedObject("sensor");
      const char* sensorSql = "SELECT * FROM moisture_sensors WHERE valve_id = ? LIMIT 1";
      sqlite3_stmt* sensorStmt;
      if (sqlite3_prepare_v2(db, sensorSql, -1, &sensorStmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(sensorStmt, 1, valveId);
        if (sqlite3_step(sensorStmt) == SQLITE_ROW) {
          sensor["active"] = sqlite3_column_int(sensorStmt, 11);
          sensor["check_interval_min"] = sqlite3_column_int(sensorStmt, 5);
          sensor["target_moisture_percent"] = sqlite3_column_double(sensorStmt, 2);
          sensor["max_watering_duration_sec"] = sqlite3_column_int(sensorStmt, 8);
          sensor["min_pause_hours"] = sqlite3_column_int(sensorStmt, 9);
          sensor["last_moisture_percent"] = sqlite3_column_double(sensorStmt, 7);
        } else {
          sensor["active"] = 0;
          sensor["check_interval_min"] = 30;
          sensor["target_moisture_percent"] = 65;
          sensor["max_watering_duration_sec"] = 180;
          sensor["min_pause_hours"] = 2;
          sensor["last_moisture_percent"] = 0;
        }
        sqlite3_finalize(sensorStmt);
      }
    }
    sqlite3_finalize(stmt);
  }
  
  String out;
  serializeJson(doc, out);
  return out;
}

// ==================== API: ОБНОВИТЬ НАСТРОЙКИ КЛАПАНА ====================
void handleUpdateValve() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing data\"}");
    return;
  }
  
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  int valveId = doc["id"] | 0;
  if (valveId < 1 || valveId > 8) {
    server.send(400, "application/json", "{\"error\":\"Invalid valve ID\"}");
    return;
  }
  
  // Обновляем клапан
  String sql = "UPDATE valves SET active=?, max_duration_sec=?, daily_limit_ml=?, moisture_mode=?, plant_name=? WHERE id=?";
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, doc["active"] | 1);
    sqlite3_bind_int(stmt, 2, doc["max_duration_sec"] | 300);
    sqlite3_bind_double(stmt, 3, doc["daily_limit_ml"] | 0);
    sqlite3_bind_int(stmt, 4, doc["moisture_mode"] | 0);
    const char* plantName = doc["plant_name"] | "";
    sqlite3_bind_text(stmt, 5, plantName, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, valveId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  // Обновляем задачу
  if (doc.containsKey("task")) {
    JsonObject task = doc["task"];
    
    // Проверяем, есть ли уже задача для этого клапана
    const char* checkSql = "SELECT id FROM watering_tasks WHERE valve_id = ? LIMIT 1";
    sqlite3_stmt* checkStmt;
    int taskId = 0;
    if (sqlite3_prepare_v2(db, checkSql, -1, &checkStmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(checkStmt, 1, valveId);
      if (sqlite3_step(checkStmt) == SQLITE_ROW) {
        taskId = sqlite3_column_int(checkStmt, 0);
      }
      sqlite3_finalize(checkStmt);
    }
    
    if (taskId > 0) {
      // Обновляем существующую задачу
      sql = "UPDATE watering_tasks SET schedule_type=?, schedule_time=?, schedule_interval_min=?, "
            "schedule_days=?, priority=?, cycle_repeat=?, max_duration_sec=?, max_volume_ml=?, "
            "weekly_days_config=?, updated_ts=? WHERE id=?";
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
        const char* schedType = task["schedule_type"] | "daily";
        sqlite3_bind_text(stmt, 1, schedType, -1, SQLITE_TRANSIENT);
        const char* schedTime = task["schedule_time"] | "";
        sqlite3_bind_text(stmt, 2, schedTime, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, task["schedule_interval_min"] | 0);
        const char* schedDays = task["schedule_days"] | "";
        sqlite3_bind_text(stmt, 4, schedDays, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, task["priority"] | 5);
        sqlite3_bind_int(stmt, 6, task["cycle_repeat"] | 1);
        sqlite3_bind_int(stmt, 7, task["max_duration_sec"] | 600);
        sqlite3_bind_double(stmt, 8, task["max_volume_ml"] | 1000);
        
        // weekly_days_config как JSON
        String weeklyJson;
        if (task.containsKey("weekly_days")) {
          serializeJson(task["weekly_days"], weeklyJson);
        }
        sqlite3_bind_text(stmt, 9, weeklyJson.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 10, time(nullptr));
        sqlite3_bind_int(stmt, 11, taskId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    } else {
      // Создаём новую задачу
      sql = "INSERT INTO watering_tasks (valve_id, schedule_type, schedule_time, schedule_interval_min, "
            "schedule_days, priority, cycle_repeat, max_duration_sec, max_volume_ml, weekly_days_config, "
            "created_ts, updated_ts) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, valveId);
        const char* schedType = task["schedule_type"] | "daily";
        sqlite3_bind_text(stmt, 2, schedType, -1, SQLITE_TRANSIENT);
        const char* schedTime = task["schedule_time"] | "";
        sqlite3_bind_text(stmt, 3, schedTime, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, task["schedule_interval_min"] | 0);
        const char* schedDays = task["schedule_days"] | "";
        sqlite3_bind_text(stmt, 5, schedDays, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 6, task["priority"] | 5);
        sqlite3_bind_int(stmt, 7, task["cycle_repeat"] | 1);
        sqlite3_bind_int(stmt, 8, task["max_duration_sec"] | 600);
        sqlite3_bind_double(stmt, 9, task["max_volume_ml"] | 1000);
        
        String weeklyJson;
        if (task.containsKey("weekly_days")) {
          serializeJson(task["weekly_days"], weeklyJson);
        }
        sqlite3_bind_text(stmt, 10, weeklyJson.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 11, time(nullptr));
        sqlite3_bind_int(stmt, 12, time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
  }
  
  // Обновляем датчик
  if (doc.containsKey("sensor")) {
    JsonObject sensor = doc["sensor"];
    sql = "UPDATE moisture_sensors SET active=?, check_interval_min=?, target_moisture_percent=?, "
          "max_watering_duration_sec=?, min_pause_hours=? WHERE valve_id=?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, sensor["active"] | 0);
      sqlite3_bind_int(stmt, 2, sensor["check_interval_min"] | 30);
      sqlite3_bind_double(stmt, 3, sensor["target_moisture_percent"] | 65);
      sqlite3_bind_int(stmt, 4, sensor["max_watering_duration_sec"] | 180);
      sqlite3_bind_int(stmt, 5, sensor["min_pause_hours"] | 2);
      sqlite3_bind_int(stmt, 6, valveId);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
  
  server.send(200, "application/json", "{\"success\":true}");
}

// ==================== API: ДОБАВИТЬ ЗАДАЧУ ====================
void handleAddTask() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing data\"}");
    return;
  }
  
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  int valveId = doc["valve_id"] | 0;
  if (valveId < 1 || valveId > 8) {
    server.send(400, "application/json", "{\"error\":\"Invalid valve ID\"}");
    return;
  }
  
  // Создаём задачу
  String sql = "INSERT INTO watering_tasks (valve_id, schedule_type, schedule_time, schedule_interval_min, "
               "schedule_days, priority, cycle_repeat, max_duration_sec, max_volume_ml, weekly_days_config, "
               "created_ts, updated_ts) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt;
  int taskId = 0;
  
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, valveId);
    const char* schedType = doc["schedule_type"] | "daily";
    sqlite3_bind_text(stmt, 2, schedType, -1, SQLITE_TRANSIENT);
    const char* schedTime = doc["schedule_time"] | "";
    sqlite3_bind_text(stmt, 3, schedTime, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, doc["schedule_interval_min"] | 0);
    const char* schedDays = doc["schedule_days"] | "";
    sqlite3_bind_text(stmt, 5, schedDays, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, doc["priority"] | 5);
    sqlite3_bind_int(stmt, 7, doc["cycle_repeat"] | 1);
    sqlite3_bind_int(stmt, 8, doc["max_duration_sec"] | 600);
    sqlite3_bind_double(stmt, 9, doc["max_volume_ml"] | 1000);
    
    String weeklyJson;
    if (doc.containsKey("weekly_days")) {
      serializeJson(doc["weekly_days"], weeklyJson);
    }
    sqlite3_bind_text(stmt, 10, weeklyJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 11, time(nullptr));
    sqlite3_bind_int(stmt, 12, time(nullptr));
    
    if (sqlite3_step(stmt) == SQLITE_DONE) {
      taskId = sqlite3_last_insert_rowid(db);
    }
    sqlite3_finalize(stmt);
  }
  
  if (taskId > 0 && doc.containsKey("operations")) {
    JsonArray operations = doc["operations"].as<JsonArray>();
    int order = 1;
    for (JsonObject op : operations) {
      sql = "INSERT INTO watering_operations (task_id, operation_order, operation_type, duration_sec, "
            "volume_ml, params_json, created_ts, updated_ts) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, taskId);
        sqlite3_bind_int(stmt, 2, order++);
        const char* opType = op["type"] | "WATER";
        sqlite3_bind_text(stmt, 3, opType, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, op["duration_sec"] | 0);
        sqlite3_bind_double(stmt, 5, op["volume_ml"] | 0);
        
        String paramsJson;
        if (op.containsKey("_params")) {
          serializeJson(op["_params"], paramsJson);
        }
        sqlite3_bind_text(stmt, 6, paramsJson.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, time(nullptr));
        sqlite3_bind_int(stmt, 8, time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
  }
  
  DynamicJsonDocument resp(256);
  resp["success"] = true;
  resp["task_id"] = taskId;
  String out;
  serializeJson(resp, out);
  server.send(200, "application/json", out);
}

// ==================== API: УДАЛИТЬ ЗАДАЧУ ====================
void handleDeleteTask() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"Task ID required\"}");
    return;
  }
  
  int taskId = server.arg("id").toInt();
  if (taskId == 0) {
    server.send(400, "application/json", "{\"error\":\"Invalid task ID\"}");
    return;
  }
  
  // Удаляем задачу (операции удалятся каскадно)
  const char* sql = "DELETE FROM watering_tasks WHERE id = ?";
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, taskId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  server.send(200, "application/json", "{\"success\":true}");
}

// ==================== API: ПОЛУЧИТЬ ЖУРНАЛ ПОЛИВОВ ====================
void handleGetWateringLog() {
  int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 100;
  int valveId = server.hasArg("valve_id") ? server.arg("valve_id").toInt() : 0;
  
  String sql = "SELECT * FROM watering_log";
  if (valveId > 0) {
    sql += " WHERE valve_id = " + String(valveId);
  }
  sql += " ORDER BY ts DESC LIMIT " + String(limit);
  
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      JsonObject entry = arr.createNestedObject();
      entry["id"] = sqlite3_column_int(stmt, 0);
      entry["ts"] = sqlite3_column_int(stmt, 1);
      entry["valve"] = sqlite3_column_int(stmt, 3);
      entry["duration"] = sqlite3_column_int(stmt, 4);
      entry["volume"] = sqlite3_column_double(stmt, 5);
      entry["status"] = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "completed";
      entry["type"] = (const char*)sqlite3_column_text(stmt, 8) ? (const char*)sqlite3_column_text(stmt, 8) : "manual";
    }
    sqlite3_finalize(stmt);
  }
  
  String out;
  serializeJson(arr, out);
  server.send(200, "application/json", out);
}

// ==================== API: РУЧНОЙ ПОЛИВ ====================
void handleManualWatering() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing data\"}");
    return;
  }
  
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  int valveId = doc["valve_id"] | 0;
  int duration = doc["duration"] | 30;
  double volume = doc["volume"] | 0;
  
  if (valveId < 1 || valveId > 8) {
    server.send(400, "application/json", "{\"error\":\"Invalid valve ID\"}");
    return;
  }
  
  // Создаём запись в task_runs
  const char* sql = "INSERT INTO task_runs (valve_id, state, triggered_by, target_duration_sec, target_volume_ml, "
                    "actual_duration_sec, actual_volume_ml, started_ts, finished_ts) VALUES (?, 'completed', 'manual', ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt;
  int runId = 0;
  
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, valveId);
    sqlite3_bind_int(stmt, 2, duration);
    sqlite3_bind_double(stmt, 3, volume);
    sqlite3_bind_int(stmt, 4, duration);
    sqlite3_bind_double(stmt, 5, volume);
    sqlite3_bind_int(stmt, 6, time(nullptr));
    sqlite3_bind_int(stmt, 7, time(nullptr));
    
    if (sqlite3_step(stmt) == SQLITE_DONE) {
      runId = sqlite3_last_insert_rowid(db);
    }
    sqlite3_finalize(stmt);
  }
  
  // Создаём запись в watering_log
  if (runId > 0) {
    sql = "INSERT INTO watering_log (ts, run_id, valve_id, duration_sec, volume_ml, status, triggered_by) "
          "VALUES (?, ?, ?, ?, ?, 'completed', 'manual')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, time(nullptr));
      sqlite3_bind_int(stmt, 2, runId);
      sqlite3_bind_int(stmt, 3, valveId);
      sqlite3_bind_int(stmt, 4, duration);
      sqlite3_bind_double(stmt, 5, volume);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
  
  server.send(200, "application/json", "{\"success\":true}");
}

// ==================== API: ПРИМЕНИТЬ НАСТРОЙКИ КО ВСЕМ КЛАПАНАМ ====================
void handleApplyToAll() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing data\"}");
    return;
  }
  
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  int sourceValveId = doc["source_valve_id"] | 0;
  if (sourceValveId < 1 || sourceValveId > 8) {
    server.send(400, "application/json", "{\"error\":\"Invalid source valve ID\"}");
    return;
  }
  
  // Получаем настройки исходного клапана
  const char* sql = "SELECT * FROM watering_tasks WHERE valve_id = ? LIMIT 1";
  sqlite3_stmt* stmt;
  int sourceTaskId = 0;
  
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, sourceValveId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      sourceTaskId = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  
  if (sourceTaskId == 0) {
    server.send(400, "application/json", "{\"error\":\"No task found for source valve\"}");
    return;
  }
  
  // Копируем настройки на все остальные клапаны
  for (int i = 1; i <= 8; i++) {
    if (i == sourceValveId) continue;
    
    // Обновляем или создаём задачу
    sql = "SELECT id FROM watering_tasks WHERE valve_id = ? LIMIT 1";
    int taskId = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, i);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        taskId = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
    }
    
    if (taskId > 0) {
      // Обновляем
      sql = "UPDATE watering_tasks SET schedule_type=(SELECT schedule_type FROM watering_tasks WHERE id=?), "
            "schedule_time=(SELECT schedule_time FROM watering_tasks WHERE id=?), "
            "priority=(SELECT priority FROM watering_tasks WHERE id=?), "
            "cycle_repeat=(SELECT cycle_repeat FROM watering_tasks WHERE id=?), "
            "max_duration_sec=(SELECT max_duration_sec FROM watering_tasks WHERE id=?), "
            "max_volume_ml=(SELECT max_volume_ml FROM watering_tasks WHERE id=?), "
            "updated_ts=? WHERE id=?";
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, sourceTaskId);
        sqlite3_bind_int(stmt, 2, sourceTaskId);
        sqlite3_bind_int(stmt, 3, sourceTaskId);
        sqlite3_bind_int(stmt, 4, sourceTaskId);
        sqlite3_bind_int(stmt, 5, sourceTaskId);
        sqlite3_bind_int(stmt, 6, sourceTaskId);
        sqlite3_bind_int(stmt, 7, time(nullptr));
        sqlite3_bind_int(stmt, 8, taskId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
  }
  
  server.send(200, "application/json", "{\"success\":true}");
}

// ==================== HTML СТРАНИЦА ПОЛИВА ====================
const char WATERING_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Модуль полива</title>
    <link rel="stylesheet" href="myplant-style.css">
    <style>
        /* Дополнительные стили для страницы полива */
        .irrigation-container {
            display: flex;
            gap: 20px;
            padding: 20px 40px;
            min-height: calc(100vh - 300px);
        }
        
        .valves-sidebar {
            width: 300px;
            flex-shrink: 0;
        }
        
        .valve-item {
            background: var(--card-bg);
            border-radius: 16px;
            padding: 16px;
            margin-bottom: 12px;
            box-shadow: var(--card-internal-shadow);
            cursor: pointer;
            transition: all 0.3s ease;
            color: #1e293b;
        }
        
        .valve-item:hover {
            transform: translateX(5px);
            box-shadow: 0 0 20px var(--glow-soft);
        }
        
        .valve-item.active {
            border: 2px solid var(--accent-green);
            background: rgba(33, 200, 95, 0.05);
        }
        
        .valve-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 10px;
        }
        
        .valve-title {
            font-size: 16px;
            font-weight: 600;
            color: var(--accent-green);
        }
        
        .valve-toggle {
            width: 40px;
            height: 22px;
            background: rgba(0,0,0,0.15);
            border-radius: 11px;
            position: relative;
            cursor: pointer;
            transition: background 0.3s;
        }
        
        .valve-toggle.on {
            background: var(--accent-green);
        }
        
        .valve-toggle::after {
            content: '';
            position: absolute;
            width: 18px;
            height: 18px;
            background: white;
            border-radius: 50%;
            top: 2px;
            left: 2px;
            transition: transform 0.3s;
        }
        
        .valve-toggle.on::after {
            transform: translateX(18px);
        }
        
        .valve-info {
            font-size: 13px;
            color: #374151;
            margin: 4px 0;
        }
        
        .valve-status {
            display: inline-flex;
            align-items: center;
            gap: 5px;
            font-size: 12px;
            padding: 3px 8px;
            background: rgba(33, 200, 95, 0.15);
            border-radius: 8px;
            margin-top: 6px;
            color: var(--accent-green);
            font-weight: 600;
        }
        
        .valve-status.inactive {
            background: rgba(100, 116, 139, 0.15);
            color: #64748b;
        }
        
        .main-content {
            flex: 1;
            background: var(--card-bg);
            border-radius: 24px;
            padding: 30px;
            box-shadow: var(--card-internal-shadow);
            max-height: calc(100vh - 220px);
            overflow-y: auto;
            color: #1e293b;
        }
        
        .content-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 30px;
        }
        
        .page-title {
            font-size: 28px;
            font-weight: 700;
            color: var(--accent-green) !important;
        }
        
        .btn-add-task {
            background: var(--accent-green);
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 20px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
        }
        
        .btn-add-task:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 15px rgba(33, 200, 95, 0.4);
        }
        
        .tabs {
            display: flex;
            gap: 10px;
            margin-bottom: 25px;
            border-bottom: 2px solid rgba(0,0,0,0.05);
            padding-bottom: 10px;
        }
        
        .tab {
            padding: 10px 20px;
            border: none;
            border-radius: 12px;
            font-size: 15px;
            cursor: pointer;
            transition: all 0.3s;
            background: rgba(0,0,0,0.05);
            color: #1e293b;
        }
        
        .tab.active {
            background: var(--accent-green);
            color: white !important;
        }
        
        .form-group {
            margin-bottom: 20px;
        }
        
        .form-label {
            display: block;
            font-size: 14px;
            color: #374151 !important;
            margin-bottom: 8px;
            font-weight: 600;
        }
        
        .form-hint {
            font-size: 12px;
            color: #64748b !important;
            margin-top: 4px;
            font-style: italic;
            opacity: 0.9;
        }
        
        .form-input, .form-select {
            width: 100%;
            padding: 12px 16px;
            background: rgba(0,0,0,0.03);
            border: 2px solid rgba(0,0,0,0.1);
            border-radius: 12px;
            color: #1e293b !important;
            font-size: 15px;
            font-weight: 500;
            outline: none;
            transition: border-color 0.3s;
        }
        
        .form-input:focus, .form-select:focus {
            border-color: var(--accent-green);
        }
        
        .form-input::placeholder, .form-select::placeholder {
            color: #64748b;
        }
        
        .form-input:disabled, .form-select:disabled {
            background: rgba(0,0,0,0.01);
            border-color: rgba(0,0,0,0.05);
            color: #94a3b8 !important;
            cursor: not-allowed;
        }
        
        .form-row {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 15px;
        }
        
        .form-row-2 {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 15px;
        }
        
        .operations-list {
            margin: 25px 0;
        }
        
        .operation-item {
            background: rgba(0,0,0,0.03);
            border-radius: 12px;
            padding: 15px;
            margin-bottom: 12px;
            display: flex;
            align-items: center;
            gap: 15px;
        }
        
        .operation-type {
            padding: 8px 16px;
            border-radius: 10px;
            font-weight: 600;
            font-size: 13px;
            min-width: 120px;
            text-align: center;
        }
        
        .operation-type.water {
            background: rgba(33, 200, 95, 0.2);
            color: var(--accent-green);
        }
        
        .operation-type.pause {
            background: rgba(255, 193, 7, 0.2);
            color: #b45309;
        }
        
        .operation-type.sensor {
            background: rgba(0, 188, 212, 0.2);
            color: #0e7490;
        }
        
        .operation-inputs {
            flex: 1;
            display: flex;
            gap: 10px;
        }
        
        .operation-input {
            flex: 1;
            padding: 8px 12px;
            background: rgba(0,0,0,0.03);
            border: 1px solid rgba(0,0,0,0.1);
            border-radius: 8px;
            color: #1e293b !important;
            font-size: 14px;
            font-weight: 500;
        }
        
        .btn-operation {
            padding: 8px 16px;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-size: 13px;
            font-weight: 600;
            transition: all 0.3s;
        }
        
        .btn-edit {
            background: var(--accent-green);
            color: white;
        }
        
        .btn-delete {
            background: rgba(244, 67, 54, 0.85);
            color: white;
        }
        
        .btn-operation:hover {
            transform: translateY(-1px);
            box-shadow: 0 2px 8px rgba(0,0,0,0.2);
        }
        
        .sidebar-right {
            width: 340px;
            flex-shrink: 0;
        }
        
        .moisture-panel {
            background: var(--card-bg);
            border-radius: 24px;
            padding: 25px;
            box-shadow: var(--card-internal-shadow);
            color: #1e293b;
        }
        
        .panel-title {
            font-size: 20px;
            font-weight: 600;
            color: var(--accent-green) !important;
            margin-bottom: 20px;
            padding-bottom: 15px;
            border-bottom: 2px solid rgba(0,0,0,0.05);
        }
        
        .toggle-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 15px;
        }
        
        .toggle-label {
            font-size: 14px;
            color: #374151 !important;
        }
        
        .global-limits {
            margin-top: 20px;
            padding-top: 20px;
            border-top: 2px solid rgba(0,0,0,0.05);
        }
        
        .action-buttons {
            display: flex;
            gap: 12px;
            margin-top: 25px;
        }
        
        .btn-action {
            flex: 1;
            padding: 14px;
            border: none;
            border-radius: 16px;
            font-size: 15px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
            background: rgba(0,0,0,0.05);
            color: #1e293b;
        }
        
        .btn-action.btn-save {
            background: var(--accent-green);
            color: white;
        }
        
        .btn-action.btn-apply {
            background: rgba(33, 200, 95, 0.15);
            color: var(--accent-green);
        }
        
        .btn-action.btn-history {
            background: rgba(0,0,0,0.05);
            color: #1e293b;
        }
        
        .btn-action:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }
        
        .valve-number {
            display: inline-block;
            width: 28px;
            height: 28px;
            background: var(--accent-green);
            color: white;
            border-radius: 8px;
            text-align: center;
            line-height: 28px;
            font-weight: 700;
            margin-right: 10px;
        }
        
        .section-title {
            font-size: 18px;
            font-weight: 600;
            color: var(--accent-green) !important;
            margin: 25px 0 15px 0;
            padding-bottom: 8px;
            border-bottom: 1px solid rgba(0,0,0,0.05);
        }

        #journalPanel {
            display: none;
        }
        
        .journal-table {
            width: 100%;
            border-collapse: collapse;
            margin-top: 15px;
            font-size: 14px;
        }
        
        .journal-table th, .journal-table td {
            padding: 12px;
            text-align: left;
            border-bottom: 1px solid rgba(0,0,0,0.1);
        }
        
        .journal-table th {
            color: var(--accent-green);
            font-weight: 600;
        }
        
        .journal-table tr:hover {
            background: rgba(33, 200, 95, 0.05);
        }
        
        .status-badge {
            padding: 4px 8px;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 600;
        }
        
        .status-success {
            background: rgba(33, 200, 95, 0.2);
            color: var(--accent-green);
        }
        
        .status-fail {
            background: rgba(244, 67, 54, 0.2);
            color: #dc2626;
        }
        
        .status-pending {
            background: rgba(255, 193, 7, 0.2);
            color: #b45309;
        }
        
        .empty-journal {
            text-align: center;
            padding: 40px;
            color: #64748b;
            font-style: italic;
        }

        .edit-valve-btn {
            position: absolute;
            bottom: 12px;
            right: 12px;
            width: 32px;
            height: 32px;
            border-radius: 50%;
            border: none;
            background: rgba(33, 200, 95, 0.85);
            color: white;
            font-size: 16px;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            opacity: 0;
            transition: all 0.25s ease;
            z-index: 15;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
            padding: 0;
            line-height: 1;
        }
        .edit-valve-btn:hover {
            background: var(--accent-green, #21C85F);
            transform: scale(1.1);
        }
        .valve-card:hover .edit-valve-btn,
        [id*="valve"]:hover .edit-valve-btn,
        .card:hover .edit-valve-btn,
        .module-card:hover .edit-valve-btn,
        div[class*="valve"]:hover .edit-valve-btn {
            opacity: 1;
        }

        .valve-editor-mini {
            position: fixed;
            background: var(--card-bg, #ffffff);
            padding: 24px 28px;
            border-radius: 32px;
            width: 90%;
            max-width: 320px;
            box-shadow: 0 0 30px var(--glow-soft, rgba(33, 200, 95, 0.3));
            animation: modalFadeIn 0.3s ease;
            z-index: 2147483640;
            font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            color: var(--text-main, #000);
        }
        @keyframes modalFadeIn {
            from { opacity: 0; transform: translateY(16px) scale(0.98); }
            to { opacity: 1; transform: translateY(0) scale(1); }
        }
        .valve-editor-title {
            color: var(--accent-green, #21C85F);
            font-size: 18px;
            font-weight: 600;
            margin-bottom: 18px;
            text-align: center;
        }
        .valve-editor-field {
            margin-bottom: 14px;
        }
        .valve-editor-field label {
            display: block;
            font-size: 13px;
            color: var(--text-main, #333);
            opacity: 0.85;
            margin-bottom: 6px;
        }
        .valve-editor-field input {
            width: 100%;
            padding: 12px 16px;
            border-radius: 24px;
            border: 2px solid var(--accent-green, #21C85F);
            background: transparent;
            font-size: 14px;
            color: var(--accent-green, #21C85F);
            box-sizing: border-box;
            outline: none;
        }
        .valve-editor-actions {
            display: flex;
            gap: 10px;
            margin-top: 20px;
        }
        .valve-editor-actions button {
            flex: 1;
            padding: 12px;
            border-radius: 24px;
            border: none;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s ease;
        }
        .valve-save-btn {
            background: var(--accent-green, #21C85F);
            color: #ffffff;
        }
        .valve-save-btn:hover {
            box-shadow: 0 4px 15px rgba(33, 200, 95, 0.4);
            transform: translateY(-1px);
        }
        .valve-cancel-btn {
            background: rgba(255, 255, 255, 0.15);
            color: var(--text-main, #333);
            border: 1px solid rgba(255, 255, 255, 0.2);
        }
        .valve-cancel-btn:hover {
            background: rgba(255, 255, 255, 0.25);
        }
        .valve-editor-backdrop {
            position: fixed;
            top: 0; left: 0;
            width: 100%; height: 100%;
            background-color: rgba(15, 24, 43, 0.6);
            backdrop-filter: blur(8px);
            -webkit-backdrop-filter: blur(8px);
            z-index: 2147483639;
        }

        .btn-add-task {
            background: var(--accent-green, #21C85F);
            color: white !important;
            border: none;
            padding: 10px 20px;
            border-radius: 20px;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
            display: inline-flex;
            align-items: center;
            gap: 6px;
        }
        .btn-add-task:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 15px rgba(33, 200, 95, 0.4);
        }
        .btn-add-task:active {
            transform: translateY(0);
        }
    </style>
</head>
<body class="theme-dark">
    <div class="top-sticky-wrapper">
        <header class="site-header">
            <a href="profile.html" class="user-pill" aria-label="Профиль">
                <img src="Т.пол.png" alt="" class="user-avatar avatar-dark">
                <img src="Св.пол.png" alt="" class="user-avatar avatar-light">
                <span class="user-text">User_login</span>
            </a>
            <div class="logo-container">
                <img src="Светлая.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="Тёмная.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>
            <label class="theme-switcher" for="themeToggle" aria-label="Сменить тему">
                <img src="тема.png" alt="" class="theme-icon-img">
            </label>
        </header>
        <input type="checkbox" id="themeToggle" class="mode-toggle">
        <nav class="navigation">
            <a href="index.html" class="nav-btn">Главная</a> 
            <a href="myplant.html" class="nav-btn active">Мои растения</a>
            <a href="settings.html" class="nav-btn">Настройки</a>
        </nav>
    </div>

    <div class="page-wrapper">
        <div class="irrigation-container">
            <div class="valves-sidebar" id="valvesSidebar"></div>

            <div class="main-content">
                <div class="content-header">
                    <h1 class="page-title">Конфигурация модуля полива</h1>
                </div>
                
                <div class="tabs">
                    <button class="tab" data-tab="manual">Вручную</button>
                    <button class="tab active" data-tab="config">Расписание</button>
                    <button class="tab" data-tab="journal">Журнал</button>
                </div>

                <div id="manualPanel" style="display: none;">
                    <div style="text-align: center; padding: 30px 20px;">
                        <select class="form-select" id="manual-valve-select" style="margin-bottom: 25px;">
                            <option value="1">Клапан 1</option>
                            <option value="2">Клапан 2</option>
                            <option value="3">Клапан 3</option>
                            <option value="4">Клапан 4</option>
                            <option value="5">Клапан 5</option>
                            <option value="6">Клапан 6</option>
                            <option value="7">Клапан 7</option>
                            <option value="8">Клапан 8</option>
                        </select>
                        <div style="display: flex; gap: 20px; justify-content: center; flex-wrap: wrap; margin-bottom: 30px;">
                            <button id="btn-manual-on" onclick="startWatering()" class="btn-action btn-save" style="min-width: 180px; padding: 16px 24px; font-size: 16px;">
                                Включить клапан
                            </button>
                            <button id="btn-manual-off" onclick="stopWatering()" class="btn-action" style="min-width: 180px; padding: 16px 24px; font-size: 16px; background: rgba(244,67,54,0.15); color: #dc2626;" disabled>
                                Выключить клапан
                            </button>
                        </div>
                        
                        <div style="background: rgba(0,0,0,0.03); border-radius: 16px; padding: 20px; max-width: 400px; margin: 0 auto;">
                            <div style="display: flex; justify-content: space-between; margin-bottom: 12px;">
                                <span style="color: #64748b; font-size: 14px;">Статус:</span>
                                <span id="manual-status" style="font-weight: 600; color: var(--accent-green);">Ожидание</span>
                            </div>
                            <div style="display: flex; justify-content: space-between; margin-bottom: 12px;">
                                <span style="color: #64748b; font-size: 14px;">Время работы:</span>
                                <span id="manual-timer" style="font-weight: 600; font-family: monospace; font-size: 18px;">00:00</span>
                            </div>
                            <div style="display: flex; justify-content: space-between;">
                                <span style="color: #64748b; font-size: 14px;">Объём воды (оценка):</span>
                                <span id="manual-volume" style="font-weight: 600;">—</span>
                            </div>
                        </div>
                        
                        <p style="color: #64748b; font-size: 13px; margin-top: 25px; max-width: 500px; margin-left: auto; margin-right: auto;">
                            При включении клапана в ручном режиме данные автоматически сохранятся в журнал после выключения.
                        </p>
                    </div>
                </div>

                <div id="configPanel">
                    <div style="background:rgba(33,200,95,0.06); border-radius:16px; padding:20px; margin-bottom:25px; border:1px solid rgba(33,200,95,0.2);">
                        <h4 style="margin:0 0 15px 0; color:var(--accent-green);">Настройка новой задачи</h4>
                        
                        <div class="form-group">
                            <label class="form-label">Клапан</label>
                            <select class="form-select" id="input-valve-select">
                                <option value="1">Клапан 1</option>
                                <option value="2">Клапан 2</option>
                                <option value="3">Клапан 3</option>
                                <option value="4">Клапан 4</option>
                                <option value="5">Клапан 5</option>
                                <option value="6">Клапан 6</option>
                                <option value="7">Клапан 7</option>
                                <option value="8">Клапан 8</option>
                            </select>
                        </div>
                        
                        <div class="form-row-2">
                            <div class="form-group">
                                <label class="form-label">Тип расписания</label>
                                <select class="form-select" id="input-schedule-type">
                                    <option value="daily">Ежедневно</option>
                                    <option value="weekly">Еженедельно</option>
                                    <option value="interval">По интервалу</option>
                                    <option value="once">Однократно</option>
                                    <option value="sunrise">На рассвете</option>
                                    <option value="sunset">На закате</option>
                                </select>
                            </div>
                            <div class="form-group">
                                <label class="form-label">Приоритет (1-10)</label>
                                <input type="number" class="form-input" id="input-priority" value="5" min="1" max="10">
                            </div>
                        </div>
                        
                        <div class="form-row">
                            <div class="form-group" id="group-time">
                                <label class="form-label">Время полива</label>
                                <input type="time" class="form-input" id="input-time" value="08:00">
                            </div>
                            <div class="form-group" id="group-days" style="display:none;">
                                <label class="form-label">Дни недели</label>
                                <input type="text" class="form-input" id="input-days" placeholder="1,3,5" 
                                    oninput="this.value=this.value.replace(/[^1-7,]/g,''); if(document.getElementById('input-schedule-type').value==='weekly') generateWeeklyDayFields(this.value);">
                                <div class="form-hint">1=Пн, 7=Вс. Пример: 1,3,5</div>
                            </div>
                            <div class="form-group" id="group-interval" style="display:none;">
                                <label class="form-label">Интервал (мин)</label>
                                <input type="number" class="form-input" id="input-interval" value="60" min="1" max="1440">
                            </div>
                        </div>
                        
                        <div class="form-row-2" style="margin-top:10px;">
                            <div class="form-group">
                                <label class="form-label">Объём воды (мл)</label>
                                <input type="number" class="form-input" id="input-water-volume" value="200" min="10" max="5000" step="10">
                                <div class="form-hint">Сколько мл вылить за один полив</div>
                            </div>
                            <div class="form-group">
                                <label class="form-label">Длительность открытия (сек)</label>
                                <input type="number" class="form-input" id="input-water-duration" value="30" min="1" max="600">
                                <div class="form-hint">На сколько секунд открывать клапан</div>
                            </div>
                        </div>
                        
                        <div class="form-row-2">
                            <div class="form-group">
                                <label class="form-label">Повторений цикла</label>
                                <input type="number" class="form-input" id="input-cycles" value="1" min="1" max="10">
                            </div>
                        </div>
                        
                        <div id="weekly-details-container" style="margin-top:15px; display:none;"></div>
                        
                        <button class="btn-add-task" onclick="addTaskOperation()" style="width:100%; margin-top:10px; padding:14px; font-size:15px;">
                            + Добавить задачу с этими параметрами
                        </button>
                    </div>
                    
                    <h3 class="section-title">Добавленные задачи</h3>
                    <div class="operations-list" id="operations-list"></div>
                </div>

                <div id="journalPanel">
                    <h3 class="section-title">История полива</h3>
                    <div id="journal-content">
                        <table class="journal-table">
                            <thead>
                                <tr>
                                    <th>Дата/Время</th>
                                    <th>Клапан</th>
                                    <th>Тип запуска</th>
                                    <th>Длительность</th>
                                    <th>Объём</th>
                                    <th>Статус</th>
                                </tr>
                            </thead>
                            <tbody id="journal-tbody"></tbody>
                        </table>
                        <div id="journal-empty" class="empty-journal">Журнал пока пуст. Выполните тестовый полив или дождитесь автоматического запуска.</div>
                    </div>
                </div>
            </div>

            <div class="sidebar-right">
                <div class="moisture-panel">
                    <h2 class="panel-title">Датчик влажности</h2>
                    <div class="toggle-row">
                        <span class="toggle-label">Режим влажности</span>
                        <div class="valve-toggle" id="input-sensor-active"></div>
                    </div>
                    <div class="form-hint" style="margin-bottom: 20px;">Автоматический полив по показаниям датчика</div>
                    
                    <div class="form-group">
                        <label class="form-label">Интервал проверки (мин)</label>
                        <select class="form-select" id="input-sensor-interval">
                            <option value="15">15 минут</option>
                            <option value="30">30 минут</option>
                            <option value="60">1 час</option>
                        </select>
                    </div>
                    
                    <div class="form-group">
                        <label class="form-label">Целевая влажность (%)</label>
                        <input type="number" class="form-input" id="input-target-moisture" min="0" max="100">
                    </div>
                    
                    <div class="form-group">
                        <label class="form-label">Макс. длительность полива (сек)</label>
                        <input type="number" class="form-input" id="input-sensor-duration">
                    </div>
                    
                    <div class="form-group">
                        <label class="form-label">Мин. пауза (часы)</label>
                        <input type="number" class="form-input" id="input-min-pause">
                    </div>
                    
                    <div class="form-group">
                        <label class="form-label">Последнее измерение</label>
                        <input type="number" class="form-input" id="input-last-moisture" readonly style="opacity: 0.7;">
                    </div>
                    
                    <div class="action-buttons">
                        <button class="btn-action btn-save" onclick="saveAllData()">Сохранить все</button>
                        <button class="btn-action btn-apply" onclick="applyToAll()">Применить ко всем</button>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <footer class="site-footer">
        <div class="footer-columns">
            <div>
                <h3 class="footer-column-title">Зелёная полка</h3>
                <p class="footer-column-text">Система контроля микроклимата растений соответствует приоритетам техлидерства РФ, IoT и импортозамещению.</p>
            </div>
            <div>
                <h3 class="footer-column-title">Модули</h3>
                <ul class="footer-column-list">
                    <li>Освещения растений</li>
                    <li>Сбор данных о растениях</li>
                    <li>Полив растений</li>
                </ul>
            </div>
            <div>
                <h3 class="footer-column-title">Контакты</h3>
                <ul class="footer-column-list">
                    <li>Почта</li>
                    <li>Наши сети</li>
                </ul>
            </div>
        </div>
        <div class="footer-copyright">
            2026 Зелёная полка
        </div>
    </footer>

    <script>
        // ===== КОНФИГУРАЦИЯ API =====
        const API_BASE = '/api/watering';
        
        let db = {};
        let currentValve = 1;
        let saveTimeout = null;

        // ===== ИНИЦИАЛИЗАЦИЯ =====
        async function init() {
            await loadData();
            renderSidebar();
            switchValve(1);
            setupListeners();
            updateFormLogic();
            await renderJournal();
        }

        // ===== ЗАГРУЗКА ДАННЫХ С СЕРВЕРА =====
        async function loadData() {
            try {
                const res = await fetch(API_BASE + '/valves');
                if (res.ok) {
                    db = await res.json();
                }
            } catch (e) {
                console.error('Ошибка загрузки данных:', e);
            }
        }

        // ===== СОХРАНЕНИЕ ДАННЫХ НА СЕРВЕР =====
        async function saveData() {
            try {
                const valveData = db[currentValve];
                const payload = {
                    id: currentValve,
                    active: valveData.active,
                    max_duration_sec: valveData.max_duration_sec,
                    daily_limit_ml: valveData.daily_limit_ml,
                    moisture_mode: valveData.moisture_mode,
                    plant_name: valveData.plant_name || '',
                    task: valveData.task,
                    sensor: valveData.sensor
                };
                
                await fetch(API_BASE + '/valve', {
                    method: 'PUT',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify(payload)
                });
            } catch (e) {
                console.error('Ошибка сохранения:', e);
            }
        }

        async function saveAllData() {
            collectUI(currentValve);
            await saveData();
            showNotification('Настройки сохранены');
        }

        async function applyToAll() {
            collectUI(currentValve);
            try {
                await fetch(API_BASE + '/apply-all', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({source_valve_id: currentValve})
                });
                await loadData();
                renderSidebar();
                showNotification('Настройки применены ко всем клапанам');
            } catch (e) {
                console.error('Ошибка применения:', e);
            }
        }

        function debounceSave() {
            clearTimeout(saveTimeout);
            saveTimeout = setTimeout(() => {
                collectUI(currentValve);
                saveData();
            }, 800);
        }

        // ===== РЕНДЕРИНГ САЙДБАРА =====
        function renderSidebar() {
            const container = document.getElementById('valvesSidebar');
            if (!container) return;
            container.innerHTML = '';
            
            for (let i = 1; i <= 8; i++) {
                const d = db[i];
                if (!d) continue;
                
                const plant = d.plant_name || '—';
                const statusText = d.active ? (d.moisture_mode ? 'By Moisture' : 'Active') : 'Неактивен';
                const statusClass = d.active ? '' : 'inactive';
                
                container.innerHTML += `
                    <div class="valve-item ${i === currentValve ? 'active' : ''}" data-valve="${i}" onclick="switchValve(${i})">
                        <div class="valve-header">
                            <span class="valve-title"><span class="valve-number">${i}</span>Клапан ${i}</span>
                            <div class="valve-toggle ${d.active ? 'on' : ''}" onclick="handleValveToggle(${i}, event)"></div>
                        </div>
                        <div class="valve-info">Pin: ${d.pin_number}</div>
                        <div class="valve-info">Макс: ${d.max_duration_sec} сек</div>
                        <div class="valve-info">Лимит: ${d.daily_limit_ml > 0 ? d.daily_limit_ml + ' мл' : '—'}</div>
                        <div class="valve-info">Растение: ${plant}</div>
                        <span class="valve-status ${statusClass}">${statusText}</span>
                    </div>`;
            }
        }

        window.handleValveToggle = async function(valveId, event) {
            event?.stopPropagation();
            const toggle = document.querySelector(`.valve-item[data-valve="${valveId}"] .valve-toggle`);
            if (!toggle) return;
            
            const isNowOn = !toggle.classList.contains('on');
            toggle.classList.toggle('on');
            db[valveId].active = isNowOn ? 1 : 0;
            
            await saveData();
            
            if (valveId === currentValve) applyUI(valveId);
            renderSidebar();
        };

        window.switchValve = function(id) {
            collectUI(currentValve);
            currentValve = id;
            renderSidebar();
            applyUI(id);
            updateFormLogic();
        };

        function applyUI(id) {
            const d = db[id]; if (!d) return;
            const t = d.task, s = d.sensor;
            const setVal = (id, val) => { const el = document.getElementById(id); if(el) el.value = val ?? ''; };
            
            setVal('input-valve-select', id);
            setVal('input-schedule-type', t.schedule_type);
            setVal('input-time', t.schedule_time || '');
            setVal('input-days', t.schedule_days || '');
            setVal('input-interval', t.schedule_interval_min || '');
            setVal('input-priority', t.priority);
            setVal('input-cycles', t.cycle_repeat);
            setVal('input-task-duration', t.max_duration_sec);
            setVal('input-max-volume', t.max_volume_ml);

            if (t.schedule_type === 'weekly' && t.weekly_days) {
                const daysInput = document.getElementById('input-days');
                if (daysInput && daysInput.value) {
                    generateWeeklyDayFields(daysInput.value);
                }
            }

            const opsList = document.getElementById('operations-list');
            if (opsList) {
                opsList.innerHTML = '';
                t.operations.forEach((op, idx) => {
                    if (op.type !== 'TASK_SUMMARY') return;
                    
                    const details = op.details || {};
                    const waterDuration = details.limits?.duration || '—';
                    const waterVolume = details.limits?.volume || '—';
                    
                    const content = `
                        <div style="background:linear-gradient(135deg, #28a745 0%, #20c997 100%); color:white; padding:16px; border-radius:12px; margin:8px 0; box-shadow:0 2px 8px rgba(0,0,0,0.15);">
                            <div style="display:flex; justify-content:space-between; align-items:start; margin-bottom:12px;">
                                <h4 style="margin:0; font-size:15px;">${op.label || 'Задача'}</h4>
                                <button class="btn-delete" onclick="deleteOperation(${idx})" 
                                        style="background:rgba(255,255,255,0.2); border:none; color:white; 
                                            border-radius:50%; width:28px; height:28px; cursor:pointer; 
                                            font-size:18px; line-height:1; display:flex; align-items:center; justify-content:center;">
                                    ×
                                </button>
                            </div>
                            
                            <div style="display:grid; grid-template-columns:repeat(2,1fr); gap:10px; font-size:13px;">
                                <div><b>Расписание:</b><br><span style="opacity:0.9;">${details.schedule || '—'}</span></div>
                                <div><b>Приоритет:</b><br><span style="opacity:0.9;">${details.priority || '—'}</span></div>
                                <div><b>Циклы:</b><br><span style="opacity:0.9;">${details.cycles || '—'}</span></div>
                                <div><b>Длит.:</b><br><span style="opacity:0.9;">${waterDuration}</span></div>
                                <div><b>Объём:</b><br><span style="opacity:0.9;">${waterVolume}</span></div>
                                <div><b>Добавлено:</b><br><span style="opacity:0.9;">${op.timestamp || ''}</span></div>
                            </div>
                        </div>`;
                    
                    opsList.insertAdjacentHTML('beforeend', content);
                });
                
                if (t.operations.filter(op => op.type === 'TASK_SUMMARY').length === 0) {
                    opsList.innerHTML = '<div style="text-align:center; padding:30px; color:#64748b; font-style:italic;">Задач пока нет. Добавьте первую задачу выше.</div>';
                }
            }

            const sensorToggle = document.getElementById('input-sensor-active');
            if (sensorToggle) s.active ? sensorToggle.classList.add('on') : sensorToggle.classList.remove('on');
            setVal('input-sensor-interval', s.check_interval_min);
            setVal('input-target-moisture', s.target_moisture_percent);
            setVal('input-sensor-duration', s.max_watering_duration_sec);
            setVal('input-min-pause', s.min_pause_hours);
            setVal('input-last-moisture', s.last_moisture_percent);
        }

        window.addTaskOperation = async function() {
            const getVal = (id) => document.getElementById(id)?.value || '';
            const getNum = (id, def) => { const v = parseInt(getVal(id)); return isNaN(v) ? def : v; };
            
            const valve = getVal('input-valve-select') || currentValve;
            const scheduleType = getVal('input-schedule-type') || 'daily';
            const scheduleTime = getVal('input-time') || '08:00';
            const scheduleDays = getVal('input-days') || '';
            const interval = getVal('input-interval') || '60';
            const priority = getNum('input-priority', 5);
            const cycles = getNum('input-cycles', 1);
            const maxDuration = getNum('input-task-duration', 600);
            const maxVolume = getNum('input-max-volume', 1000);
            const waterVolume = getNum('input-water-volume', 200);
            const waterDuration = getNum('input-water-duration', 30);

            let scheduleDesc = '';
            switch(scheduleType) {
                case 'daily': scheduleDesc = `Ежедневно в ${scheduleTime}`; break;
                case 'weekly': scheduleDesc = `Еженедельно: дни ${scheduleDays || '—'}`; break;
                case 'interval': scheduleDesc = `Каждые ${interval} мин`; break;
                case 'once': scheduleDesc = `Однократно: ${scheduleTime}`; break;
                case 'sunrise': scheduleDesc = 'На рассвете (авто)'; break;
                case 'sunset': scheduleDesc = 'На закате (авто)'; break;
                default: scheduleDesc = scheduleType;
            }
            
            const task = {
                valve_id: parseInt(valve),
                schedule_type: scheduleType,
                schedule_time: scheduleTime,
                schedule_days: scheduleDays,
                schedule_interval_min: parseInt(interval),
                priority: priority,
                cycle_repeat: cycles,
                max_duration_sec: maxDuration,
                max_volume_ml: maxVolume,
                weekly_days: {},
                operations: [{
                    type: 'TASK_SUMMARY',
                    label: `Задача для Клапана ${valve}`,
                    details: {
                        schedule: scheduleDesc,
                        priority: priority,
                        cycles: cycles,
                        limits: { 
                            duration: waterDuration + ' сек',
                            volume: waterVolume + ' мл'
                        }
                    },
                    timestamp: new Date().toLocaleString('ru-RU'),
                    _params: {
                        valve: parseInt(valve),
                        schedule_type: scheduleType,
                        schedule_time: scheduleTime,
                        schedule_days: scheduleDays,
                        schedule_interval_min: parseInt(interval),
                        duration_sec: waterDuration,
                        volume_ml: waterVolume,
                        priority: priority,
                        cycles: cycles,
                        max_duration_sec: maxDuration,
                        max_volume_ml: maxVolume
                    }
                }]
            };
            
            try {
                const res = await fetch(API_BASE + '/task', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify(task)
                });
                
                if (res.ok) {
                    await loadData();
                    applyUI(currentValve);
                    showNotification(`Задача добавлена: ${waterVolume} мл / ${waterDuration} сек`);
                }
            } catch (e) {
                console.error('Ошибка добавления задачи:', e);
            }
        };

        window.deleteOperation = async function(idx) {
            if(!confirm('Удалить задачу?')) return;
            
            // Получаем ID задачи из БД
            const task = db[currentValve].task;
            if (task.id) {
                try {
                    await fetch(API_BASE + '/task?id=' + task.id, {method: 'DELETE'});
                    await loadData();
                    applyUI(currentValve);
                    showNotification('Задача удалена');
                } catch (e) {
                    console.error('Ошибка удаления:', e);
                }
            }
        };

        window.updateFormLogic = function() {
            const type = document.getElementById('input-schedule-type')?.value;
            if (!type) return;
            
            const timeGroup = document.getElementById('group-time');
            const daysGroup = document.getElementById('group-days');
            const intervalGroup = document.getElementById('group-interval');
            const weeklyContainer = document.getElementById('weekly-details-container');
            
            const toggle = (el, show) => {
                if (!el) return;
                el.style.display = show ? 'block' : 'none';
                el.querySelectorAll('input')?.forEach(inp => inp.disabled = !show);
            };
            
            toggle(timeGroup, false);
            toggle(daysGroup, false);
            toggle(intervalGroup, false);
            if (weeklyContainer) { 
                weeklyContainer.style.display = 'none'; 
                weeklyContainer.innerHTML = ''; 
            }
            
            switch(type) {
                case 'daily':
                case 'once':
                    toggle(timeGroup, true);
                    break;
                case 'weekly':
                    toggle(timeGroup, true);
                    toggle(daysGroup, true);
                    if (weeklyContainer) {
                        const daysInput = document.getElementById('input-days');
                        if (daysInput?.value.trim()) {
                            generateWeeklyDayFields(daysInput.value);
                        } else {
                            weeklyContainer.style.display = 'block';
                            weeklyContainer.innerHTML = '<div style="padding:10px; color:#64748b; font-style:italic;">Введите дни недели выше (например: 1,3,5)</div>';
                        }
                    }
                    break;
                case 'interval':
                    toggle(intervalGroup, true);
                    break;
                case 'sunrise':
                case 'sunset':
                    showNotification('Время определяется автоматически');
                    break;
            }
        };

        function generateWeeklyDayFields(daysStr) {
            const container = document.getElementById('weekly-details-container');
            if (!container) return;
            
            const days = daysStr.split(',').map(d=>parseInt(d.trim())).filter(d=>d>=1&&d<=7);
            if (days.length === 0) { 
                container.style.display = 'none'; 
                container.innerHTML = '';
                return; 
            }
        
            container.style.display = 'block';
            const WEEKDAYS = {1:'Пн',2:'Вт',3:'Ср',4:'Чт',5:'Пт',6:'Сб',7:'Вс'};
            
            let html = `<div style="background:rgba(33,200,95,0.1); padding:15px; border-radius:12px; margin:15px 0;">
                <strong style="color:var(--accent-green); display:block; margin-bottom:12px;">
                    Индивидуальные настройки для каждого дня:
                </strong>
            </div>`;
            
            days.forEach(dayId => {
                const existingConfig = db[currentValve]?.task?.weekly_days?.[dayId] || {};
                const defaultTime = existingConfig.time || '08:00';
                const defaultVolume = existingConfig.volume || 200;
                const defaultDuration = existingConfig.duration || 30;
                
                html += `
                <div class="weekly-day-row" data-day="${dayId}" style="
                    display:grid; grid-template-columns: 70px 1fr 1fr 1fr; gap:10px;
                    align-items:end; margin:10px 0; padding:15px; background:var(--card-bg);
                    border-radius:12px; border:2px solid rgba(33,200,95,0.3);
                    box-shadow:0 2px 8px rgba(0,0,0,0.08);
                ">
                    <div style="font-weight:700; color:var(--accent-green); font-size:15px;">${WEEKDAYS[dayId]}</div>
                    <div>
                        <label style="font-size:11px; opacity:0.8; display:block; margin-bottom:5px; font-weight:600;">Время</label>
                        <input type="time" class="wd-time" value="${defaultTime}" style="width:100%; padding:8px; border-radius:8px; border:1px solid rgba(0,0,0,0.15); font-size:14px;">
                    </div>
                    <div>
                        <label style="font-size:11px; opacity:0.8; display:block; margin-bottom:5px; font-weight:600;">Объём (мл)</label>
                        <input type="number" class="wd-vol" value="${defaultVolume}" min="10" max="5000" step="10" style="width:100%; padding:8px; border-radius:8px; border:1px solid rgba(0,0,0,0.15); font-size:14px;">
                    </div>
                    <div>
                        <label style="font-size:11px; opacity:0.8; display:block; margin-bottom:5px; font-weight:600;">Длит. (сек)</label>
                        <input type="number" class="wd-dur" value="${defaultDuration}" min="1" max="600" style="width:100%; padding:8px; border-radius:8px; border:1px solid rgba(0,0,0,0.15); font-size:14px;">
                    </div>
                </div>`;
            });
            
            container.innerHTML = html;
            
            container.querySelectorAll('.wd-time, .wd-vol, .wd-dur').forEach(inp => {
                inp.onchange = saveWeeklyConfig;
                inp.oninput = debounce(() => saveWeeklyConfig(), 500);
            });
        }

        function debounce(fn, ms) { let t; return (...a) => { clearTimeout(t); t = setTimeout(() => fn.apply(this,a), ms); }; }

        function saveWeeklyConfig() {
            const container = document.getElementById('weekly-details-container');
            if (!container || !db[currentValve]) return;
            
            if (!db[currentValve].task.weekly_days) {
                db[currentValve].task.weekly_days = {};
            }
            
            container.querySelectorAll('.weekly-day-row').forEach(row => {
                const dayId = parseInt(row.dataset.day);
                db[currentValve].task.weekly_days[dayId] = {
                    time: row.querySelector('.wd-time')?.value || '08:00',
                    volume: parseInt(row.querySelector('.wd-vol')?.value) || 200,
                    duration: parseInt(row.querySelector('.wd-dur')?.value) || 60
                };
            });
            
            debounceSave();
        }

        async function renderJournal(filterValve = null) {
            try {
                const url = filterValve ? `${API_BASE}/log?valve_id=${filterValve}` : `${API_BASE}/log`;
                const res = await fetch(url);
                const logs = await res.json();
                
                const tbody = document.getElementById('journal-tbody');
                const emptyMsg = document.getElementById('journal-empty');
                if (!tbody) return;
                
                tbody.innerHTML = '';
                if(logs.length === 0) { if(emptyMsg) emptyMsg.style.display = 'block'; return; }
                if(emptyMsg) emptyMsg.style.display = 'none';
                
                logs.forEach(log => {
                    const statusClass = log.status==='completed'?'status-success':(log.status==='failed'?'status-fail':'status-pending');
                    const typeText = {schedule:'Расписание', sensor:'Датчик', manual:'Вручную'}[log.type]||log.type;
                    tbody.innerHTML += `<tr><td>${new Date(log.ts * 1000).toLocaleString('ru-RU')}</td><td><b>Клапан ${log.valve}</b></td><td>${typeText}</td><td>${log.duration} сек</td><td>${log.volume} мл</td><td><span class="status-badge ${statusClass}">${log.status==='completed'?'Успешно':log.status==='failed'?'Провалено':'В ожидании'}</span></td></tr>`;
                });
            } catch (e) {
                console.error('Ошибка загрузки журнала:', e);
            }
        }

        window.switchTab = function(tabName) {
            document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
            document.querySelector(`.tab[data-tab="${tabName}"]`)?.classList.add('active');
            document.getElementById('configPanel').style.display = tabName==='config'?'block':'none';
            document.getElementById('journalPanel').style.display = tabName==='journal'?'block':'none';
            document.getElementById('manualPanel').style.display = tabName==='manual'?'block':'none';
            if(tabName==='journal') { renderJournal(); }
        };

        function collectUI(id) {
            const d = db[id]; if(!d) return;
            const toggle = document.querySelector(`.valve-item[data-valve="${id}"] .valve-toggle`);
            if(toggle) d.active = toggle.classList.contains('on')?1:0;
            const t = d.task, s = d.sensor;
            const getVal = (id)=>document.getElementById(id)?.value;
            const getNum = (id,def)=>{const v=parseInt(getVal(id)); return isNaN(v)?def:v;};
            t.schedule_type = getVal('input-schedule-type')||'daily';
            t.priority = getNum('input-priority',5); t.cycle_repeat = getNum('input-cycles',1);
            t.max_duration_sec = getNum('input-task-duration',600); t.max_volume_ml = getNum('input-max-volume',1000);
            const isDisabled = (id)=>document.getElementById(id)?.disabled;
            t.schedule_time = isDisabled('input-time')?null:getVal('input-time');
            t.schedule_days = isDisabled('input-days')?'':getVal('input-days');
            t.schedule_interval_min = isDisabled('input-interval')?0:getNum('input-interval',0);
            const sensorToggle = document.getElementById('input-sensor-active');
            s.active = sensorToggle?.classList.contains('on')?1:0;
            s.check_interval_min = getNum('input-sensor-interval',30);
            s.target_moisture_percent = getNum('input-target-moisture',65);
            s.max_watering_duration_sec = getNum('input-sensor-duration',180);
            s.min_pause_hours = getNum('input-min-pause',2);
            s.last_moisture_percent = getNum('input-last-moisture',0);
        }

        function setupListeners() {
            const sched = document.getElementById('input-schedule-type'); if(sched) sched.onchange = ()=>{updateFormLogic(); debounceSave();};
            document.querySelectorAll('.tab').forEach(tab=>tab.onclick=function(){switchTab(this.dataset.tab);});
            const sensTog = document.getElementById('input-sensor-active'); if(sensTog) sensTog.onclick=function(){this.classList.toggle('on'); debounceSave();};
            const valveSel = document.getElementById('input-valve-select'); if(valveSel) valveSel.onchange=function(){switchValve(+this.value);};
            document.querySelectorAll('#configPanel input,#configPanel select,#configPanel textarea').forEach(el=>{el.onchange=debounceSave; el.oninput=debounceSave;});
            setTimeout(updateFormLogic, 100);
        }

        function showNotification(msg){
            const ex=document.getElementById('gs-notify'); if(ex)ex.remove(); 
            const n=document.createElement('div'); n.id='gs-notify'; 
            n.style.cssText='position:fixed;top:20px;right:20px;background:var(--accent-green,#28a745);color:#fff;padding:12px 24px;border-radius:12px;z-index:9999;box-shadow:0 4px 12px rgba(0,0,0,.2);font-family:system-ui,sans-serif;'; 
            n.textContent=msg; document.body.appendChild(n); 
            setTimeout(()=>{n.style.opacity='0'; n.style.transition='opacity .3s'; setTimeout(()=>n.remove(),300);},2500);
        }

        // ===== РУЧНОЕ УПРАВЛЕНИЕ =====
        let manualState = { isActive: false, startTime: null, valveId: null, timerId: null, flowRate: 8.33 };
        
        function fmt(sec) {
            const m = Math.floor(sec/60).toString().padStart(2,'0');
            const s = (sec%60).toString().padStart(2,'0');
            return `${m}:${s}`;
        }

        window.startWatering = async function() {
            if (manualState.isActive) return;
            const sel = document.getElementById('manual-valve-select');
            if (!sel) return;
            
            manualState.valveId = parseInt(sel.value);
            manualState.startTime = Date.now();
            manualState.isActive = true;
            
            const toggle = document.querySelector(`.valve-item[data-valve="${manualState.valveId}"] .valve-toggle`);
            if (toggle) toggle.classList.add('on');
            
            manualState.timerId = setInterval(() => {
                const elapsed = Math.floor((Date.now() - manualState.startTime) / 1000);
                const tm = document.getElementById('manual-timer');
                const vl = document.getElementById('manual-volume');
                if (tm) tm.textContent = fmt(elapsed);
                if (vl) vl.textContent = Math.round(elapsed * manualState.flowRate) + ' мл';
            }, 1000);
            
            document.getElementById('manual-status').textContent = 'Работает';
            document.getElementById('manual-status').style.color = '#22c55e';
            document.getElementById('btn-manual-on').disabled = true;
            document.getElementById('btn-manual-off').disabled = false;
        };

        window.stopWatering = async function() {
            if (!manualState.isActive) return;
            
            clearInterval(manualState.timerId);
            const duration = Math.floor((Date.now() - manualState.startTime) / 1000);
            const volume = Math.round(duration * manualState.flowRate);
            
            manualState.isActive = false;
            
            const toggle = document.querySelector(`.valve-item[data-valve="${manualState.valveId}"] .valve-toggle`);
            if (toggle) toggle.classList.remove('on');
            
            // Отправляем на сервер
            try {
                await fetch(API_BASE + '/manual', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({
                        valve_id: manualState.valveId,
                        duration: duration,
                        volume: volume
                    })
                });
            } catch (e) {
                console.error('Ошибка сохранения ручного полива:', e);
            }
            
            document.getElementById('manual-status').textContent = 'Ожидание';
            document.getElementById('manual-status').style.color = 'var(--accent-green)';
            document.getElementById('manual-timer').textContent = '00:00';
            document.getElementById('manual-volume').textContent = '—';
            document.getElementById('btn-manual-on').disabled = false;
            document.getElementById('btn-manual-off').disabled = true;
            
            await renderJournal();
            alert(`Клапан ${manualState.valveId}: ${fmt(duration)}, ~${volume} мл — записано в журнал`);
        };

        document.addEventListener('DOMContentLoaded', () => {
            init();
        });
    </script>
</body>
</html>
)rawliteral";

// ==================== ВЕБ-ОБРАБОТЧИКИ ====================
void handleWatering() {
  server.send(200, "text/html", WATERING_HTML);
}
// Универсальный обработчик статических файлов с SD-карты
void handleStaticImage() {
  String uri = server.uri();
  int qMark = uri.indexOf('?');
  if (qMark > 0) uri = uri.substring(0, qMark);
  
  Serial.printf("📁 Static request: %s\n", uri.c_str());
  
  // URL-decode для кириллицы
  String path = uri;
  path.replace("%20", " ");
  
  if (!path.startsWith("/")) path = "/" + path;
  
  // Защита от path traversal
  if (path.indexOf("..") >= 0) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  
  // Проверяем существование файла
  if (!SD.exists(path)) {
    Serial.printf("❌ File not found: %s\n", path.c_str());
    server.send(404, "text/plain", "File not found: " + path);
    return;
  }
  
  // Определяем MIME тип
  String contentType = "application/octet-stream";
  if (path.endsWith(".png"))       contentType = "image/png";
  else if (path.endsWith(".jpg") || path.endsWith(".jpeg")) contentType = "image/jpeg";
  else if (path.endsWith(".gif"))  contentType = "image/gif";
  else if (path.endsWith(".svg"))  contentType = "image/svg+xml";
  else if (path.endsWith(".css"))  contentType = "text/css";
  else if (path.endsWith(".js"))   contentType = "application/javascript";
  else if (path.endsWith(".html")) contentType = "text/html";
  else if (path.endsWith(".json")) contentType = "application/json";
  else if (path.endsWith(".ico"))  contentType = "image/x-icon";
  
  Serial.printf("✅ Sending: %s [%s]\n", path.c_str(), contentType.c_str());
  
  // Кэширование для изображений (1 час)
  if (path.endsWith(".png") || path.endsWith(".jpg") || path.endsWith(".jpeg")) {
    server.sendHeader("Cache-Control", "public, max-age=3600");
  }
  
  server.sendHeader("Content-Type", contentType);
  File file = SD.open(path, FILE_READ);
  if (file) {
    server.streamFile(file, contentType);
    file.close();
  } else {
    server.send(500, "text/plain", "Failed to open file");
  }
}

void handleGetAllValves() {
  String data = getAllValvesData();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", data);
}

void handleNotFound() {
  String uri = server.uri();
  int qMark = uri.indexOf('?');
  if (qMark > 0) uri = uri.substring(0, qMark);
  
  Serial.printf("🔍 404 handler: %s\n", uri.c_str());
  
  // === Проверяем: может это статический файл? ===
  bool isStaticFile = false;
  
  // 1. По пути
  if (uri.startsWith("/image/") || uri.startsWith("/avatar/") || 
      uri.startsWith("/plant/") || uri.startsWith("/www/")) {
    isStaticFile = true;
  }
  
  // 2. По расширению (включая CSS и JS!)
  if (uri.endsWith(".png") || uri.endsWith(".jpg") || uri.endsWith(".jpeg") ||
      uri.endsWith(".gif") || uri.endsWith(".svg") || uri.endsWith(".css") ||
      uri.endsWith(".js") || uri.endsWith(".ico") || uri.endsWith(".html") ||
      uri.endsWith(".json") || uri.endsWith(".webp") || uri.endsWith(".bmp")) {
    isStaticFile = true;
  }
  
  // 3. Файлы из HTML страницы полива (в корне SD)
  if (uri == "/myplant-style.css" || 
      uri.endsWith(".png") || uri.endsWith(".jpg")) {
    isStaticFile = true;
  }
  
  // === Если это статический файл — отдаём его ===
  if (isStaticFile) {
    handleStaticImage();
    return;
  }
  
  // === Иначе — 404 ===
  Serial.printf("❌ 404 Not Found: %s\n", uri.c_str());
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>404</title>";
  html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#0F182B;color:#21C85F;}";
  html += "h1{font-size:48px;}p{font-size:18px;}a{color:#21C85F;}</style>";
  html += "</head><body>";
  html += "<h1>404</h1>";
  html += "<p>Страница не найдена</p>";
  html += "<p>Запрошенный URL: " + uri + "</p>";
  html += "<p><a href='/watering'>Перейти к модулю полива</a></p>";
  html += "</body></html>";
  server.send(404, "text/html", html);
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(800);
  Serial.println("\nESP32-C3 Watering Module Starting...");
  
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
  
  // Инициализация БД
  initDB();
  createSDDirectories();
  
  if (!db) {
    Serial.println("Критическая ошибка: БД не инициализирована!");
    while (1) {
      delay(1000);
      Serial.println("Требуется SD-карта для работы!");
    }
  }
  
  // Инициализация пинов клапанов
  for (int i = 0; i < 8; i++) {
    pinMode(valvePins[i], OUTPUT);
    digitalWrite(valvePins[i], LOW);
  }
  
  // Инициализация пинов датчиков
  for (int i = 0; i < 8; i++) {
    pinMode(sensorPins[i], INPUT);
  }
  // === В начале setup(), ПЕРЕД server.on("/watering", ...) ===

// Редирект с корня на страницу полива
server.on("/", HTTP_GET, []() {
  server.sendHeader("Location", "/watering");
  server.send(302, "text/plain", "Redirecting to /watering");
});

// Обработчик для favicon.ico (чтобы не было 404 в логах)
server.on("/favicon.ico", HTTP_GET, []() {
  if (SD.exists("/favicon.ico")) {
    File f = SD.open("/favicon.ico", FILE_READ);
    server.streamFile(f, "image/x-icon");
    f.close();
  } else {
    server.send(204, "text/plain", "");
  }
});
  // Настройка веб-сервера
  server.on("/watering", handleWatering);
  server.on("/api/watering/valves", HTTP_GET, handleGetAllValves);
  server.on("/api/watering/valve", HTTP_PUT, handleUpdateValve);
  server.on("/api/watering/task", HTTP_POST, handleAddTask);
  server.on("/api/watering/task", HTTP_DELETE, handleDeleteTask);
  server.on("/api/watering/log", HTTP_GET, handleGetWateringLog);
  server.on("/api/watering/manual", HTTP_POST, handleManualWatering);
  server.on("/api/watering/apply-all", HTTP_POST, handleApplyToAll);
  
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started");
  
  // Настройка OTA
  ArduinoOTA.setHostname("WateringC3");
  ArduinoOTA.onStart([]() {
    Serial.println("OTA update started");
  });
  ArduinoOTA.begin();
  
  Serial.println("\nSystem ready! Access via http://" + 
    (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "192.168.10.1") + "/watering");
}

// ====================== LOOP ======================
void loop() {
  server.handleClient();
  ArduinoOTA.handle();
}
