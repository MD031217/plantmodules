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
#define STA_SSID "linksys"
#define STA_PASS ""
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

/*/ === НОВОЕ: Таблицы пользователей и сессий ===
#define SQL_CREATE_USERS "CREATE TABLE IF NOT EXISTS Users (" \
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " \
    "username TEXT UNIQUE NOT NULL, " \
    "password TEXT NOT NULL, " \
    "email TEXT UNIQUE NOT NULL, " \
    "gender TEXT DEFAULT 'female', " \
    "timezone TEXT DEFAULT '(UTC+05:00) Asia/Yekaterinburg', " \
    "avatar TEXT DEFAULT '', " \
    "created_at INTEGER NOT NULL);"

#define SQL_CREATE_SESSIONS "CREATE TABLE IF NOT EXISTS Sessions (" \
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " \
    "token TEXT UNIQUE NOT NULL, " \
    "user_id INTEGER NOT NULL, " \
    "created_at INTEGER NOT NULL, " \
    "FOREIGN KEY(user_id) REFERENCES Users(id));"*/

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

// === НОВОЕ: Глобальные переменные для загрузки файлов ===
int pendingUserId = 0;
String pendingOldAvatarPath = "";
String pendingNewAvatarPath = "";

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


// Добавь эту функцию где-то после initSDCard()
void createSDDirectories() {
  Serial.println("=== Создание папок на SD-карте ===");
  
  const char* folders[] = {"/sd/image", "/sd/avatars", "/sd/plant"};
  
  for (int i = 0; i < 3; i++) {
    if (!SD.exists(folders[i])) {
      if (SD.mkdir(folders[i])) {
        Serial.printf("Создана папка: %s\n", folders[i]);
      } else {
        Serial.printf("Не удалось создать папку: %s\n", folders[i]);
      }
    } else {
      Serial.printf("Папка уже существует: %s\n", folders[i]);
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
  
    // === Создание/обновление таблицы Users (с проверкой колонок) ===
    const char* createUsers = 
    "CREATE TABLE IF NOT EXISTS Users ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "username TEXT UNIQUE NOT NULL, "
    "password TEXT NOT NULL, "
    "email TEXT UNIQUE NOT NULL, "
    "gender TEXT DEFAULT 'female', "
    "timezone TEXT DEFAULT '(UTC+05:00) Asia/Yekaterinburg', "
    "avatar TEXT DEFAULT '', "
    "created_at INTEGER NOT NULL);";

  rc = sqlite3_exec(db, createUsers, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания Users: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }

  // Добавляем колонку username, если её нет (на случай старой таблицы)
  sqlite3_exec(db, "ALTER TABLE Users ADD COLUMN username TEXT UNIQUE;", NULL, NULL, NULL);
  sqlite3_exec(db, "ALTER TABLE Users ADD COLUMN avatar TEXT DEFAULT '';", NULL, NULL, NULL);

    // Таблица Sessions
    const char* sqlSessions = 
    "CREATE TABLE IF NOT EXISTS Sessions ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "token TEXT UNIQUE NOT NULL, "
    "user_id INTEGER NOT NULL, "
    "created_at INTEGER NOT NULL, "
    "FOREIGN KEY(user_id) REFERENCES Users(id));";

  sqlite3_exec(db, sqlSessions, NULL, NULL, &zErrMsg);
  if (zErrMsg) {
    Serial.printf("Sessions error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
    zErrMsg = nullptr;
  }

    Serial.println("Все таблицы проверены и готовы");

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

// === НОВОЕ: Генерация токена (24 символа) ===
String generateToken() {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    String token;
    token.reserve(24);
    for (int i = 0; i < 24; i++) {
        token += chars[random(62)];
    }
    return token;
}

// === НОВОЕ: Регистрация пользователя ===
bool registerUser(const String& username, const String& password, const String& email, 
                  const String& gender = "female", const String& timezone = "(UTC+05:00) Asia/Yekaterinburg") {
    if (!db) return false;
    
    const char* sql = "INSERT INTO Users (username, password, email, gender, timezone, created_at) VALUES (?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Serial.printf("Prepare register error: %s\n", sqlite3_errmsg(db));
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, gender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, timezone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, time(nullptr));
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    
    if (!success) {
        Serial.printf("Register error: %s\n", sqlite3_errmsg(db));
    }
    return success;
}

// === НОВОЕ: Авторизация пользователя ===
String loginUser(const String& username, const String& password) {
    if (!db) return "{\"success\":false}";
    
    const char* sql = "SELECT id, username FROM Users WHERE username=? AND password=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return "{\"success\":false}";
    }
    
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int userId = sqlite3_column_int(stmt, 0);
        const char* uname = (const char*)sqlite3_column_text(stmt, 1);
        String token = generateToken();
        
        // Сохраняем сессию
        const char* insSql = "INSERT INTO Sessions (token, user_id, created_at) VALUES (?,?,?);";
        sqlite3_stmt* insStmt = nullptr;
        if (sqlite3_prepare_v2(db, insSql, -1, &insStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(insStmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insStmt, 2, userId);
            sqlite3_bind_int(insStmt, 3, time(nullptr));
            sqlite3_step(insStmt);
            sqlite3_finalize(insStmt);
        }
        
        sqlite3_finalize(stmt);
        
        DynamicJsonDocument doc(256);
        doc["success"] = true;
        doc["token"] = token;
        doc["username"] = uname ? uname : "";
        String out;
        serializeJson(doc, out);
        return out;
    }
    
    sqlite3_finalize(stmt);
    return "{\"success\":false,\"message\":\"Invalid credentials\"}";
}

// === НОВОЕ: Получение ID пользователя по токену ===
int getUserIdByToken(const String& token) {
    if (!db || token.length() != 24) return 0;
    
    const char* sql = "SELECT user_id FROM Sessions WHERE token=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int userId = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return userId;
    }
    
    sqlite3_finalize(stmt);
    return 0;
}

// === НОВОЕ: Получение профиля по токену ===
String getProfileByToken(const String& token) {
    int userId = getUserIdByToken(token);
    if (!userId) return "{}";
    
    const char* sql = "SELECT username, email, gender, timezone, avatar FROM Users WHERE id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "{}";
    
    sqlite3_bind_int(stmt, 1, userId);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        DynamicJsonDocument doc(512);
        const char* val;
        
        val = (const char*)sqlite3_column_text(stmt, 0);
        doc["username"] = val ? val : "";
        val = (const char*)sqlite3_column_text(stmt, 1);
        doc["email"] = val ? val : "";
        val = (const char*)sqlite3_column_text(stmt, 2);
        doc["gender"] = val ? val : "female";
        val = (const char*)sqlite3_column_text(stmt, 3);
        doc["timezone"] = val ? val : "";
        val = (const char*)sqlite3_column_text(stmt, 4);
        doc["avatar"] = val ? val : "";
        
        String out;
        serializeJson(doc, out);
        sqlite3_finalize(stmt);
        return out;
    }
    
    sqlite3_finalize(stmt);
    return "{}";
}

// === НОВОЕ: Обновление профиля по токену ===
bool updateProfileByToken(const String& token, const String& username, const String& email,
                          const String& gender, const String& timezone, const String& avatar = "") {
    int userId = getUserIdByToken(token);
    if (!userId) return false;
    
    String sql = "UPDATE Users SET username=?, email=?, gender=?, timezone=?";
    if (avatar.length() > 0) sql += ", avatar=?";
    sql += " WHERE id=?;";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    
    int idx = 1;
    sqlite3_bind_text(stmt, idx++, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, gender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, timezone.c_str(), -1, SQLITE_TRANSIENT);
    if (avatar.length() > 0) {
        sqlite3_bind_text(stmt, idx++, avatar.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, idx, userId);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

// === НОВОЕ: Получение пути к аватару ===
String getCurrentAvatarPath(int userId) {
    if (!db || !userId) return "";
    
    const char* sql = "SELECT avatar FROM Users WHERE id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    
    sqlite3_bind_int(stmt, 1, userId);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* path = (const char*)sqlite3_column_text(stmt, 0);
        String result = path ? String(path) : "";
        sqlite3_finalize(stmt);
        return result;
    }
    
    sqlite3_finalize(stmt);
    return "";
}

// === НОВОЕ: Обновление аватара в БД ===
bool updateUserAvatarInDB(const String& token, const String& relPath) {
    int userId = getUserIdByToken(token);
    if (!userId) return false;
    
    const char* sql = "UPDATE Users SET avatar=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    
    sqlite3_bind_text(stmt, 1, relPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, userId);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

// === НОВОЕ: Удаление сессии (логаут) ===
void handleApiLogout(); // forward declaration

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
            // НОВОЕ: Отправка на сервер для проверки
            fetch('/api/login', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'login=' + encodeURIComponent(login) + '&password=' + encodeURIComponent(password)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    // === НОВОЕ: Сохраняем ТОЛЬКО токен ===
                    localStorage.setItem('auth_token', data.token);
                    window.location.href = '/profile';
                } else {
                    alert(data.message || 'Неверный логин или пароль');
                }
            })
            .catch(() => {
                alert('Ошибка соединения с сервером');
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
                    // === НОВОЕ: После регистрации сразу логинимся для получения токена ===
                    return fetch('/api/login', {
                        method: 'POST',
                        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                        body: 'login=' + encodeURIComponent(login) + '&password=' + encodeURIComponent(pass)
                    });
                } else {
                    throw new Error(data.message || 'Ошибка регистрации');
                }
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    localStorage.setItem('auth_token', data.token);
                    window.location.href = '/profile';
                }
            })
            .catch(err => {
                alert(err.message || 'Ошибка');
            });
        });
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
                <img src="/image/Т.пол.png" alt="Avatar Dark" class="user-avatar avatar-dark" id="header-avatar-dark">
                <img src="/image/Св.пол.png" alt="Avatar Light" class="user-avatar avatar-light" id="header-avatar-light">
                <span class="user-text" id="header-username">User_login</span>
            </a>

            <div class="logo-container">
                <img src="/image/Светлая.png" alt="Зелёная полка (Dark)" class="main-logo logo-dark">
                <img src="/image/Тёмная.png" alt="Зелёная полка (Light)" class="main-logo logo-light">
            </div>

            <div class="theme-switcher" onclick="toggleTheme()">
                <input type="checkbox" id="theme-toggle" class="mode-toggle">
                <label for="theme-toggle">
                    <img src="/image/тема.png" alt="Тема" class="theme-icon-img">
                </label>
            </div>
        </header>

        <nav class="navigation">
            <a href="/index" class="nav-btn">Главная</a>
            <a href="/plant" class="nav-btn">Мои растения</a>
            <a href="/settings" class="nav-btn">Настройки</a>
        </nav>
    </div>

    <main class="page-wrapper">
        <div class="card">
            <a href="/login" class="add-link" style="text-decoration: none;">
                <div class="logout-icon" onclick="logout()" title="Выйти">
                    <img src="/image/ненаведдверьтем.png" alt="Выйти" class="logout-img logout-dark-default">
                    <img src="/image/наведдверьтем.png" alt="Выйти при наведении" class="logout-img logout-dark-hover">
                    <img src="/image/ненаведдверьсвет.png" alt="Выйти" class="logout-img logout-light-default">
                    <img src="/image/наведдверьсвет.png" alt="Выйти при наведении" class="logout-img logout-light-hover">
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
        // === ИСПРАВЛЕННЫЙ JavaScript ===
        const resolveAvatarPath = (path) => {
            if (!path) return '';
            if (path.startsWith('http')) return path;
            return '/api/avatar/file?file=' + encodeURIComponent(path);
        };

        const loadProfileFromServer = () => {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                window.location.href = '/login';
                return Promise.resolve(null);
            }
            return fetch('/api/profile', {
                headers: {'Authorization': 'Bearer ' + token}
            })
            .then(res => {
                if (res.status === 401) {
                    localStorage.removeItem('auth_token');
                    window.location.href = '/login';
                    return null;
                }
                return res.json();
            })
            .catch(e => {
                console.error('Profile load error:', e);
                return null;
            });
        };

        const toggleTheme = () => {
            document.body.classList.toggle('theme-light');
            document.body.classList.toggle('theme-dark');
            const theme = document.body.classList.contains('theme-dark') ? 'dark' : 'light';
            localStorage.setItem('theme', theme);
        };

        const logout = () => {
            if (!confirm('Вы уверены, что хотите выйти?')) return;
            
            const token = localStorage.getItem('auth_token');
            if (token) {
                fetch('/api/logout', {
                    method: 'POST',
                    headers: {'Authorization': 'Bearer ' + token}
                }).catch(() => {});
            }
            localStorage.removeItem('auth_token');
            window.location.href = '/login';
        };

        const deleteAccount = () => {
            if (confirm('Вы уверены, что хотите удалить аккаунт? Это действие нельзя отменить.')) {
                localStorage.removeItem('userData');
                window.location.href = '/register';
            }
        };

        document.addEventListener('DOMContentLoaded', () => {
            loadProfileFromServer().then(profile => {
                if (!profile) return;
                
                const fields = {
                    'header-username': profile.username,
                    'profile-name': profile.username,
                    'profile-email': profile.email,
                    'profile-timezone': profile.timezone
                };
                
                Object.keys(fields).forEach(id => {
                    const el = document.getElementById(id);
                    if (el && fields[id]) el.textContent = fields[id];
                });
                
                const avatarSrc = resolveAvatarPath(profile.avatar);
                ['header-avatar-dark', 'header-avatar-light', 'profile-avatar'].forEach(id => {
                    const el = document.getElementById(id);
                    if (el) el.src = avatarSrc || el.src;
                });
                
                if (profile.gender) {
                    document.getElementById('view-gender-female')?.classList.toggle('selected', profile.gender === 'female');
                    document.getElementById('view-gender-male')?.classList.toggle('selected', profile.gender === 'male');
                }
            });
        });
    </script>
</body>
</html>
)rawliteral";

const char EDIT_HTML[] PROGMEM = R"rawliteral(
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
                <img src="/image/Т.пол.png" alt="User" class="user-avatar avatar-dark" id="header-avatar-dark">
                <img src="/image/Св.пол.png" alt="User" class="user-avatar avatar-light" id="header-avatar-light">
                <span class="user-text" id="header-username">User_login</span>
            </div>
            
            <div class="logo-container">
                <img src="/image/Светлая.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/Тёмная.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>
            
            <div class="theme-switcher" onclick="toggleTheme()">
                <input type="checkbox" id="theme-toggle" class="mode-toggle">
                <label for="theme-toggle">
                    <img src="/image/тема.png" alt="Theme" class="theme-icon-img">
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
                        <img src="/image/warning-icon.png" alt="Warning" class="warning-icon" id="pass-warn-1" style="display:none;">
                    </div>
                    <div class="password-input-wrapper">
                        <input type="password" id="confirm-password" placeholder="Подтверждение нового пароль">
                        <img src="/image/warning-icon.png" alt="Warning" class="warning-icon" id="pass-warn-2" style="display:none;">
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
            // === НОВОЕ: Хелпер для разрешения пути к аватару ===
        function resolveAvatarPath(path) {
            if (!path) return '';
            if (path.startsWith('http')) return path;
            return '/api/avatar/file?file=' + encodeURIComponent(path);
        }

        // === НОВОЕ: Загрузка профиля с сервера ===
        async function loadProfileFromServer() {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                window.location.href = '/login';
                return null;
            }
            
            try {
                const res = await fetch('/api/profile', {
                    headers: {'Authorization': 'Bearer ' + token}
                });
                if (res.status === 401) {
                    localStorage.removeItem('auth_token');
                    window.location.href = '/login';
                    return null;
                }
                return await res.json();
            } catch (e) {
                console.error('Profile load error:', e);
                return null;
            }
        }
        
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

        document.addEventListener('DOMContentLoaded', async function() {
            // === НОВОЕ: Проверяем токен и загружаем профиль ===
            const profile = await loadProfileFromServer();
            if (!profile) return;
            
            // Заполняем поля из серверного ответа
            const fields = {
                'header-username': profile.username,
                'profile-name': profile.username,
                'profile-email': profile.email,
                'profile-timezone': profile.timezone,
                'edit-username': profile.username,
                'edit-email': profile.email,
                'edit-timezone': profile.timezone
            };
            
            Object.keys(fields).forEach(id => {
                const el = document.getElementById(id);
                if (el && fields[id]) el.textContent = fields[id];
            });
            
            // === НОВОЕ: Аватары через resolveAvatarPath ===
            const avatarSrc = resolveAvatarPath(profile.avatar);
            ['header-avatar-dark', 'header-avatar-light', 'profile-avatar'].forEach(id => {
                const el = document.getElementById(id);
                if (el) el.src = avatarSrc || el.dataset.default || el.src;
            });
            
            // Пол
            if (profile.gender) {
                document.getElementById('view-gender-female')?.classList.toggle('selected', profile.gender === 'female');
                document.getElementById('view-gender-male')?.classList.toggle('selected', profile.gender === 'male');
                document.getElementById('edit-gender-female')?.classList.toggle('selected', profile.gender === 'female');
                document.getElementById('edit-gender-male')?.classList.toggle('selected', profile.gender === 'male');
            }
            
            // Тема
            const savedTheme = localStorage.getItem('theme') || 'dark';
            document.body.classList.remove('theme-dark', 'theme-light');
            document.body.classList.add('theme-' + savedTheme);
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

        const confirmAvatarUpload = async () => {
            const token = localStorage.getItem('auth_token');
            if (!token || !tempAvatarData) return;
            
            const formData = new FormData();
            // Конвертируем dataURL в Blob
            const response = await fetch(tempAvatarData);
            const blob = await response.blob();
            formData.append('file', blob, 'avatar.jpg');
            
            try {
                const res = await fetch('/api/avatar', {
                    method: 'POST',
                    headers: {'Authorization': 'Bearer ' + token},
                    body: formData
                });
                const data = await res.json();
                if (data.success) {
                    // === НОВОЕ: Обновляем src через resolveAvatarPath ===
                    const newSrc = resolveAvatarPath(data.avatar);
                    document.getElementById('profile-avatar').src = newSrc;
                    document.getElementById('header-avatar-dark').src = newSrc;
                    document.getElementById('header-avatar-light').src = newSrc;
                    closeAvatarModal();
                } else {
                    alert('Ошибка загрузки: ' + (data.error || 'Unknown'));
                }
            } catch (e) {
                alert('Ошибка сети');
            }
        }

        async function saveChanges() {
            const token = localStorage.getItem('auth_token');
            if (!token) { window.location.href = '/login'; return; }
            
            const username = document.getElementById('edit-username').value.trim();
            const email = document.getElementById('edit-email').value.trim();
            const timezone = document.getElementById('edit-timezone').value;
            const gender = document.getElementById('edit-gender-female').classList.contains('selected') ? 'female' : 'male';
            
            if (!username || !email.includes('@')) {
                alert('Проверьте поля'); return;
            }
            
            const payload = { username, email, gender, timezone };
            
            try {
                const res = await fetch('/api/profile', {
                    method: 'POST',
                    headers: {
                        'Authorization': 'Bearer ' + token,
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify(payload)
                });
                const data = await res.json();
                if (data.success) {
                    alert('Сохранено!');
                    window.location.href = '/profile';
                } else {
                    alert('Ошибка: ' + (data.error || 'Unknown'));
                }
            } catch (e) {
                alert('Ошибка сети');
            }
        }
    </script>
</body>
</html>
)rawliteral";



const char SETTINGS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Настройки</title>
    <style>
        :root {
            --card-radius: 24px;
            --transition-speed: 0.4s;
        }

        /*ТЁМНАЯ ТЕМА*/
        body.theme-dark {
            --page-bg: #0F182B;
            --text-main: #00674B;
            --accent-green: #21C85F;
            --card-bg: rgba(255, 255, 255, 0.95);
            --glow-color: rgba(33, 200, 95, 0.6);
            --glow-soft: rgba(33, 200, 95, 0.3);
            --user-pill-bg: #1a2d45;
            --user-pill-text: #a0aab5;
            --nav-bg: #21C85F;
            --nav-btn-bg: rgba(255, 255, 255, 0.25);
            --label-bg: rgba(33, 200, 95, 0.15);
            --label-text: #21C85F;
            --footer-bg: #21C85F;
            --switcher-bg: #1a2d45;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.35);

            --body-bg: var(--page-bg);
            --title-color: var(--text-main);
            --green-primary: var(--accent-green);
            --green-hover: #1aab4e;
            --green-glow: var(--accent-green);
            --card-glow: var(--glow-color);
            --card-shadow: 0 0 20px var(--glow-soft);
            --subtitle-color: #7f8c8d;
            --network-row-bg: var(--label-bg);
            --network-row-border: transparent;
            --manual-section-bg: var(--card-bg);
            --input-border: rgba(33, 200, 95, 0.4);
            --input-focus-border: var(--accent-green);
            --input-bg: #ffffff;
            --label-color: #95a5a6;
            --btn-scan-bg: var(--accent-green);
            --btn-scan-text: #ffffff;
            --btn-connect-bg: var(--accent-green);
            --btn-connect-text: #ffffff;
            --btn-disconnect-bg: #ffffff;
            --btn-disconnect-text: #0a4d3a;
            --footer-text: #ffffff;
            --modal-overlay: rgba(15, 24, 43, 0.6);
            --modal-blur: blur(8px);
            --section-list-bg: var(--label-bg);
            --section-list-shadow: var(--card-internal-shadow);
            --plus-icon-bg: var(--accent-green);
            --section-glow: var(--glow-color);
            --empty-row-bg: rgba(33, 200, 95, 0.12);
            --modal-bg: var(--card-bg);
            --modal-input-bg: #ffffff;
            --footer-column-title: #ffffff;
            --footer-column-text: rgba(255,255,255,0.9);
            --footer-copy: rgba(255,255,255,0.8);
        }

        /*СВЕТЛАЯ ТЕМА*/
        body.theme-light {
            --page-bg: #E8F0F2;
            --text-main: #00674B;
            --accent-green: #10B981;
            --card-bg: #ffffff;
            --glow-color: rgba(16, 185, 129, 0.4);
            --glow-soft: rgba(16, 185, 129, 0.2);
            --user-pill-bg: #ffffff;
            --user-pill-text: #8899aa;
            --nav-bg: #10B981;
            --nav-btn-bg: rgba(255, 255, 255, 0.3);
            --label-bg: rgba(16, 185, 129, 0.15);
            --label-text: #10B981;
            --footer-bg: #10B981;
            --switcher-bg: #ffffff;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.2);
            --body-bg: var(--page-bg);
            --title-color: var(--text-main);
            --green-primary: var(--accent-green);
            --green-hover: #0d9668;
            --green-glow: var(--accent-green);
            --card-glow: var(--glow-soft);
            --card-shadow: 0 0 18px var(--glow-soft);
            --subtitle-color: #7f8c8d;
            --network-row-bg: var(--label-bg);
            --network-row-border: transparent;
            --manual-section-bg: var(--card-bg);
            --input-border: rgba(16, 185, 129, 0.35);
            --input-focus-border: var(--accent-green);
            --input-bg: #ffffff;
            --label-color: #a0a0a0;
            --btn-scan-bg: #78d5b2;
            --btn-scan-text: #0a4d3a;
            --btn-connect-bg: var(--accent-green);
            --btn-connect-text: #ffffff;
            --btn-disconnect-bg: #ffffff;
            --btn-disconnect-text: #0a4d3a;
            --footer-text: #ffffff;
            --modal-overlay: rgba(232, 240, 242, 0.7);
            --modal-blur: blur(8px);
            --section-list-bg: var(--label-bg);
            --section-list-shadow: var(--card-internal-shadow);
            --plus-icon-bg: var(--accent-green);
            --section-glow: var(--glow-soft);
            --empty-row-bg: rgba(16, 185, 129, 0.1);
            --modal-bg: var(--card-bg);
            --modal-input-bg: #ffffff;
            --footer-column-title: #ffffff;
            --footer-column-text: rgba(255,255,255,0.9);
            --footer-copy: rgba(255,255,255,0.8);
        }

        *, *::before, *::after {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;
            background: var(--body-bg);
            color: var(--title-color);
            min-height: 100vh;
            transition: background var(--transition-speed) ease,
                        color var(--transition-speed) ease;
            line-height: 1.5;
        }

        .mode-toggle {
            display: none !important;
            position: absolute;
            opacity: 0;
            pointer-events: none;
        }

        .page-wrapper {
            display: flex;
            flex-direction: column;
            min-height: calc(100vh - 200px);
        }

        .page-wrapper > .main-content {
            flex: 1 0 auto;
        }

        .top-sticky-wrapper {
            position: sticky;
            top: 0;
            z-index: 1000;
            width: 100%;
            background: transparent;
            pointer-events: none;
        }

        .top-sticky-wrapper > * {
            pointer-events: auto;
        }

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
            width: 145px;
            height: 44px;
            box-shadow: 0 0 8px var(--glow-soft);
            animation: userPulse 3s infinite alternate;
            flex-shrink: 0;
        }

        @keyframes userPulse {
            0% { box-shadow: 0 0 6px var(--glow-soft); }
            100% { box-shadow: 0 0 16px var(--glow-color); }
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

        .user-text {
            font-size: 14px;
            font-weight: 400;
            color: var(--user-pill-text);
            letter-spacing: 0.5px;
        }

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

        .theme-switcher {
            width: 48px;
            height: 48px;
            background: var(--switcher-bg);
            border-radius: 14px;
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
            box-shadow: 0 0 10px var(--glow-soft);
            animation: switcherPulse 3s infinite alternate;
            flex-shrink: 0;
        }

        @keyframes switcherPulse {
            0% { box-shadow: 0 0 8px var(--glow-soft); }
            100% { box-shadow: 0 0 18px var(--glow-color); }
        }

        .theme-icon-img {
            width: 24px;
            height: 24px;
            filter: invert(1);
        }

        .theme-light .theme-icon-img { filter: invert(0); }

        .navigation {
            display: flex;
            justify-content: center;
            align-items: center;
            gap: 8px;
            background: var(--nav-bg);
            border-radius: 40px;
            padding: 6px;
            margin: 0px 30px 35px 30px;
            height: 52px;
            pointer-events: auto;
            box-shadow: 0 6px 18px rgba(0, 0, 0, 0.3);
        }

        .nav-btn {
            color: #ffffff;
            text-decoration: none;
            font-size: 18px;
            font-weight: 600;
            padding: 8px 22px;
            border-radius: 30px;
            transition: all 0.3s ease;
            white-space: nowrap;
            position: relative;
            background: rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(4px);
            -webkit-backdrop-filter: blur(4px);
            border: 1px solid rgba(255, 255, 255, 0.1);
        }

        .nav-btn:hover,
        .nav-btn.active {
            background: rgba(255, 255, 255, 0.35);
            backdrop-filter: blur(10px);
            -webkit-backdrop-filter: blur(10px);
            border: 1px solid rgba(255, 255, 255, 0.25);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }

        .main-content {
            max-width: 1100px; 
            width: 100%;
            margin: 0 auto;
            padding: 0 30px 40px;
        }

        .scan-card {
            background: var(--card-bg);
            border-radius: var(--card-radius);
            padding: 60px 40px 50px; 
            text-align: center;
            box-shadow: var(--card-shadow);
            margin-bottom: 30px;
            transition: background var(--transition-speed) ease,
                        box-shadow var(--transition-speed) ease;
            position: relative;
        }

        .scan-card::before {
            content: '';
            position: absolute;
            inset: -2px;
            border-radius: calc(var(--card-radius) + 2px);
            box-shadow: 0 0 25px var(--card-glow);
            z-index: -1;
            transition: box-shadow var(--transition-speed) ease;
        }

        .wifi-icon {
            width: 150px;
            height: 150px;
            margin: 0 auto 20px;
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .wifi-img {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: none;
            transition: opacity var(--transition-speed) ease;
        }

        body.theme-dark .wifi-dark { display: block; }
        body.theme-light .wifi-light { display: block; }

        .scan-title {
            font-size: 26px; 
            font-weight: 700;
            color: var(--title-color);
            margin-bottom: 10px;
            transition: color var(--transition-speed) ease;
        }

        .scan-subtitle {
            font-size: 16px; 
            color: var(--subtitle-color);
            margin-bottom: 30px;
            transition: color var(--transition-speed) ease;
        }

        .btn-scan {
            display: inline-block;
            background: var(--btn-scan-bg);
            color: var(--btn-scan-text);
            border: none;
            border-radius: 40px;
            padding: 16px 50px; 
            font-size: 18px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(0, 0, 0, 0.15);
            letter-spacing: 0.3px;
        }

        .btn-scan:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 22px rgba(0, 0, 0, 0.2);
            filter: brightness(1.05);
        }

        .btn-scan:active {
            transform: translateY(0);
        }

        .network-list-section {
            background: var(--section-list-bg);
            border-radius: 16px;
            padding: 20px;
            margin-top: 35px;
            box-shadow: var(--section-list-shadow);
            transition: background var(--transition-speed) ease;
        }

        .network-item {
            display: flex;
            align-items: center;
            padding: 16px 20px;
            border-radius: 14px;
            margin-bottom: 12px;
            background: var(--network-row-bg);
            transition: background var(--transition-speed) ease;
        }

        .network-item:last-child { margin-bottom: 0; }
        .network-item.empty { background: var(--empty-row-bg); }

        .network-icon {
            width: 22px;
            height: 22px;
            margin-right: 14px;
            flex-shrink: 0;
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .wifi-img-small {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: none;
        }

        body.theme-dark .wifi-dark { display: block; }
        body.theme-light .wifi-light { display: block; }

        .network-name {
            font-size: 16px; 
            font-weight: 600;
            color: var(--title-color);
            min-width: 80px;
            transition: color var(--transition-speed) ease;
        }

        .network-security {
            font-size: 15px; 
            color: var(--subtitle-color);
            flex: 1;
            padding-left: 10px;
            transition: color var(--transition-speed) ease;
        }

        .btn-connect, .btn-disconnect {
            border: none;
            border-radius: 30px;
            padding: 8px 24px;
            font-size: 14px; 
            font-weight: 600;
            cursor: pointer;
            transition: all 0.25s ease;
            white-space: nowrap;
            flex-shrink: 0;
        }

        .btn-connect { background: var(--btn-connect-bg); color: var(--btn-connect-text); }
        .btn-connect:hover { filter: brightness(1.1); transform: translateY(-1px); }

        .btn-disconnect { background: var(--btn-disconnect-bg); color: var(--btn-disconnect-text); box-shadow: 0 2px 6px rgba(0,0,0,0.1); }
        .btn-disconnect:hover { transform: translateY(-1px); box-shadow: 0 3px 10px rgba(0,0,0,0.15); }

        .btn-add-network {
            display: inline-block;
            background: var(--btn-scan-bg);
            color: var(--btn-scan-text);
            border: none;
            border-radius: 40px;
            padding: 12px 32px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(0, 0, 0, 0.15);
            letter-spacing: 0.3px;
        }

        .btn-add-network:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 22px rgba(0, 0, 0, 0.2);
            filter: brightness(1.05);
        }
        .btn-add-network:active {
            transform: translateY(0);
        }

        .manual-section {
            background: var(--manual-section-bg);
            border-radius: var(--card-radius);
            padding: 50px 50px 55px; 
            box-shadow: var(--card-shadow);
            margin-bottom: 30px;
            transition: background var(--transition-speed) ease,
                        box-shadow var(--transition-speed) ease;
            position: relative;
        }

        .manual-section::before {
            content: '';
            position: absolute;
            inset: -2px;
            border-radius: calc(var(--card-radius) + 2px);
            box-shadow: 0 0 20px var(--card-glow);
            z-index: -1;
            transition: box-shadow var(--transition-speed) ease;
        }

        .manual-title {
            display: flex;
            align-items: center;
            font-size: 24px; 
            font-weight: 700;
            color: var(--title-color);
            margin-bottom: 30px;
            transition: color var(--transition-speed) ease;
        }

        .plus-icon {
            width: 28px;
            height: 28px;
            background: var(--plus-icon-bg);
            border-radius: 50%;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            margin-right: 12px;
            flex-shrink: 0;
            transition: background var(--transition-speed) ease;
        }

        .plus-icon svg { width: 16px; height: 16px; }
        .plus-icon svg line { stroke: #ffffff; stroke-width: 2.5; stroke-linecap: round; }

        .manual-form {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 24px 40px; 
        }

        .form-group { display: flex; flex-direction: column; }
        .form-group.full-width { grid-column: 1 / -1; }

        .form-label {
            font-size: 16px; 
            color: var(--label-color);
            margin-bottom: 10px;
            font-weight: 400;
            transition: color var(--transition-speed) ease;
        }

        .form-input {
            border: 2px solid var(--input-border);
            border-radius: 20px; 
            padding: 16px 22px; 
            font-size: 16px;
            color: var(--title-color);
            background: var(--input-bg);
            outline: none;
            transition: border-color 0.3s ease,
                        background var(--transition-speed) ease,
                        color var(--transition-speed) ease;
        }

        .form-input::placeholder { color: var(--label-color); }
        .form-input:focus { border-color: var(--input-focus-border); }

        .site-footer {
            background: var(--footer-bg);
            color: var(--footer-text);
            padding: 36px 40px 0;
            transition: background var(--transition-speed) ease;
            flex-shrink: 0;
        }

        .footer-columns {
            display: grid;
            grid-template-columns: 1.3fr 1fr 0.8fr;
            gap: 30px;
            max-width: 900px; 
            margin: 0 auto;
            padding-bottom: 24px;
            border-bottom: 1px solid rgba(255,255,255,0.2);
        }

        .footer-column-title {
            font-size: 20px;
            font-weight: 700;
            margin-bottom: 12px;
            color: var(--footer-column-title);
        }

        .footer-column-text { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-column-list { list-style: none; padding: 0; }
        .footer-column-list li { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }

        .footer-copyright {
            text-align: center;
            padding: 16px 0 12px;
            font-size: 14px;
            color: var(--footer-copy);
            max-width: 900px; 
            margin: 0 auto;
        }

        .modal-overlay {
            display: none;
            position: fixed;
            inset: 0;
            background: var(--modal-overlay);
            backdrop-filter: var(--modal-blur);
            -webkit-backdrop-filter: var(--modal-blur);
            z-index: 2000;
            align-items: center;
            justify-content: center;
            transition: background var(--transition-speed) ease;
        }

        .modal-overlay.active { display: flex; }

        .modal-card {
            background: var(--modal-bg);
            border-radius: 20px;
            padding: 28px 32px;
            width: 90%;
            max-width: 480px;
            box-shadow: 0 8px 40px rgba(0,0,0,0.15);
            position: relative;
            transition: background var(--transition-speed) ease;
        }

        .modal-network-row {
            display: flex;
            align-items: center;
            padding: 10px 14px;
            border-radius: 14px;
            background: var(--network-row-bg);
            margin-bottom: 18px;
            transition: background var(--transition-speed) ease;
        }

        .modal-network-name {
            font-size: 15px;
            font-weight: 600;
            color: var(--title-color);
            transition: color var(--transition-speed) ease;
        }

        .modal-network-security {
            font-size: 14px;
            color: var(--subtitle-color);
            margin-left: 16px;
            transition: color var(--transition-speed) ease;
        }

        .modal-label {
            font-size: 14px;
            color: var(--label-color);
            margin-bottom: 8px;
            display: block;
            transition: color var(--transition-speed) ease;
        }

        .modal-input-wrapper {
            position: relative;
            display: flex;
            align-items: center;
        }

        .modal-input {
            width: 100%;
            border: 2px solid var(--input-border);
            border-radius: 16px;
            padding: 12px 48px 12px 18px;
            font-size: 15px;
            color: var(--title-color);
            background: var(--modal-input-bg);
            outline: none;
            transition: border-color 0.3s ease,
                        background var(--transition-speed) ease,
                        color var(--transition-speed) ease;
        }

        .modal-input:focus { border-color: var(--input-focus-border); }

        .modal-eye-btn {
            position: absolute;
            right: 14px;
            background: none;
            border: none;
            cursor: pointer;
            padding: 4px;
            display: flex;
            align-items: center;
            justify-content: center;
        }

        #eyeImg {
            width: 22px;
            height: 22px;
            object-fit: contain;
        }

        body.theme-dark .theme-light-only { display: none !important; }
        body.theme-light .theme-dark-only { display: none !important; }

        @media (max-width: 700px) {
            .page-wrapper { min-height: calc(100vh - 170px); }
            .site-header { padding: 14px 16px; }
            .main-logo { height: 60px; }
            .user-pill { width: 110px; height: 38px; padding: 6px 10px 6px 6px; gap: 7px; }
            .user-avatar { width: 26px; height: 26px; }
            .user-text { font-size: 12px; }
            .theme-switcher { width: 40px; height: 40px; border-radius: 12px; }
            .navigation { margin: 0 12px 24px; height: 44px; gap: 4px; }
            .nav-btn { font-size: 14px; padding: 6px 14px; }
            .main-content { padding: 0 14px 30px; max-width: 100%; }
            .scan-card { padding: 40px 24px 30px; }
            .manual-section { padding: 25px 20px 30px; }
            .wifi-icon { width: 72px; height: 56px; }
            .scan-title { font-size: 20px; }
            .scan-subtitle { font-size: 13px; }
            .btn-scan { padding: 12px 36px; font-size: 15px; }
            .network-item { padding: 10px 12px; flex-wrap: wrap; gap: 6px 0; }
            .network-name { font-size: 13px; min-width: 60px; }
            .network-security { font-size: 12px; padding-left: 4px; }
            .btn-connect, .btn-disconnect { font-size: 12px; padding: 5px 14px; }
            .manual-title { font-size: 18px; }
            .manual-form { grid-template-columns: 1fr; gap: 14px; }
            .footer-columns { grid-template-columns: 1fr; gap: 20px; }
            .site-footer { padding: 28px 20px 0; }
            .modal-card { padding: 22px 20px; margin: 0 14px; }
        }

        @keyframes fadeInUp {
            from { opacity: 0; transform: translateY(16px); }
            to { opacity: 1; transform: translateY(0); }
        }

        .scan-card { animation: fadeInUp 0.5s ease forwards; }
        .manual-section { animation: fadeInUp 0.5s ease 0.15s forwards; opacity: 0; animation-fill-mode: forwards; }
        .site-footer { animation: fadeInUp 0.5s ease 0.3s forwards; opacity: 0; animation-fill-mode: forwards; }
        .network-item { animation: fadeInUp 0.35s ease forwards; opacity: 0; }
        .network-item:nth-child(1) { animation-delay: 0.1s; }
        .network-item:nth-child(2) { animation-delay: 0.2s; }
        .network-item:nth-child(3) { animation-delay: 0.3s; }
    </style>
</head>
<body class="theme-dark">
    <div class="top-sticky-wrapper">
        <header class="site-header">
            <a href="/profile" class="user-pill" aria-label="Профиль">
                <img src="/image/Т.пол.png" alt="" class="user-avatar avatar-dark">
                <img src="/image/Св.пол.png" alt="" class="user-avatar avatar-light">
                <span class="user-text">User_login</span>
            </a>

            <div class="logo-container">
                <img src="/image/Светлая.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/Тёмная.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>

            <label class="theme-switcher" for="themeToggle" aria-label="Сменить тему">
                <img src="/image/тема.png" alt="" class="theme-icon-img">
            </label>
        </header>
        <!-- ВАЖНО: чекбокс вынесен за пределы header -->
        <input type="checkbox" id="themeToggle" class="mode-toggle">

        <nav class="navigation">
            <a href="/index" class="nav-btn">Главная</a>
            <a href="/plant" class="nav-btn">Мои растения</a>
            <a href="/settings" class="nav-btn active">Настройки</a>
        </nav>
    </div>
    <div class="page-wrapper">
        <main class="main-content">
            <div class="scan-card">
                <div class="wifi-icon">
                    <img src="/image/Светлая_большая.png" alt="WiFi" class="wifi-img wifi-light">
                    <img src="/image/Тёмная_большая.png" alt="WiFi" class="wifi-img wifi-dark">
                </div>

                <h1 class="scan-title">Сканирование сетей</h1>
                <p class="scan-subtitle">Нажмите кнопку ниже для сканирования доступных Wi-Fi сетей</p>

                <button class="btn-scan" id="scanBtn" onclick="toggleScan()">Начать сканирование</button>
                <div class="network-list-section" id="networkList" style="display: none;">
                    <div class="network-item">
                        <div class="network-icon">
                            <img src="/image/Светлая_маленькая.png" alt="WiFi" class="wifi-img-small wifi-light">
                            <img src="/image/Тёмная_маленькая.png" alt="WiFi" class="wifi-img-small wifi-dark">
                        </div>
                        <span class="network-name">HONOR 7</span>
                        <span class="network-security">Защищено/Общедоступная</span>
                        <button class="btn-connect" onclick="openModal('HONOR 7')">Отключиться</button>
                    </div>

                    <div class="network-item">
                        <div class="network-icon">
                            <img src="/image/Светлая_маленькая.png" alt="WiFi" class="wifi-img-small wifi-light">
                            <img src="/image/Тёмная_маленькая.png" alt="WiFi" class="wifi-img-small wifi-dark">
                        </div>
                        <span class="network-name">iPhone</span>
                        <span class="network-security">Защищено/Общедоступная</span>
                        <button class="btn-disconnect" onclick="openModal('iPhone')">Подключиться</button>
                    </div>

                    <div class="network-item empty">
                        <div class="network-icon">
                            <img src="/image/Светлая_маленькая.png" alt="WiFi" class="wifi-img-small wifi-light">
                            <img src="/image/Тёмная_маленькая.png" alt="WiFi" class="wifi-img-small wifi-dark">
                        </div>
                        <span class="network-name" style="visibility: hidden;">—</span>
                        <span class="network-security"></span>
                    </div>
                </div>
            </div>

            <div class="manual-section">
                <h2 class="manual-title">
                    <span class="plus-icon">
                        <svg viewBox="0 0 14 14" xmlns="http://www.w3.org/2000/svg">
                            <line x1="7" y1="1" x2="7" y2="13"/>
                            <line x1="1" y1="7" x2="13" y2="7"/>
                        </svg>
                    </span>
                    Добавить сеть вручную
                </h2>

                <div class="manual-form">
                    <div class="form-group">
                        <label class="form-label">Название сети (SSID)</label>
                        <input type="text" class="form-input" placeholder="">
                    </div>
                    <div class="form-group">
                        <label class="form-label">Тип безопасности</label>
                        <input type="text" class="form-input" placeholder="">
                    </div>
                    <div class="form-group full-width">
                        <label class="form-label">Пароль</label>
                        <input type="password" class="form-input" placeholder="">
                    </div>
                    <div class="form-group full-width" style="margin-top: 16px; display: flex; justify-content: flex-end;">
                        <button type="button" class="btn-add-network" id="addNetworkBtn">Добавить</button>
                    </div>
                </div>
            </div>

        </main>

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

    </div>
    <div class="modal-overlay" id="passwordModal">
        <div class="modal-card">
            <div class="modal-network-row">
                <div class="network-icon">
                    <img src="/image/Светлая_маленькая.png" alt="WiFi" class="wifi-img-small wifi-light">
                    <img src="/image/Тёмная_маленькая.png" alt="WiFi" class="wifi-img-small wifi-dark">
                </div>
                <span class="modal-network-name" id="modalNetworkName">iPhone</span>
                <span class="modal-network-security">Защищено</span>
            </div>

            <label class="modal-label">Введите код безопасности</label>
            <div class="modal-input-wrapper">
                <input type="password" class="modal-input" id="modalPasswordInput">
                <button type="button" class="modal-eye-btn" id="eyeBtn">
                    <img src="/image/Открытый.png" id="eyeImg" alt="Показать пароль">
                </button>
            </div>
        </div>
    </div>

    <script>
        // === ИСПРАВЛЕННЫЙ JAVASCRIPT БЕЗ IIFE ===

        const initThemes = () => {
            // Первый инициализатор темы
            const toggle = document.getElementById('themeToggle');
            const body = document.body;
            const KEY = 'theme';
            const saved = localStorage.getItem(KEY) || 'dark';
            body.classList.remove('theme-dark', 'theme-light');
            body.classList.add('theme-' + saved);
            if (toggle) toggle.checked = (saved === 'light');

            if (toggle) {
                toggle.addEventListener('change', () => {
                    const next = toggle.checked ? 'light' : 'dark';
                    body.classList.remove('theme-dark', 'theme-light');
                    body.classList.add('theme-' + next);
                    localStorage.setItem(KEY, next);
                });
            }
        };

        // Второй инициализатор темы
        const initThemeSwitcher = () => {
            const body = document.body;
            let savedTheme = localStorage.getItem('theme') || 'dark';
            body.classList.remove('theme-light', 'theme-dark');
            body.classList.add('theme-' + savedTheme);

            const themeBtn = document.getElementById('themeSwitcher');
            if (savedTheme) {
                body.className = savedTheme === 'dark' ? 'theme-dark' : 'theme-light';
            }

            if (themeBtn) {
                themeBtn.addEventListener('click', () => {
                    if (body.classList.contains('theme-light')) {
                        body.className = 'theme-dark';
                        localStorage.setItem('theme', 'dark');
                    } else {
                        body.className = 'theme-light';
                        localStorage.setItem('theme', 'light');
                    }
                });
            }
        };

        // Остальные функции
        let isScanned = false;
        const toggleScan = () => {
            const btn = document.getElementById('scanBtn');
            const list = document.getElementById('networkList');
            if (!isScanned) {
                btn.textContent = 'Обновить список';
                list.style.display = 'block';
                isScanned = true;
            } else {
                list.style.display = 'none';
                void list.offsetWidth;
                list.style.display = 'block';
            }
        };

        let passwordVisible = false;
        const updateEyeSource = () => {
            const eyeImg = document.getElementById('eyeImg');
            if (passwordVisible) {
                eyeImg.src = '/image/Закрытый.png';
                eyeImg.alt = 'Скрыть пароль';
            } else {
                eyeImg.src = '/image/Открытый.png';
                eyeImg.alt = 'Показать пароль';
            }
        };

        const openModal = (networkName) => {
            const modal = document.getElementById('passwordModal');
            document.getElementById('modalNetworkName').textContent = networkName;
            modal.classList.add('active');
            passwordVisible = false;
            document.getElementById('modalPasswordInput').type = 'password';
            updateEyeSource();
        };

        const closeModal = () => {
            document.getElementById('passwordModal').classList.remove('active');
        };

        const loadUserToHeader = () => {
            const data = JSON.parse(localStorage.getItem('userData') || '{}');
            const userText = document.querySelector('.user-pill .user-text');
            if (userText) userText.textContent = data.username || 'User_login';

            const avDark = document.querySelector('.user-pill .avatar-dark');
            const avLight = document.querySelector('.user-pill .avatar-light');
            if (avDark) avDark.src = data.avatar || '';
            if (avLight) avLight.src = data.avatar || '';
        };

        // Запуск всего после загрузки страницы
        document.addEventListener('DOMContentLoaded', () => {
            initThemes();
            initThemeSwitcher();
            loadUserToHeader();

            // Обработчик для eyeBtn
            const eyeBtn = document.getElementById('eyeBtn');
            if (eyeBtn) {
                eyeBtn.addEventListener('click', () => {
                    passwordVisible = !passwordVisible;
                    const passwordInput = document.getElementById('modalPasswordInput');
                    if (passwordInput) {
                        passwordInput.type = passwordVisible ? 'text' : 'password';
                    }
                    updateEyeSource();
                });
            }

            // Обработчик закрытия модального окна
            const modal = document.getElementById('passwordModal');
            if (modal) {
                modal.addEventListener('click', (e) => {
                    if (e.target === modal) closeModal();
                });
            }
        });
    </script>
</body>
</html>
)rawliteral";


const char PLANT_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Мои растения</title>
    <style>
        :root {
            /* ТЁМНАЯ ТЕМА */
            --page-bg: #0F182B;
            --text-main: #ffffff;
            --accent-green: #21C85F;
            --card-bg: rgba(255, 255, 255, 0.95);
            --glow-color: rgba(33, 200, 95, 0.6);
            --glow-soft: rgba(33, 200, 95, 0.3);
            --user-pill-bg: #1a2d45;
            --user-pill-text: #a0aab5;
            --nav-bg: #21C85F;
            --nav-btn-bg: rgba(255, 255, 255, 0.25);
            --label-bg: rgba(33, 200, 95, 0.15);
            --label-text: #21C85F;
            --footer-bg: #21C85F;
            --switcher-bg: #1a2d45;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.35);
        }

        .theme-light {
            /* СВЕТЛАЯ ТЕМА */
            --page-bg: #E8F0F2;
            --text-main: #ffffff;
            --accent-green: #10B981;
            --card-bg: #ffffff;
            --glow-color: rgba(16, 185, 129, 0.4);
            --glow-soft: rgba(16, 185, 129, 0.2);
            --user-pill-bg: #ffffff;
            --user-pill-text: #8899aa;
            --nav-bg: #10B981;
            --nav-btn-bg: rgba(255, 255, 255, 0.3);
            --label-bg: rgba(16, 185, 129, 0.15);
            --label-text: #10B981;
            --footer-bg: #10B981;
            --switcher-bg: #ffffff;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.2);
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--page-bg);
            color: var(--text-main);
            transition: background-color 0.4s ease;
            min-height: 100vh;
            overflow-x: hidden;
            display: flex;
            flex-direction: column;
        }

        .mode-toggle {
            display: none !important;
            position: absolute;
            opacity: 0;
            pointer-events: none;
        }

        .top-sticky-wrapper {
            position: sticky;
            top: 0;
            z-index: 1000;
            width: 100%;
            background: transparent;
            pointer-events: none;
        }

        .top-sticky-wrapper > * {
            pointer-events: auto;
        }

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
            width: 145px;
            height: 44px;
            box-shadow: 0 0 8px var(--glow-soft);
            animation: userPulse 3s infinite alternate;
            flex-shrink: 0;
        }

        @keyframes userPulse {
            0% { box-shadow: 0 0 6px var(--glow-soft); }
            100% { box-shadow: 0 0 16px var(--glow-color); }
        }

        .user-avatar {
            width: 32px;
            height: 32px;
            border-radius: 50%;
            object-fit: cover;
            display: none;
        }

        body.theme-dark .avatar-dark {
            display: block;
        }

        body.theme-light .avatar-light {
            display: block;
        }

        .user-text {
            font-size: 14px;
            font-weight: 400;
            color: var(--user-pill-text);
            letter-spacing: 0.5px;
        }

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

        .theme-switcher {
            width: 48px;
            height: 48px;
            background: var(--switcher-bg);
            border-radius: 14px;
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
            box-shadow: 0 0 10px var(--glow-soft);
            animation: switcherPulse 3s infinite alternate;
            flex-shrink: 0;
        }

        @keyframes switcherPulse {
            0% { box-shadow: 0 0 8px var(--glow-soft); }
            100% { box-shadow: 0 0 18px var(--glow-color); }
        }

        .theme-icon-img {
            width: 24px;
            height: 24px;
            filter: invert(1);
        }

        .theme-light .theme-icon-img { filter: invert(0); }

        .navigation {
            display: flex;
            justify-content: center;
            align-items: center;
            gap: 8px;
            background: var(--nav-bg);
            border-radius: 40px;
            padding: 6px;
            margin: 0px 30px 35px 30px;
            height: 52px;
            pointer-events: auto;
            box-shadow: 0 6px 18px rgba(0, 0, 0, 0.3); 
        }

        .nav-btn {
            color: #ffffff;
            text-decoration: none;
            font-size: 18px;
            font-weight: 600;
            padding: 8px 22px;
            border-radius: 30px;
            transition: all 0.3s ease;
            white-space: nowrap;
            position: relative;
            background: rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(4px);
            -webkit-backdrop-filter: blur(4px);
            border: 1px solid rgba(255, 255, 255, 0.1);
        }

        .nav-btn:hover,
        .nav-btn.active {
            background: rgba(255, 255, 255, 0.35);
            backdrop-filter: blur(10px);
            -webkit-backdrop-filter: blur(10px);
            border: 1px solid rgba(255, 255, 255, 0.25);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }


        .page-wrapper {
            flex: 1;
            padding: 0 20px;
        }

        .cards-area {
            display: flex;
            justify-content: center;
            align-items: center;
            gap: 40px;
            padding: 20px 0 60px 0;
            flex-wrap: wrap;
        }

        .plant-card {
            width: 350px;
            height: 420px;
            background: var(--card-bg);
            border-radius: 32px;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            position: relative;
            transition: transform 0.3s;
            box-shadow: var(--card-internal-shadow);
        }

        .plant-card:hover { transform: translateY(-5px); }

        .glowing {
            animation: glowPulse 3s infinite alternate;
        }

        @keyframes glowPulse {
            0% { 
                box-shadow: 0 0 12px var(--glow-color), var(--card-internal-shadow); 
            }
            100% { 
                box-shadow: 0 0 28px var(--glow-color), var(--card-internal-shadow); 
            }
        }

        .plant-img {
            width: 300px;
            height: 300px;
            object-fit: contain;
            margin-bottom: 16px;
        }

        .plant-label {
            background: var(--label-bg);
            color: var(--label-text);
            font-size: 19px;
            font-weight: 300;
            padding: 8px 32px;
            border-radius: 24px;
            text-decoration: none;  
            display: inline-block;    
        }

        .add-card { cursor: pointer; }

        .plus-icon {
            display: none;
            width: 90px;
            height: 90px;
            object-fit: contain;
        }

        body.theme-dark .icon-dark { display: block; }
        body.theme-light .icon-light { display: block; }

        .site-footer {
            background: var(--footer-bg);
            color: var(--footer-text);
            padding: 36px 40px 0;
            transition: background var(--transition-speed) ease;
        }

        .footer-columns {
            display: grid;
            grid-template-columns: 1.3fr 1fr 0.8fr;
            gap: 30px;
            max-width: 900px;
            margin: 0 auto;
            padding-bottom: 24px;
            border-bottom: 1px solid rgba(255,255,255,0.2);
        }

        .footer-column-title {
            font-size: 20px;
            font-weight: 700;
            margin-bottom: 12px;
            color: var(--footer-column-title);
        }

        .footer-column-text {
            font-size: 14px;
            line-height: 1.7;
            color: var(--footer-column-text);
        }

        .footer-column-list {
            list-style: none;
            padding: 0;
        }

        .footer-column-list li {
            font-size: 14px;
            line-height: 1.7;
            color: var(--footer-column-text);
        }

        .footer-copyright {
            text-align: center;
            padding: 16px 0 12px;
            font-size: 14px;
            color: var(--footer-copy);
            max-width: 900px;
            margin: 0 auto;
        }

        .falling-leaf {
            position: fixed;
            pointer-events: none;
            z-index: 9999;
            animation: fallingLeaf var(--fall-duration, 3s) linear forwards;
        }

        .add-link {
            display: flex; 
            align-items: center;
            justify-content: center;
        }

        /* --- Стили для модального окна --- */
        .modal-overlay {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background-color: rgba(15, 24, 43, 0.6); /* Полупрозрачный фон */
            backdrop-filter: blur(8px);
            -webkit-backdrop-filter: blur(8px);
            z-index: 2000; /* Поверх всего */
            display: flex;
            justify-content: center;
            align-items: center;
        }

        .modal-content {
            background: var(--card-bg);
            padding: 30px;
            border-radius: 32px;
            width: 90%;
            max-width: 450px;
            position: relative;
            box-shadow: 0 0 30px var(--glow-soft);
            animation: modalFadeIn 0.3s ease;
            text-align: center;
            color: var(--text-main);
        }

        /* Цвет текста внутри модального окна */
        .modal-content h3, 
        .modal-content .modal-title,
        .modal-content .modal-title-sub {
            color: var(--accent-green);
            font-size: 18px;
            font-weight: 600;
            margin-bottom: 15px;
            line-height: 1.4;
        }

        .modal-title-sub {
            margin-top: 20px;
            font-size: 16px;
        }

        @keyframes modalFadeIn {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }

        .modal-close {
            position: absolute;
            top: 15px;
            right: 20px;
            background: var(--accent-green);
            border: none;
            color: white;
            width: 28px;
            height: 28px;
            border-radius: 50%;
            cursor: pointer;
            font-size: 16px;
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .modal-input {
            width: 100%;
            padding: 14px 20px;
            border-radius: 24px;
            border: 2px solid var(--accent-green);
            background: transparent;
            font-size: 16px;
            color: var(--accent-green);
            box-sizing: border-box;
            margin-bottom: 15px;
            outline: none;
        }

        .modal-input::placeholder {
            color: var(--accent-green);
            opacity: 0.5;
        }

        .modal-photo-area {
            margin: 20px 0;
        }

        .modal-preview-area {
            margin: 15px 0 20px 0;
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        .plant-preview {
            width: 120px;
            height: 120px;
            object-fit: contain;
            margin-bottom: 5px;
        }

        .modal-caption {
            font-size: 12px;
            color: var(--text-main);
            opacity: 0.6;
            display: block;
            margin-bottom: 5px;
        }

        .modal-link {
            color: var(--accent-green);
            font-size: 18px;
            font-weight: 600;
            text-decoration: underline;
            cursor: pointer;
        }

        .modal-btn {
            width: 100%;
            padding: 14px;
            border-radius: 24px;
            border: none;
            background: rgba(16, 185, 129, 0.3); /* Полупрозрачный по умолчанию (Disabled) */
            color: rgba(255, 255, 255, 0.5);
            font-size: 20px;
            font-weight: 700;
            cursor: not-allowed;
            margin-top: 10px;
            transition: all 0.3s ease;
        }

        /* Активная кнопка (как на картинках) */
        .modal-btn.active {
            background: var(--accent-green);
            color: #ffffff;
            cursor: pointer;
            box-shadow: 0 4px 15px rgba(0,0,0,0.2);
        }

        /* Адаптив для мобильных */
        @media (max-width: 480px) {
            .modal-content {
                padding: 20px;
                width: 95%;
            }
            .modal-input {
                padding: 12px 16px;
                font-size: 14px;
            }
        }

        /* Контейнер для динамических растений */
        .plants-container {
            display: contents;
        }

        /* Кнопка удаления растения */
        .delete-plant-btn {
            position: absolute;
            top: 15px;
            right: 15px;
            width: 32px;
            height: 32px;
            border-radius: 50%;
            border: none;
            background: rgba(255, 0, 0, 0.8);
            color: white;
            font-size: 22px;
            font-weight: bold;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            opacity: 0;
            transition: all 0.3s ease;
            z-index: 10;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
        }

        .plant-card:hover .delete-plant-btn {
            opacity: 1;
        }

        .delete-plant-btn:hover {
            background: rgba(255, 0, 0, 1);
            transform: scale(1.1);
            box-shadow: 0 4px 12px rgba(255, 0, 0, 0.4);
        }

        /* Стили для модального окна по умолчанию скрыто */
        .modal-overlay {
            display: none; /* Переопределяем из базового CSS */
        }

        /* Кнопка редактирования (карандаш) */
        .edit-plant-btn {
            position: absolute;
            top: 15px;
            left: 15px;
            width: 32px;
            height: 32px;
            border-radius: 50%;
            border: none;
            background: rgba(33, 200, 95, 0.8);
            color: white;
            font-size: 16px;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            opacity: 0;
            transition: all 0.3s ease;
            z-index: 10;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
        }

        .plant-card:hover .edit-plant-btn {
            opacity: 1;
        }

        .edit-plant-btn:hover {
            background: var(--accent-green);
            transform: scale(1.1);
            box-shadow: 0 4px 12px rgba(33, 200, 95, 0.4);
        }

        /* Адаптив кнопки редактирования */
        @media (max-width: 768px) {
            .edit-plant-btn {
                opacity: 1;
                top: 10px;
                left: 10px;
                width: 28px;
                height: 28px;
                font-size: 14px;
            }
        }
        @media (max-width: 480px) {
            .edit-plant-btn {
                width: 26px;
                height: 26px;
                font-size: 12px;
            }
        }

        /* Адаптив для кнопки удаления */
        @media (max-width: 768px) {
            .delete-plant-btn {
                opacity: 1;
                top: 10px;
                right: 10px;
                width: 28px;
                height: 28px;
                font-size: 18px;
            }
        }

        @media (max-width: 480px) {
            .delete-plant-btn {
                width: 26px;
                height: 26px;
                font-size: 16px;
            }
        }

        /* Стили для параметров в модальном окне */
        .modal-params-list {
            display: flex;
            flex-direction: column;
            gap: 12px;
            margin-bottom: 20px;
        }

        .modal-step {
            animation: modalFadeIn 0.3s ease;
        }

        @keyframes fallingLeaf {
            0% { transform: translateY(0) translateX(0) rotate(0deg); opacity: 1; }
            100% { transform: translateY(110vh) translateX(var(--sway, 30px)) rotate(var(--rotation, 20deg)); opacity: 0; }
        }

        @media (max-width: 768px) {
        /* Шапка */
        .site-header { 
            padding: 12px 16px; 
            flex-wrap: wrap;
            gap: 12px;
        }

        .logo-container { 
            position: relative !important;
            left: auto !important;
            transform: none !important;
            order: -1; 
            width: 100%; 
            justify-content: center; 
            margin-bottom: 0;
        }

        .main-logo { 
            height: 80px; 
        }

        .user-pill { 
            width: auto;
            padding: 6px 12px 6px 6px;
            height: 38px;
            order: 1;
            margin-right: auto;
            animation: none; /* Отключаем анимацию для экономии батареи */
        }

        .user-avatar {
            width: 26px;
            height: 26px;
        }

        .user-text {
            font-size: 13px;
        }

        .theme-switcher { 
            position: relative !important;
            top: auto !important;
            right: auto !important;
            width: 42px;
            height: 42px;
            order: 2;
            animation: none; /* Отключаем анимацию для экономии батареи */
        }

        .theme-icon-img {
            width: 20px;
            height: 20px;
        }

        /* Навигация */
        .navigation { 
            order: 3;
            margin: 8px 12px 20px 12px; 
            flex-wrap: wrap; 
            height: auto; 
            gap: 6px; 
            padding: 6px;
            justify-content: center;
        }

        .nav-btn { 
            padding: 8px 16px; 
            font-size: 15px;
            flex: 0 1 auto;
        }

        /* Карточки */
        .page-wrapper {
            padding: 0 12px;
        }

        .cards-area { 
            gap: 20px; 
            padding: 10px 0 40px 0;
        }

        .plant-card { 
            width: 100%;
            max-width: 340px;
            height: auto; 
            min-height: 360px;
            padding: 20px;
        }

        .plant-img { 
            width: 200px; 
            height: 200px; 
        }

        .plant-label {
            font-size: 17px;
            padding: 6px 24px;
        }

        .plus-icon {
            width: 70px;
            height: 70px;
        }

        /* Подвал */
        .footer-grid { 
            flex-direction: column; 
            text-align: center; 
            gap: 30px; 
            padding: 30px 20px 20px 20px; 
        }

        .footer-col {
            text-align: center !important;
        }

        .footer-col h3 {
            font-size: 19px;
            margin-bottom: 10px;
        }

        .footer-col p {
            font-size: 13px;
            line-height: 1.6;
        }

        .copyright-bar {
            padding: 12px;
            font-size: 12px;
        }

        /* Листья */
        .falling-leaf {
            display: none; /* Отключаем падающие листья на мобильных для производительности */
        }
        }

        /* Очень маленькие экраны */
        @media (max-width: 480px) {
        .site-header {
            padding: 10px 12px;
        }

        .main-logo {
            height: 80px;
        }

        .user-pill {
            padding: 5px 10px 5px 5px;
            height: 34px;
            gap: 8px;
        }

        .user-avatar {
            width: 24px;
            height: 24px;
        }

        .user-text {
            font-size: 12px;
        }

        .theme-switcher {
            width: 38px;
            height: 38px;
        }

        .navigation {
            margin: 0px 8px 16px 8px;
            padding: 5px;
            gap: 5px;
        }

        .nav-btn {
            padding: 7px 12px;
            font-size: 14px;
            border-radius: 24px;
        }

        .plant-card {
            padding: 16px;
            min-height: 320px;
            border-radius: 24px;
        }

        .plant-img {
            width: 160px;
            height: 160px;
            margin-bottom: 12px;
        }

        .plant-label {
            font-size: 16px;
            padding: 6px 20px;
            border-radius: 20px;
        }

        .plus-icon {
            width: 60px;
            height: 60px;
        }

        .footer-grid {
            padding: 25px 16px 16px 16px;
            gap: 24px;
        }

        .footer-col h3 {
            font-size: 17px;
        }

        .footer-col p {
            font-size: 12px;
        }
        }
    </style>
</head>
<body class="theme-dark">
    <div class="top-sticky-wrapper">
        
        <header class="site-header">
            <a href="/profile" class="user-pill" aria-label="Профиль">
                <img src="/image/Т.пол.png" alt="" class="user-avatar avatar-dark">
                <img src="/image/Св.пол.png" alt="" class="user-avatar avatar-light">
                <span class="user-text">User_login</span>
            </a>

            <div class="logo-container">
                <img src="/image/Светлая.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/Тёмная.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>

            <label class="theme-switcher" for="themeToggle" aria-label="Сменить тему">
                <img src="/image/тема.png" alt="" class="theme-icon-img">
            </label>
        </header>
        <input type="checkbox" id="themeToggle" class="mode-toggle">

        <nav class="navigation">
            <a href="/index" class="nav-btn">Главная</a> 
            <a href="/plant" class="nav-btn active">Мои растения</a>
            <a href="/settings" class="nav-btn">Настройки</a>
        </nav>
        
    </div>

    <div class="page-wrapper">
        <div class="cards-area">
            <div id="plants-container" class="plants-container"></div>
            <div class="plant-card glowing" data-plant-name="Фикус" data-params='{}'>
                <img src="/image/Фикус.png" alt="Фикус" class="plant-img">
                <a href="/app" class="plant-label">Фикус</a>
                <button class="edit-plant-btn" title="Редактировать">f</button>
                <button class="delete-plant-btn" onclick="deletePlant(this, 'Фикус')" title="Удалить растение">×</button>
            </div>
            
            <div class="plant-card glowing" data-plant-name="Монстера" data-params='{}'>
                <img src="/image/Монстера.png" alt="Монстера" class="plant-img">
                <a href="/app" class="plant-label">Монстера</a>
                <button class="edit-plant-btn" title="Редактировать">f</button>
                <button class="delete-plant-btn" onclick="deletePlant(this, 'Монстера')" title="Удалить растение">×</button>
            </div>
            
            <div class="plant-card add-card glowing" id="addPlantCard">
                <div class="add-link">
                    <img src="/image/Тёмная_иконка.png" alt="Добавить" class="plus-icon icon-dark">
                    <img src="/image/Светлая_иконка.png" alt="Добавить" class="plus-icon icon-light">
                </div>
            </div>
        </div>
    </div>

    <div id="plantModal" class="modal-overlay" style="display: none;">
        <div class="modal-content">
            <button class="modal-close" onclick="closeModal()">✕</button>
            
            <!-- Шаг 1: Название -->
            <div id="modal-step-1" class="modal-step">
                <h3 class="modal-title">Название растения</h3>
                <input type="text" id="plant-name-input" class="modal-input" placeholder="">
                
                <div class="modal-photo-area">
                    <a href="#" class="modal-link" onclick="handlePhotoSelect(event)">Выбрать фото</a>
                    <input type="file" id="file-input" style="display: none;" accept="image/*">
                </div>

                <button id="add-btn-step-1" class="modal-btn" disabled>Добавить</button>
            </div>

            <div id="modal-step-2" class="modal-step" style="display: none;">
                <h3 class="modal-title">Название растения</h3>
                <input type="text" id="plant-name-display" class="modal-input" readonly>
                
                <div class="modal-preview-area">
                    <img id="plant-preview-img" src="" alt="Preview" class="plant-preview" style="display: none;">
                    <span class="modal-caption">Предпросмотр</span>
                    <a href="#" class="modal-link" onclick="resetPhoto(event)">Выбрать фото</a>
                </div>

                <h3 class="modal-title-sub">Добавить комментарий</h3>
                <input type="text" id="plant-comment-input" class="modal-input" placeholder="Напишите что-нибудь...">

                <button class="modal-btn active" onclick="goToParameters()">Добавить</button>
            </div>

            <div id="modal-step-3" class="modal-step" style="display: none;">
                <h3 class="modal-title">Задайте начальные параметры<br>модуля сбора данных:</h3>
                
                <div class="modal-params-list">
                    <input type="number" class="modal-input param-input" placeholder="Начальный параметр влажности воздуха (%)">
                    <input type="number" class="modal-input param-input" placeholder="Начальный параметр влажность почвы (%)">
                    <input type="number" class="modal-input param-input" placeholder="Начальный параметр температуры (°C)">
                    <input type="number" class="modal-input param-input" placeholder="Начальный параметр атм. давление (мм рт.ст.)">
                </div>

                <button class="modal-btn active" onclick="finalAddPlant()">Добавить</button>
            </div>
        </div>
    </div>

    <!-- Модальное окно редактирования -->
    <div id="editModal" class="modal-overlay">
        <div class="modal-content">
            <button class="modal-close" id="closeEditModal">✕</button>
            <h3 class="modal-title">Редактировать растение</h3>
            
            <input type="text" id="editPlantName" class="modal-input" placeholder="Название растения">
            
            <div class="modal-photo-area">
                <label for="editPlantPhoto" class="modal-link">Изменить фото</label>
                <input type="file" id="editPlantPhoto" accept="image/*" style="display: none;">
            </div>
            
            <div class="modal-preview-area">
                <img id="editPlantPreview" class="plant-preview" src="" alt="Предпросмотр">
            </div>
            
            <h4 class="modal-title-sub">Измените параметры<br>модуля сбора данных:</h4>
            <div class="modal-params-list">
                <input type="number" id="editParamTemp" class="modal-input" placeholder="Параметр влажности воздуха (%)">
                <input type="number" id="editParamHumid" class="modal-input" placeholder="Параметр влажности почвы (%)">
                <input type="number" id="editParamLight" class="modal-input" placeholder="Параметр температуры (°C)">
                <input type="number" id="editParamWater" class="modal-input" placeholder="Параметр атм. давление (мм рт.ст.)">
            </div>
            
            <button id="savePlantBtn" class="modal-btn">Сохранить изменения</button>
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
        (function () {
            const toggle = document.getElementById('themeToggle');
            const body   = document.body;
            const KEY    = 'theme'; 
            const saved = localStorage.getItem(KEY) || 'dark';
            body.classList.remove('theme-dark', 'theme-light');
            body.classList.add('theme-' + saved);
            if (toggle) toggle.checked = (saved === 'light');
            if (toggle) {
                toggle.addEventListener('change', function () {
                    const next = toggle.checked ? 'light' : 'dark';
                    body.classList.remove('theme-dark', 'theme-light');
                    body.classList.add('theme-' + next);
                    localStorage.setItem(KEY, next);
                });
            }
        })();
        
        document.addEventListener('DOMContentLoaded', () => {
            const themeBtn = document.getElementById('themeSwitcher'); 
            const body = document.body;
            const savedTheme = localStorage.getItem('theme');
            if (savedTheme) {
                body.className = savedTheme === 'dark' ? 'theme-dark' : 'theme-light';
            }
            if(themeBtn) {
                themeBtn.addEventListener('click', () => {
                    if (body.classList.contains('theme-light')) {
                        body.className = 'theme-dark';
                        localStorage.setItem('theme', 'dark');
                    } else {
                        body.className = 'theme-light';
                        localStorage.setItem('theme', 'light');
                    }
                });
            }
        });

         function loadUserToHeader() {
            const data = JSON.parse(localStorage.getItem('userData'));
            if (!data) return; 
            const userText = document.querySelector('.user-pill .user-text');
            if (userText) userText.textContent = data.username;

            const avDark = document.querySelector('.user-pill .avatar-dark');
            const avLight = document.querySelector('.user-pill .avatar-light');
            if (avDark) avDark.src = data.avatar;
            if (avLight) avLight.src = data.avatar;
        }

        document.addEventListener('DOMContentLoaded', loadUserToHeader);

        const modal = document.getElementById('plantModal');
        const addCard = document.getElementById('addPlantCard');
        const closeBtn = document.querySelector('.modal-close');

        const step1 = document.getElementById('modal-step-1');
        const step2 = document.getElementById('modal-step-2');
        const step3 = document.getElementById('modal-step-3');

        const plantNameInput = document.getElementById('plant-name-input');
        const plantNameDisplay = document.getElementById('plant-name-display');
        const commentInput = document.getElementById('plant-comment-input');
        const fileInput = document.getElementById('file-input');
        const plantPreviewImg = document.getElementById('plant-preview-img');
        const addBtnStep1 = document.getElementById('add-btn-step-1');

        const paramInputs = document.querySelectorAll('#modal-step-3 .modal-input');

        // Открытие модального окна
        if (addCard) {
            addCard.addEventListener('click', () => {
                openModal();
            });
        }

        function openModal() {
            modal.style.display = 'flex';
            showStep(1);
            resetForm();
        }

        // Закрытие модального окна
        function closeModal() {
            modal.style.display = 'none';
        }

        if (closeBtn) {
            closeBtn.addEventListener('click', closeModal);
        }

        // Закрытие по клику вне модального окна
        modal.addEventListener('click', (e) => {
            if (e.target === modal) {
                closeModal();
            }
        });

        // Закрытие по Escape
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && modal.style.display === 'flex') {
                closeModal();
            }
        });

        // Показ нужного шага
        function showStep(stepNumber) {
            step1.style.display = 'none';
            step2.style.display = 'none';
            step3.style.display = 'none';
            
            if (stepNumber === 1) {
                step1.style.display = 'block';
            } else if (stepNumber === 2) {
                step2.style.display = 'block';
            } else if (stepNumber === 3) {
                step3.style.display = 'block';
            }
        }

        // Сброс формы
        function resetForm() {
            plantNameInput.value = '';
            plantNameDisplay.value = '';
            if (commentInput) {
                commentInput.value = '';
            }
            plantPreviewImg.src = '';
            plantPreviewImg.style.display = 'none';
            fileInput.value = '';
            addBtnStep1.disabled = true;
            addBtnStep1.classList.remove('active');
            
            paramInputs.forEach(input => {
                input.value = '';
            });
        }

        // Активация кнопки при вводе названия
        plantNameInput.addEventListener('input', function() {
            if (this.value.trim().length > 0) {
                addBtnStep1.disabled = false;
                addBtnStep1.classList.add('active');
            } else {
                addBtnStep1.disabled = true;
                addBtnStep1.classList.remove('active');
            }
        });

        // Выбор фото
        function handlePhotoSelect(e) {
            e.preventDefault();
            fileInput.click();
        }

        // Обработка загруженного файла
        fileInput.addEventListener('change', function(e) {
            const file = e.target.files[0];
            if (file) {
                if (!file.type.startsWith('image/')) {
                    alert('Пожалуйста, выберите изображение');
                    return;
                }
                
                if (file.size > 5 * 1024 * 1024) {
                    alert('Размер файла не должен превышать 5MB');
                    return;
                }
                
                const reader = new FileReader();
                reader.onload = function(e) {
                    plantPreviewImg.src = e.target.result;
                    plantPreviewImg.style.display = 'block';
                };
                reader.onerror = function() {
                    alert('Ошибка при загрузке файла');
                };
                reader.readAsDataURL(file);
            }
        });

        // Переход ко второму шагу
        addBtnStep1.addEventListener('click', function() {
            if (!this.disabled) {
                plantNameDisplay.value = plantNameInput.value;
                showStep(2);
            }
        });

        // Функция для перехода к параметрам
        function goToParameters() {
            showStep(3);
        }

        // Функция сброса фото
        function resetPhoto(e) {
            e.preventDefault();
            plantPreviewImg.src = '';
            plantPreviewImg.style.display = 'none';
            fileInput.value = '';
        }

        // Финальное добавление растения с перенаправлением
        function finalAddPlant() {
            const plantData = {
                id: Date.now(),
                name: plantNameInput.value.trim(),
                comment: commentInput ? commentInput.value.trim() : '',
                photo: plantPreviewImg.src || null,
                parameters: {
                    airHumidity: paramInputs[0].value || '',
                    soilHumidity: paramInputs[1].value || '',
                    temperature: paramInputs[2].value || '',
                    pressure: paramInputs[3].value || ''
                },
                dateAdded: new Date().toISOString()
            };
            
            if (!plantData.name) {
                alert('Пожалуйста, введите название растения');
                showStep(1);
                return;
            }
            
            try {
                let plants = JSON.parse(localStorage.getItem('myPlants') || '[]');
                plants.push(plantData);
                localStorage.setItem('myPlants', JSON.stringify(plants));
                
                console.log('Растение добавлено:', plantData);
                
                closeModal();
                
                // Перенаправление на страницу настроек
                window.location.href = '/app';
                
            } catch (error) {
                console.error('Ошибка при сохранении:', error);
                alert('Произошла ошибка при сохранении данных');
            }
        }

        // Загрузка и отображение сохранённых растений
        function loadPlants() {
            const container = document.getElementById('plants-container');
            if (!container) return;
            
            try {
                const plants = JSON.parse(localStorage.getItem('myPlants') || '[]');
                container.innerHTML = '';
                
                plants.forEach(plant => {
                    const plantCard = createPlantCard(plant);
                    container.appendChild(plantCard);
                });
            } catch (error) {
                console.error('Ошибка при загрузке растений:', error);
            }
        }

        // Создание карточки растения
        function createPlantCard(plant) {
            const card = document.createElement('div');
            card.className = 'plant-card glowing';
            card.setAttribute('data-plant-name', plant.name);
            card.setAttribute('data-plant-id', plant.id);
            
            const imgSrc = plant.photo || 'img/default-plant.png';
            const imgAlt = plant.name;
            
            card.innerHTML = `
                <img src="${imgSrc}" alt="${imgAlt}" class="plant-img">
                <a href="/app" class="plant-label">${plant.name}</a>
                <button class="delete-plant-btn" onclick="deletePlant(this, '${plant.name}', ${plant.id})" title="Удалить растение">×</button>
            `;
            
            return card;
        }

        // Удаление растения
        function deletePlant(btn, plantName, plantId) {
            if (confirm(`Вы действительно хотите удалить растение "${plantName}"?`)) {
                try {
                    let plants = JSON.parse(localStorage.getItem('myPlants') || '[]');
                    
                    if (plantId) {
                        // Удаление по ID (для динамических растений)
                        plants = plants.filter(p => p.id !== plantId);
                    } else {
                        // Удаление по имени (для статических растений)
                        plants = plants.filter(p => p.name !== plantName);
                    }
                    
                    localStorage.setItem('myPlants', JSON.stringify(plants));
                    
                    // Удаляем карточку из DOM
                    const card = btn.closest('.plant-card');
                    if (card) {
                        card.remove();
                    }
                    
                    console.log(`Растение "${plantName}" удалено`);
                } catch (error) {
                    console.error('Ошибка при удалении:', error);
                    alert('Произошла ошибка при удалении растения');
                }
            }
        }

        // Валидация параметров
        paramInputs.forEach(input => {
            input.addEventListener('input', function() {
                if (this.value === '' || !isNaN(this.value)) {
                    this.style.borderColor = 'var(--accent-green)';
                } else {
                    this.style.borderColor = 'red';
                }
            });
        });

        document.addEventListener('DOMContentLoaded', loadPlants);

        document.addEventListener('DOMContentLoaded', () => {
            const modal = document.getElementById('editModal');
            const closeBtn = document.getElementById('closeEditModal');
            const saveBtn = document.getElementById('savePlantBtn');
            const nameInput = document.getElementById('editPlantName');
            const photoInput = document.getElementById('editPlantPhoto');
            const previewImg = document.getElementById('editPlantPreview');
            
            const paramInputs = {
                temp: document.getElementById('editParamTemp'),
                humid: document.getElementById('editParamHumid'),
                light: document.getElementById('editParamLight'),
                water: document.getElementById('editParamWater')
            };

            let currentCard = null;

            // 1. Делегирование: открытие модалки для ЛЮБОЙ карточки
            document.querySelector('.cards-area').addEventListener('click', (e) => {
                const editBtn = e.target.closest('.edit-plant-btn');
                if (!editBtn) return;

                currentCard = editBtn.closest('.plant-card');
                const img = currentCard.querySelector('.plant-img');
                const label = currentCard.querySelector('.plant-label');
                const params = JSON.parse(currentCard.dataset.params || '{}');

                // Заполняем поля текущими значениями
                nameInput.value = label.textContent;
                previewImg.src = img.src;
                photoInput.value = ''; // сброс файлового инпута
                paramInputs.temp.value = params.temp || '';
                paramInputs.humid.value = params.humid || '';
                paramInputs.light.value = params.light || '';
                paramInputs.water.value = params.water || '';

                modal.style.display = 'flex';
                checkValidity();
            });

            // 2. Закрытие окна
            const closeModal = () => {
                modal.style.display = 'none';
                currentCard = null;
            };
            closeBtn.addEventListener('click', closeModal);
            modal.addEventListener('click', (e) => {
                if (e.target === modal) closeModal();
            });

            // 3. Предпросмотр фото
            photoInput.addEventListener('change', (e) => {
                const file = e.target.files[0];
                if (file) {
                    const reader = new FileReader();
                    reader.onload = (ev) => previewImg.src = ev.target.result;
                    reader.readAsDataURL(file);
                }
            });

            // 4. Активация кнопки сохранения
            const checkValidity = () => {
                const isValid = nameInput.value.trim().length > 0;
                saveBtn.classList.toggle('active', isValid);
                saveBtn.style.cursor = isValid ? 'pointer' : 'not-allowed';
            };
            nameInput.addEventListener('input', checkValidity);

            // 5. Сохранение изменений
            saveBtn.addEventListener('click', () => {
                if (!currentCard || !nameInput.value.trim()) return;

                // Обновляем DOM
                currentCard.querySelector('.plant-label').textContent = nameInput.value.trim();
                if (previewImg.src && previewImg.src !== currentCard.querySelector('.plant-img').src) {
                    currentCard.querySelector('.plant-img').src = previewImg.src;
                }

                // Сохраняем параметры в data-атрибут (для следующих открытий)
                const newParams = {
                    temp: paramInputs.temp.value,
                    humid: paramInputs.humid.value,
                    light: paramInputs.light.value,
                    water: paramInputs.water.value
                };
                currentCard.dataset.params = JSON.stringify(newParams);

                closeModal();
            });

            // Клик по ссылке "Изменить фото"
            document.querySelector('.modal-link')?.addEventListener('click', (e) => {
                e.preventDefault();
                photoInput.click();
            });
        });
    </script>
</body>
</html>
)rawliteral";

const char ROOT_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Главная</title>  
    <style>
        :root {
            /* ТЁМНАЯ ТЕМА */
            --page-bg: #0F182B;
            --text-main: #ffffff;
            --accent-green: #21C85F;
            --card-bg: rgba(255, 255, 255, 0.95);
            --glow-color: rgba(33, 200, 95, 0.6);
            --glow-soft: rgba(33, 200, 95, 0.3);
            --user-pill-bg: #1a2d45;
            --user-pill-text: #a0aab5;
            --nav-bg: #21C85F;
            --nav-btn-bg: rgba(255, 255, 255, 0.25);
            --label-bg: rgba(33, 200, 95, 0.15);
            --label-text: #21C85F;
            --footer-bg: #21C85F;
            --switcher-bg: #1a2d45;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.35);
        }

        .theme-light {
            /* СВЕТЛАЯ ТЕМА */
            --page-bg: #E8F0F2;
            --text-main: #ffffff;
            --accent-green: #10B981;
            --card-bg: #ffffff;
            --glow-color: rgba(16, 185, 129, 0.4);
            --glow-soft: rgba(16, 185, 129, 0.2);
            --user-pill-bg: #ffffff;
            --user-pill-text: #8899aa;
            --nav-bg: #10B981;
            --nav-btn-bg: rgba(255, 255, 255, 0.3);
            --label-bg: rgba(16, 185, 129, 0.15);
            --label-text: #10B981;
            --footer-bg: #10B981;
            --switcher-bg: #ffffff;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.2);
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--page-bg);
            color: var(--text-main);
            transition: background-color 0.4s ease;
            min-height: 100vh;
            overflow-x: hidden;
            display: flex;
            flex-direction: column;
        }

        .mode-toggle {
            display: none !important;
            position: absolute;
            opacity: 0;
            pointer-events: none;
        }

        .top-sticky-wrapper {
            position: sticky;
            top: 0;
            z-index: 1000;
            width: 100%;
            background: transparent;
            pointer-events: none;
        }

        .top-sticky-wrapper > * {
            pointer-events: auto;
        }

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
            width: 145px;
            height: 44px;
            box-shadow: 0 0 8px var(--glow-soft);
            animation: userPulse 3s infinite alternate;
            flex-shrink: 0;
        }

        @keyframes userPulse {
            0% { box-shadow: 0 0 6px var(--glow-soft); }
            100% { box-shadow: 0 0 16px var(--glow-color); }
        }

        .user-avatar {
            width: 32px;
            height: 32px;
            border-radius: 50%;
            object-fit: cover;
            display: none;
        }

        body.theme-dark .avatar-dark {
            display: block;
        }

        body.theme-light .avatar-light {
            display: block;
        }

        .user-text {
            font-size: 14px;
            font-weight: 400;
            color: var(--user-pill-text);
            letter-spacing: 0.5px;
        }

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

        .theme-switcher {
            width: 48px;
            height: 48px;
            background: var(--switcher-bg);
            border-radius: 14px;
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
            box-shadow: 0 0 10px var(--glow-soft);
            animation: switcherPulse 3s infinite alternate;
            flex-shrink: 0;
        }

        @keyframes switcherPulse {
            0% { box-shadow: 0 0 8px var(--glow-soft); }
            100% { box-shadow: 0 0 18px var(--glow-color); }
        }

        .theme-icon-img {
            width: 24px;
            height: 24px;
            filter: invert(1);
        }

        .theme-light .theme-icon-img { filter: invert(0); }

        .navigation {
            display: flex;
            justify-content: center;
            align-items: center;
            gap: 8px;
            background: var(--nav-bg);
            border-radius: 40px;
            padding: 6px;
            margin: 0px 30px 35px 30px;
            height: 52px;
            pointer-events: auto;
            box-shadow: 0 6px 18px rgba(0, 0, 0, 0.3); 
        }

        .nav-btn {
            color: #ffffff;
            text-decoration: none;
            font-size: 18px;
            font-weight: 600;
            padding: 8px 22px;
            border-radius: 30px;
            transition: all 0.3s ease;
            white-space: nowrap;
            position: relative;
            background: rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(4px);
            -webkit-backdrop-filter: blur(4px);
            border: 1px solid rgba(255, 255, 255, 0.1);
        }

        .nav-btn:hover,
        .nav-btn.active {
            background: rgba(255, 255, 255, 0.35);
            backdrop-filter: blur(10px);
            -webkit-backdrop-filter: blur(10px);
            border: 1px solid rgba(255, 255, 255, 0.25);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }


        .page-wrapper {
            flex: 1;
            padding: 0 20px;
        }

        .cards-area {
            display: flex;
            justify-content: center;
            align-items: center;
            gap: 40px;
            padding: 20px 0 60px 0;
            flex-wrap: wrap;
        }

        .plant-card {
            width: 350px;
            height: 420px;
            background: var(--card-bg);
            border-radius: 32px;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            position: relative;
            transition: transform 0.3s;
            box-shadow: var(--card-internal-shadow);
        }

        .plant-card:hover { transform: translateY(-5px); }

        .glowing {
            animation: glowPulse 3s infinite alternate;
        }

        @keyframes glowPulse {
            0% { 
                box-shadow: 0 0 12px var(--glow-color), var(--card-internal-shadow); 
            }
            100% { 
                box-shadow: 0 0 28px var(--glow-color), var(--card-internal-shadow); 
            }
        }

        .plant-img {
            width: 300px;
            height: 300px;
            object-fit: contain;
            margin-bottom: 16px;
        }

        .plant-label {
            background: var(--label-bg);
            color: var(--label-text);
            font-size: 19px;
            font-weight: 300;
            padding: 8px 32px;
            border-radius: 24px;
            text-decoration: none;  
            display: inline-block;    
        }

        .add-card { cursor: pointer; }

        .plus-icon {
            display: none;
            width: 90px;
            height: 90px;
            object-fit: contain;
        }

        body.theme-dark .icon-dark { display: block; }
        body.theme-light .icon-light { display: block; }

        .site-footer {
            background: var(--footer-bg);
            color: var(--footer-text);
            padding: 36px 40px 0;
            transition: background var(--transition-speed) ease;
        }

        .footer-columns {
            display: grid;
            grid-template-columns: 1.3fr 1fr 0.8fr;
            gap: 30px;
            max-width: 900px;
            margin: 0 auto;
            padding-bottom: 24px;
            border-bottom: 1px solid rgba(255,255,255,0.2);
        }

        .footer-column-title {
            font-size: 20px;
            font-weight: 700;
            margin-bottom: 12px;
            color: var(--footer-column-title);
        }

        .footer-column-text {
            font-size: 14px;
            line-height: 1.7;
            color: var(--footer-column-text);
        }

        .footer-column-list {
            list-style: none;
            padding: 0;
        }

        .footer-column-list li {
            font-size: 14px;
            line-height: 1.7;
            color: var(--footer-column-text);
        }

        .footer-copyright {
            text-align: center;
            padding: 16px 0 12px;
            font-size: 14px;
            color: var(--footer-copy);
            max-width: 900px;
            margin: 0 auto;
        }

        .falling-leaf {
            position: fixed;
            pointer-events: none;
            z-index: 9999;
            animation: fallingLeaf var(--fall-duration, 3s) linear forwards;
        }

        .add-link {
            display: flex; 
            align-items: center;
            justify-content: center;
        }

        /* --- Стили для модального окна --- */
        .modal-overlay {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background-color: rgba(15, 24, 43, 0.6); /* Полупрозрачный фон */
            backdrop-filter: blur(8px);
            -webkit-backdrop-filter: blur(8px);
            z-index: 2000; /* Поверх всего */
            display: flex;
            justify-content: center;
            align-items: center;
        }

        .modal-content {
            background: var(--card-bg);
            padding: 30px;
            border-radius: 32px;
            width: 90%;
            max-width: 450px;
            position: relative;
            box-shadow: 0 0 30px var(--glow-soft);
            animation: modalFadeIn 0.3s ease;
            text-align: center;
            color: var(--text-main);
        }

        /* Цвет текста внутри модального окна */
        .modal-content h3, 
        .modal-content .modal-title,
        .modal-content .modal-title-sub {
            color: var(--accent-green);
            font-size: 18px;
            font-weight: 600;
            margin-bottom: 15px;
            line-height: 1.4;
        }

        .modal-title-sub {
            margin-top: 20px;
            font-size: 16px;
        }

        @keyframes modalFadeIn {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }

        .modal-close {
            position: absolute;
            top: 15px;
            right: 20px;
            background: var(--accent-green);
            border: none;
            color: white;
            width: 28px;
            height: 28px;
            border-radius: 50%;
            cursor: pointer;
            font-size: 16px;
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .modal-input {
            width: 100%;
            padding: 14px 20px;
            border-radius: 24px;
            border: 2px solid var(--accent-green);
            background: transparent;
            font-size: 16px;
            color: var(--accent-green);
            box-sizing: border-box;
            margin-bottom: 15px;
            outline: none;
        }

        .modal-input::placeholder {
            color: var(--accent-green);
            opacity: 0.5;
        }

        .modal-photo-area {
            margin: 20px 0;
        }

        .modal-preview-area {
            margin: 15px 0 20px 0;
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        .plant-preview {
            width: 120px;
            height: 120px;
            object-fit: contain;
            margin-bottom: 5px;
        }

        .modal-caption {
            font-size: 12px;
            color: var(--text-main);
            opacity: 0.6;
            display: block;
            margin-bottom: 5px;
        }

        .modal-link {
            color: var(--accent-green);
            font-size: 18px;
            font-weight: 600;
            text-decoration: underline;
            cursor: pointer;
        }

        .modal-btn {
            width: 100%;
            padding: 14px;
            border-radius: 24px;
            border: none;
            background: rgba(16, 185, 129, 0.3); /* Полупрозрачный по умолчанию (Disabled) */
            color: rgba(255, 255, 255, 0.5);
            font-size: 20px;
            font-weight: 700;
            cursor: not-allowed;
            margin-top: 10px;
            transition: all 0.3s ease;
        }

        /* Активная кнопка (как на картинках) */
        .modal-btn.active {
            background: var(--accent-green);
            color: #ffffff;
            cursor: pointer;
            box-shadow: 0 4px 15px rgba(0,0,0,0.2);
        }

        /* Адаптив для мобильных */
        @media (max-width: 480px) {
            .modal-content {
                padding: 20px;
                width: 95%;
            }
            .modal-input {
                padding: 12px 16px;
                font-size: 14px;
            }
        }

        /* Контейнер для динамических растений */
        .plants-container {
            display: contents;
        }

        /* Кнопка удаления растения */
        .delete-plant-btn {
            position: absolute;
            top: 15px;
            right: 15px;
            width: 32px;
            height: 32px;
            border-radius: 50%;
            border: none;
            background: rgba(255, 0, 0, 0.8);
            color: white;
            font-size: 22px;
            font-weight: bold;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            opacity: 0;
            transition: all 0.3s ease;
            z-index: 10;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
        }

        .plant-card:hover .delete-plant-btn {
            opacity: 1;
        }

        .delete-plant-btn:hover {
            background: rgba(255, 0, 0, 1);
            transform: scale(1.1);
            box-shadow: 0 4px 12px rgba(255, 0, 0, 0.4);
        }

        /* Стили для модального окна по умолчанию скрыто */
        .modal-overlay {
            display: none; /* Переопределяем из базового CSS */
        }

        /* Кнопка редактирования (карандаш) */
        .edit-plant-btn {
            position: absolute;
            top: 15px;
            left: 15px;
            width: 32px;
            height: 32px;
            border-radius: 50%;
            border: none;
            background: rgba(33, 200, 95, 0.8);
            color: white;
            font-size: 16px;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            opacity: 0;
            transition: all 0.3s ease;
            z-index: 10;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
        }

        .plant-card:hover .edit-plant-btn {
            opacity: 1;
        }

        .edit-plant-btn:hover {
            background: var(--accent-green);
            transform: scale(1.1);
            box-shadow: 0 4px 12px rgba(33, 200, 95, 0.4);
        }

        /* Адаптив кнопки редактирования */
        @media (max-width: 768px) {
            .edit-plant-btn {
                opacity: 1;
                top: 10px;
                left: 10px;
                width: 28px;
                height: 28px;
                font-size: 14px;
            }
        }
        @media (max-width: 480px) {
            .edit-plant-btn {
                width: 26px;
                height: 26px;
                font-size: 12px;
            }
        }

        /* Адаптив для кнопки удаления */
        @media (max-width: 768px) {
            .delete-plant-btn {
                opacity: 1;
                top: 10px;
                right: 10px;
                width: 28px;
                height: 28px;
                font-size: 18px;
            }
        }

        @media (max-width: 480px) {
            .delete-plant-btn {
                width: 26px;
                height: 26px;
                font-size: 16px;
            }
        }

        /* Стили для параметров в модальном окне */
        .modal-params-list {
            display: flex;
            flex-direction: column;
            gap: 12px;
            margin-bottom: 20px;
        }

        .modal-step {
            animation: modalFadeIn 0.3s ease;
        }

        @keyframes fallingLeaf {
            0% { transform: translateY(0) translateX(0) rotate(0deg); opacity: 1; }
            100% { transform: translateY(110vh) translateX(var(--sway, 30px)) rotate(var(--rotation, 20deg)); opacity: 0; }
        }

        @media (max-width: 768px) {
        /* Шапка */
        .site-header { 
            padding: 12px 16px; 
            flex-wrap: wrap;
            gap: 12px;
        }

        .logo-container { 
            position: relative !important;
            left: auto !important;
            transform: none !important;
            order: -1; 
            width: 100%; 
            justify-content: center; 
            margin-bottom: 0;
        }

        .main-logo { 
            height: 80px; 
        }

        .user-pill { 
            width: auto;
            padding: 6px 12px 6px 6px;
            height: 38px;
            order: 1;
            margin-right: auto;
            animation: none; /* Отключаем анимацию для экономии батареи */
        }

        .user-avatar {
            width: 26px;
            height: 26px;
        }

        .user-text {
            font-size: 13px;
        }

        .theme-switcher { 
            position: relative !important;
            top: auto !important;
            right: auto !important;
            width: 42px;
            height: 42px;
            order: 2;
            animation: none; /* Отключаем анимацию для экономии батареи */
        }

        .theme-icon-img {
            width: 20px;
            height: 20px;
        }

        /* Навигация */
        .navigation { 
            order: 3;
            margin: 8px 12px 20px 12px; 
            flex-wrap: wrap; 
            height: auto; 
            gap: 6px; 
            padding: 6px;
            justify-content: center;
        }

        .nav-btn { 
            padding: 8px 16px; 
            font-size: 15px;
            flex: 0 1 auto;
        }

        /* Карточки */
        .page-wrapper {
            padding: 0 12px;
        }

        .cards-area { 
            gap: 20px; 
            padding: 10px 0 40px 0;
        }

        .plant-card { 
            width: 100%;
            max-width: 340px;
            height: auto; 
            min-height: 360px;
            padding: 20px;
        }

        .plant-img { 
            width: 200px; 
            height: 200px; 
        }

        .plant-label {
            font-size: 17px;
            padding: 6px 24px;
        }

        .plus-icon {
            width: 70px;
            height: 70px;
        }

        /* Подвал */
        .footer-grid { 
            flex-direction: column; 
            text-align: center; 
            gap: 30px; 
            padding: 30px 20px 20px 20px; 
        }

        .footer-col {
            text-align: center !important;
        }

        .footer-col h3 {
            font-size: 19px;
            margin-bottom: 10px;
        }

        .footer-col p {
            font-size: 13px;
            line-height: 1.6;
        }

        .copyright-bar {
            padding: 12px;
            font-size: 12px;
        }

        /* Листья */
        .falling-leaf {
            display: none; /* Отключаем падающие листья на мобильных для производительности */
        }
        }

        /* Очень маленькие экраны */
        @media (max-width: 480px) {
        .site-header {
            padding: 10px 12px;
        }

        .main-logo {
            height: 80px;
        }

        .user-pill {
            padding: 5px 10px 5px 5px;
            height: 34px;
            gap: 8px;
        }

        .user-avatar {
            width: 24px;
            height: 24px;
        }

        .user-text {
            font-size: 12px;
        }

        .theme-switcher {
            width: 38px;
            height: 38px;
        }

        .navigation {
            margin: 0px 8px 16px 8px;
            padding: 5px;
            gap: 5px;
        }

        .nav-btn {
            padding: 7px 12px;
            font-size: 14px;
            border-radius: 24px;
        }

        .plant-card {
            padding: 16px;
            min-height: 320px;
            border-radius: 24px;
        }

        .plant-img {
            width: 160px;
            height: 160px;
            margin-bottom: 12px;
        }

        .plant-label {
            font-size: 16px;
            padding: 6px 20px;
            border-radius: 20px;
        }

        .plus-icon {
            width: 60px;
            height: 60px;
        }

        .footer-grid {
            padding: 25px 16px 16px 16px;
            gap: 24px;
        }

        .footer-col h3 {
            font-size: 17px;
        }

        .footer-col p {
            font-size: 12px;
        }
        }
        .deco-leaf {
            position: absolute;
            z-index: 0;
            pointer-events: none;   
            width: 700px;  
            opacity: 0.9;
            transition: opacity 0.3s;
        }
        .leaf-left { left: 0px; top: 9.5%; }
        .leaf-right { right: 0px; top: -25%; }
        @media (max-width: 900px) {
            .deco-leaf { width: 250px; opacity: 0.4; }
            .leaf-left { left: -50px; }
            .leaf-right { right: -50px; }
        }

        .page-wrapper {
            position: relative;
            width: 100%;
            min-height: 1400px;
            padding-top: 60px;
            padding-bottom: 50px;
        }

        .content-block,
        .center-block {
            position: absolute;
            width: 650px;
            height: 250px;
            z-index: 2;
        }

        .content-block {
            left: 150px;
            top: 25px;
            text-align: left;
        }

        .hero-title {
            font-size: 52px;
            font-weight: 400;
            line-height: 1.1;
            margin-bottom: 24px;
            color: var(--text-main);
            transition: transform 0.3s ease;
        }

        .hero-text {
            font-size: 22px;
            line-height: 1.7;
            color: var(--text-main);
            opacity: 0.9;
            margin: 0;
            transition: transform 0.3s ease;
        }

        .hero-text:hover {
            transform: scale(1.04);
        }

        .center-block.about-block {
            right: 500px;
            top: 380px;
            text-align: right;
        }

        .center-block.modules-block {
            left: 50%;
            transform: translateX(-50%);
            top: 740px; /* ️ Сдвинул ниже (было 700px) */
            text-align: center;
        }

        .section-title {
            font-size: 52px;
            font-weight: 300;
            letter-spacing: 2px;
            margin-bottom: 10px;
            color: var(--accent-green);
            transition: transform 0.3s ease;
        }

        .section-title:hover {
            transform: scale(1.05);
        }

        /* ️ Принудительно тёмный цвет для текста в карточках */
        .module-title,
        .module-description {
            color: #0F182B; 
        }

        /* ГАЛЕРЕЯ МОДУЛЕЙ */
        .modules-gallery {
            position: absolute;
            left: 50%;
            transform: translateX(-50%);
            top: 820px;
            width: 90%;
            max-width: 1200px;
            display: flex;
            justify-content: center;
            align-items: stretch;
            gap: 30px;
            flex-wrap: wrap;
            z-index: 2;
            padding: 20px;
        }

        .module-card {
            background: var(--card-bg);
            border-radius: 32px;
            padding: 30px;
            width: 350px;
            min-height: 420px;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: flex-start;
            box-shadow: var(--card-internal-shadow);
            transition: all 0.3s ease;
            cursor: pointer;
            box-shadow: 0 10px 30px var(--glow-color)
        }

        .module-card:hover {
            transform: translateY(-5px);
        }

        .module-icon {
            width: 320px;
            height: 320px;
            object-fit: contain;
            margin-bottom: 20px;
            transition: transform 0.3s ease;
        }

        .module-card:hover .module-icon {
            transform: scale(1.1);
        }

        .module-title {
            font-size: 22px;
            font-weight: 600;
            margin-bottom: 12px;
            text-align: center;
        }

        .module-description {
            font-size: 15px;
            line-height: 1.6;
            opacity: 0.8;
            text-align: center;
            margin: 0;
        }

        body.theme-light .hero-title { color: #0E5A42; }
        body.theme-light .hero-text { color: #1C2533; }
        body.theme-light .section-title { color: #0E5A42; }
        body.theme-dark .hero-title { color: #21C85F; }
        body.theme-dark .hero-text { color: #FFFFFF; }
        body.theme-dark .section-title { color: #21C85F; }

        @media (max-width: 900px) {
            .content-block,
            .center-block {
                width: 90%;
                left: 5% !important;
                right: 5% !important;
                transform: none !important;
                position: relative;
                margin: 0 auto 40px auto;
                text-align: center !important;
            }
            .modules-gallery {
                position: relative;
                top: auto;
                left: auto;
                transform: none;
                margin-top: 40px;
            }
        }
    </style>
</head>
<body class="theme-light">

    <div class="top-sticky-wrapper">
        <header class="site-header">
            <a href="/profile" class="user-pill" aria-label="Профиль">
                <img src="/image/Т.пол.png" alt="" class="user-avatar avatar-dark">
                <img src="/image/Св.пол.png" alt="" class="user-avatar avatar-light">
                <span class="user-text">User_login</span>
            </a>

            <div class="logo-container">
                <img src="/image/Светлая.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/Тёмная.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>

            <label class="theme-switcher" for="themeToggle" aria-label="Сменить тему">
                <img src="/image/тема.png" alt="" class="theme-icon-img">
            </label>
        </header>
<!-- ВАЖНО: input вынесен за пределы header, чтобы не ломать flex-верстку и CSS-селекторы темы -->
        <input type="checkbox" id="themeToggle" class="mode-toggle">

        <nav class="navigation">
            <a href="/index" class="nav-btn active">Главная</a>
            <a href="/plant" class="nav-btn">Мои растения</a>
            <a href="/settings" class="nav-btn">Настройки</a>
        </nav>
    </div>

    <div class="page-wrapper">
        <img src="/image/Лево.png" alt="Leaf Decor" class="deco-leaf leaf-left">
        <img src="/image/Право.png" alt="Leaf Decor" class="deco-leaf leaf-right">
        
        <div class="content-block">
            <h1 class="hero-title">Мы любим то,<br>что делаем</h1>
            <p class="hero-text">
                Мы сделали умную систему, которая сама ухаживает за комнатными растениями. 
                У нас есть три устройства: одно проверяет тепло и влажность, другое включает 
                подсветку, третье поливает цветы. Все наши приборы работают без проводов и 
                сами решают, когда и сколько воды или света нужно растению.
            </p>
        </div>
        
        <div class="center-block about-block">
            <h2 class="hero-title">О нас</h2>
            <p class="hero-text">
                Мы сделали умную систему, которая сама ухаживает за комнатными растениями. 
                У нас есть три устройства: одно проверяет тепло и влажность, другое включает 
                подсветку, третье поливает цветы. Все наши приборы работают без проводов и 
                сами решают, когда и сколько воды или света нужно растению.
            </p>
        </div>
        <div class="center-block modules-block">
            <h2 class="hero-title">Модули</h2>
        </div>

        <div class="modules-gallery">
            <div class="module-card">
                <img src="/image/Осветительный.png" alt="Освещение" class="module-icon">
                <h3 class="module-title">Освещение растений</h3>
                <p class="module-description">
                    Автоматическая подсветка с регулировкой интенсивности и спектра для оптимального фотосинтеза
                </p>
            </div>

            <div class="module-card">
                <img src="/image/Полив.png" alt="Полив" class="module-icon">
                <h3 class="module-title">Полив растений</h3>
                <p class="module-description">
                    Умная система полива с контролем влажности почвы и автоматическим дозированием воды
                </p>
            </div>

            <div class="module-card" onclick="location.href='/app'">
                <img src="Сбор_данных.jpg" alt="Климат" class="module-icon">
                <h3 class="module-title">Контроль климата</h3>
                <p class="module-description">
                    Мониторинг температуры и влажности с автоматической корректировкой микроклимата
                </p>
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
        (function () {
            const toggle = document.getElementById('themeToggle');
            const body   = document.body;
            const KEY    = 'theme'; 
            const saved = localStorage.getItem(KEY) || 'dark';
            body.classList.toggle('theme-dark', saved === 'dark');
            body.classList.toggle('theme-light', saved === 'light');
            if (toggle) toggle.checked = saved === 'light';
            if (toggle) {
                toggle.addEventListener('change', function () {
                    const next = toggle.checked ? 'light' : 'dark';
                    body.classList.toggle('theme-dark', !toggle.checked);
                    body.classList.toggle('theme-light', toggle.checked);
                    localStorage.setItem(KEY, next);
                });
            }
        })();

        document.addEventListener('DOMContentLoaded', () => {
            const themeBtn = document.getElementById('themeSwitcher'); 
            const body = document.body;
            const savedTheme = localStorage.getItem('theme');
            
            if (savedTheme) {
                body.className = savedTheme === 'dark' ? 'theme-dark' : 'theme-light';
            }

            if(themeBtn) {
                themeBtn.addEventListener('click', () => {
                    if (body.classList.contains('theme-light')) {
                        body.className = 'theme-dark';
                        localStorage.setItem('theme', 'dark');
                    } else {
                        body.className = 'theme-light';
                        localStorage.setItem('theme', 'light');
                    }
                });
            }
        });

        function loadUserToHeader() {
            const data = JSON.parse(localStorage.getItem('userData'));
            if (!data) return; 
            const userText = document.querySelector('.user-pill .user-text');
            if (userText) userText.textContent = data.username;

            const avDark = document.querySelector('.user-pill .avatar-dark');
            const avLight = document.querySelector('.user-pill .avatar-light');
            if (avDark) avDark.src = data.avatar;
            if (avLight) avLight.src = data.avatar;
        }

        document.addEventListener('DOMContentLoaded', loadUserToHeader);
        
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
  server.send(200, "text/html", EDIT_HTML);
}

void handleSettings() {
  server.send(200, "text/html", SETTINGS_HTML);
}

void handlePlant() {
  server.send(200, "text/html", PLANT_HTML);
}

void handleIndex() {
  server.send(200, "text/html", ROOT_HTML);
}

/*// API для входа
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
}*/

/*// API для регистрации
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
}*/

void handleClear() {
  clearDB();
  server.send(200, "text/plain", "OK");
}

// === НОВОЕ: Обработчик API входа ===
void handleApiLogin() {
    if (!server.hasArg("login") || !server.hasArg("password")) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing fields\"}");
        return;
    }
    
    String login = server.arg("login");
    String password = server.arg("password");
    
    // Валидация минимальной длины
    if (login.length() < 3 || password.length() < 4) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid input\"}");
        return;
    }
    
    String response = loginUser(login, password);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(response.startsWith("{\"success\":true") ? 200 : 401, "application/json", response);
}


// === НОВОЕ: Обработчик API регистрации ===
void handleApiRegister() {
    if (!server.hasArg("login") || !server.hasArg("password") || !server.hasArg("email")) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing fields\"}");
        return;
    }
    
    String login = server.arg("login");
    String password = server.arg("password");
    String email = server.arg("email");
    
    // Валидация
    if (login.length() < 3 || password.length() < 4 || !email.indexOf('@')) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid input\"}");
        return;
    }
    
    bool success = registerUser(login, password, email);
    
    if (success) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Registration successful\"}");
    } else {
        server.send(409, "application/json", "{\"success\":false,\"message\":\"User already exists\"}");
    }
}

// === НОВОЕ: Получение профиля ===
void handleGetProfile() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    String profile = getProfileByToken(token);
    
    if (profile == "{}") {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
    } else {
        server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        server.send(200, "application/json", profile);
    }
}

// === НОВОЕ: Обновление профиля ===
void handleUpdateProfile() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ") || !server.hasArg("plain")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    if (!getUserIdByToken(token)) {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
        return;
    }
    
    // Парсинг JSON body
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    String username = doc["username"] | "";
    String email = doc["email"] | "";
    String gender = doc["gender"] | "female";
    String timezone = doc["timezone"] | "";
    String avatar = doc["avatar"] | "";
    
    bool success = updateProfileByToken(token, username, email, gender, timezone, avatar);
    
    if (success) {
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Update failed\"}");
    }
}

// === УЛУЧШЕННАЯ ЗАГРУЗКА АВАТАРА ===
void handleAvatarUpload() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        String auth = server.header("Authorization");
        if (!auth.startsWith("Bearer ")) {
            server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        
        String token = auth.substring(7);
        pendingUserId = getUserIdByToken(token);
        if (!pendingUserId) {
            server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
            return;
        }

        // Получаем и сохраняем старый аватар для удаления
        pendingOldAvatarPath = getCurrentAvatarPath(pendingUserId);
        
        // Новое имя файла
        pendingNewAvatarPath = "/sd/avatars/av_" + String(millis()) + ".jpg";
        Serial.printf("→ Загрузка аватара: %s\n", pendingNewAvatarPath.c_str());

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (pendingNewAvatarPath.length() == 0) return;
        
        File f = SD.open(pendingNewAvatarPath, FILE_WRITE);
        if (f) {
            f.write(upload.buf, upload.currentSize);
            f.close();
        } else {
            Serial.printf("Ошибка записи: %s\n", pendingNewAvatarPath.c_str());
        }

    } else if (upload.status == UPLOAD_FILE_END) {
        if (SD.exists(pendingNewAvatarPath)) {
            // Удаляем старый аватар
            if (pendingOldAvatarPath.length() > 0 && !pendingOldAvatarPath.startsWith("http")) {
                String oldFull = "/sd/" + pendingOldAvatarPath;
                if (SD.exists(oldFull)) {
                    SD.remove(oldFull);
                    Serial.printf("Удалён старый аватар: %s\n", oldFull.c_str());
                }
            }

            // Относительный путь для БД
            String relPath = "avatars/" + pendingNewAvatarPath.substring(12); // после /sd/avatars/

            if (updateUserAvatarInDB(server.header("Authorization").substring(7), relPath)) {
                DynamicJsonDocument doc(256);
                doc["success"] = true;
                doc["avatar"] = relPath;
                String json;
                serializeJson(doc, json);
                server.send(200, "application/json", json);
            } else {
                server.send(500, "application/json", "{\"error\":\"DB update failed\"}");
            }
        } else {
            server.send(500, "application/json", "{\"error\":\"File not saved\"}");
        }

        // Сброс
        pendingUserId = 0;
        pendingOldAvatarPath = "";
        pendingNewAvatarPath = "";
    }
}

// === НОВОЕ: Получение файла аватара ===
void handleGetAvatar() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing 'file' parameter");
        return;
    }
    
    String fileArg = server.arg("file");
    // Защита от path traversal
    if (fileArg.indexOf("..") >= 0 || fileArg.indexOf("//") >= 0) {
        server.send(400, "text/plain", "Invalid path");
        return;
    }
    
    String fullPath = "/sd/" + fileArg;
    if (!SD.exists(fullPath)) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    
    // Определяем MIME-type
    String contentType = "image/jpeg";
    if (fileArg.endsWith(".png")) contentType = "image/png";
    else if (fileArg.endsWith(".gif")) contentType = "image/gif";
    
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.sendHeader("Content-Type", contentType);
    
    // Потоковая отправка файла
    File f = SD.open(fullPath, FILE_READ);
    if (f) {
        server.streamFile(f, contentType);
        f.close();
    } else {
        server.send(500, "text/plain", "Failed to open file");
    }
}

// === НОВОЕ: Логаут ===
void handleApiLogout() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    if (db && getUserIdByToken(token)) {
        const char* sql = "DELETE FROM Sessions WHERE token=?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    
    server.send(200, "application/json", "{\"success\":true}");
}


void handleNotFound() {
  handleRoot();
}

// === Отдача статических изображений с SD-карты ===
void handleStaticImage() {
  String filePath = server.uri();           // например: /image/Т.пол.png
  String fullPath = "/sd" + filePath;       // → /sd/image/Т.пол.png

  if (!SD.exists(fullPath)) {
    server.send(404, "text/plain", "Image not found");
    return;
  }

  String contentType = "image/jpeg";
  if (filePath.endsWith(".png")) contentType = "image/png";
  else if (filePath.endsWith(".gif")) contentType = "image/gif";
  else if (filePath.endsWith(".jpg") || filePath.endsWith(".jpeg")) contentType = "image/jpeg";

  server.sendHeader("Cache-Control", "public, max-age=31536000"); // кэшировать на год
  File file = SD.open(fullPath, FILE_READ);
  if (file) {
    server.streamFile(file, contentType);
    file.close();
  } else {
    server.send(500, "text/plain", "Failed to open file");
  }
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
  createSDDirectories();

  if (!db) {
    Serial.println("Критическая ошибка: БД не инициализирована!");
    while (1) {
      delay(1000);
      Serial.println("Требуется SD-карта для работы!");
    }
  }
  
if (!SD.exists("/sd/avatars")) {
    SD.mkdir("/sd/avatars");
    Serial.println("Created /sd/avatars directory");
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
  server.on("/settings", handleSettings);
  server.on("/plant", handlePlant);
  server.on("/index", handleIndex);
  server.onNotFound(handleNotFound);
  // === НОВОЕ: Регистрация новых API маршрутов ===
server.on("/api/profile", HTTP_GET, handleGetProfile);
server.on("/api/profile", HTTP_POST, handleUpdateProfile);
server.on("/api/avatar", HTTP_POST, [](){ 
    // Пустой обработчик для инициализации upload
    server.send(200, "application/json", "{}"); 
}, handleAvatarUpload);
server.on("/api/avatar/file", HTTP_GET, handleGetAvatar);
server.on("/api/logout", HTTP_POST, handleApiLogout);

// Обновляем существующие обработчики
server.on("/api/login", HTTP_POST, handleApiLogin);      // ЗАМЕНИТЬ старую версию
server.on("/api/register", HTTP_POST, handleApiRegister); // ЗАМЕНИТЬ старую версию
  server.begin();
  server.on("/image/*", HTTP_GET, handleStaticImage);
  server.onNotFound(handleNotFound); // уже должно быть
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
