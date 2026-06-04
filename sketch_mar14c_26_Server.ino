#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <SD.h>
#include <BME280Spi.h>
#include <time.h>
#include <ArduinoJson.h>
#include <sqlite3.h>

#include <SHA256.h>

// ==================== E-PAPER БИБЛИОТЕКИ ====================
#include "epd1in54_V2.h"
#include "imagedata.h"
#include "epdpaint.h"

#define SERIAL_BAUD 115200

// Эндпоинт /debug раскрывает пользователей, e-mail и токены сессий.
// По умолчанию выключен. Установите 1 только для временной отладки.
#define ENABLE_DEBUG_ENDPOINT 1

// ==================== Wi-Fi НАСТРОЙКИ ====================
#define STA_SSID "Galaxy M56 5G A166"
#define STA_PASS "gdre3uqbhwts6t8"
#define AP_SSID  "netSensorModule-01"
#define AP_PASS  "12345678"
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
Preferences prefs;

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
int tzOffsetSec = 5 * 3600;  // Часовой пояс по умолчанию UTC+5, обновляется из профиля

// Парсит строку вида "(UTC+05:00) ..." или "(UTC-03:30) ..." → смещение в секундах
int parseTimezoneOffset(const String& tz) {
  int signIdx = tz.indexOf("UTC");
  if (signIdx < 0) return 5 * 3600; // default UTC+5
  int sign = 1;
  int pos = signIdx + 3;
  if (tz[pos] == '-') { sign = -1; pos++; }
  else if (tz[pos] == '+') { sign = 1; pos++; }
  int colon = tz.indexOf(':', pos);
  if (colon < 0) return 5 * 3600;
  int hours = tz.substring(pos, colon).toInt();
  int mins  = tz.substring(colon + 1, colon + 3).toInt();
  return sign * (hours * 3600 + mins * 60);
}

// Обновить tzOffsetSec из БД по первому найденному пользователю (или конкретному userId)
void reloadTimezoneFromDB(int userId = 0) {
  if (!db) return;
  sqlite3_stmt* stmt = nullptr;
  const char* sql = userId ? "SELECT timezone FROM Users WHERE id=? LIMIT 1;" 
                           : "SELECT timezone FROM Users LIMIT 1;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    if (userId) sqlite3_bind_int(stmt, 1, userId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* tz = (const char*)sqlite3_column_text(stmt, 0);
      if (tz) {
        tzOffsetSec = parseTimezoneOffset(String(tz));
        Serial.printf("[TZ] Часовой пояс обновлён: %s → %d сек\n", tz, tzOffsetSec);
      }
    }
    sqlite3_finalize(stmt);
  }
}

// === НОВОЕ: Глобальные переменные для загрузки файлов ===
int pendingUserId = 0;
String pendingOldAvatarPath = "";
String pendingNewAvatarPath = "";
// Результат загрузки (заполняется в upload-хендлере, отправляется в финальном хендлере)
bool   uploadOk = false;
String uploadResultPath = "";
String uploadErrorMsg = "";

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


void createSDDirectories() {
  Serial.println("=== Создание папок на SD-карте ===");

  // Лучше создавать относительно корня после монтирования
  const char* folders[] = {"image", "avatar", "plant"};

  for (int i = 0; i < 3; i++) {
    String path = String("/") + folders[i];
    
    if (!SD.exists(path)) {
      if (SD.mkdir(path)) {
        Serial.printf("Создана папка: %s\n", path.c_str());
      } else {
        Serial.printf("Не удалось создать папку: %s\n", path.c_str());
      }
    } else {
      Serial.printf("Папка уже существует: %s\n", path.c_str());
    }
  }

  // Дополнительная проверка
  Serial.println("Проверка содержимого SD:");
  File root = SD.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf("  - %s (%s)\n", file.name(), file.isDirectory() ? "DIR" : "FILE");
    file = root.openNextFile();
  }
  root.close();
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
    "salt TEXT DEFAULT '', "
    "created_at INTEGER NOT NULL);";

  rc = sqlite3_exec(db, createUsers, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    Serial.printf("Ошибка создания Users: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }

  // Добавляем колонки, если их нет (на случай старой таблицы)
  sqlite3_exec(db, "ALTER TABLE Users ADD COLUMN username TEXT UNIQUE;", NULL, NULL, NULL);
  sqlite3_exec(db, "ALTER TABLE Users ADD COLUMN avatar TEXT DEFAULT '';", NULL, NULL, NULL);
  sqlite3_exec(db, "ALTER TABLE Users ADD COLUMN salt TEXT DEFAULT '';", NULL, NULL, NULL);

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

    // Таблица Plants (растения пользователя)
    const char* sqlPlants = 
    "CREATE TABLE IF NOT EXISTS Plants ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "user_id INTEGER NOT NULL, "
    "name TEXT NOT NULL, "
    "comment TEXT, "
    "photo_path TEXT, "
    "temp_param REAL, "
    "humidity_param REAL, "
    "soil_param REAL, "
    "pressure_param REAL, "
    "created_at INTEGER NOT NULL, "
    "FOREIGN KEY(user_id) REFERENCES Users(id) ON DELETE CASCADE);";

    rc = sqlite3_exec(db, sqlPlants, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
        Serial.printf("Ошибка создания Plants: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    } else {
        Serial.println("Таблица Plants создана/проверена");
    }

    // Добавить индексы для быстрого поиска
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_plants_user_id ON Plants(user_id);", NULL, NULL, NULL);

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
        // esp_random() — аппаратный ГСЧ, не зависит от seed и не предсказуем
        token += chars[esp_random() % 62];
    }
    return token;
}

// === Генерация случайной соли (32 hex-символа) ===
String generateSalt() {
    const char* hexChars = "0123456789abcdef";
    String salt;
    salt.reserve(32);
    for (int i = 0; i < 32; i++) {
        salt += hexChars[esp_random() % 16];
    }
    return salt;
}

// === РЕГИСТРАЦИЯ ===
bool registerUser(const String& username, const String& password, const String& email, 
                  const String& gender = "female", const String& timezone = "(UTC+05:00) Asia/Yekaterinburg") {
    if (!db) return false;
    
    // Проверка уникальности
    sqlite3_stmt* stmt = nullptr;
    const char* checkSql = "SELECT id FROM Users WHERE username=? OR email=? LIMIT 1;";
    if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return false; // уже существует
        }
        sqlite3_finalize(stmt);
    }

    String salt = generateSalt();
    String hashedPass = hashPassword(salt + password);

    const char* sql = "INSERT INTO Users (username, password, email, gender, timezone, salt, created_at) VALUES (?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hashedPass.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, gender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, timezone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, salt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, time(nullptr));

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

// === НОВОЕ: Авторизация пользователя ===
String loginUser(const String& username, const String& password) {
    if (!db) return "{\"success\":false}";

    // Достаём пользователя по логину и проверяем хэш с его солью.
    // Старые пользователи имеют пустую соль => hashPassword("" + password) == hashPassword(password),
    // т.е. совместимость со старой (несолёной) схемой сохраняется.
    const char* sql = "SELECT id, username, password, salt FROM Users WHERE username=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return "{\"success\":false}";
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int userId = sqlite3_column_int(stmt, 0);
        const char* uname = (const char*)sqlite3_column_text(stmt, 1);
        String unameStr = uname ? String(uname) : "";
        const char* storedHashC = (const char*)sqlite3_column_text(stmt, 2);
        const char* saltC = (const char*)sqlite3_column_text(stmt, 3);
        String storedHash = storedHashC ? String(storedHashC) : "";
        String salt = saltC ? String(saltC) : "";

        String computed = hashPassword(salt + password);
        if (computed != storedHash) {
            sqlite3_finalize(stmt);
            return "{\"success\":false,\"message\":\"Invalid credentials\"}";
        }

        String token = generateToken();

        // === УДАЛЯЕМ ВСЕ СТАРЫЕ СЕССИИ ЭТОГО ПОЛЬЗОВАТЕЛЯ ===
        // (делаем это ТОЛЬКО после успешной проверки логина/пароля)
        const char* deleteOld = "DELETE FROM Sessions WHERE user_id = ?;";
        sqlite3_stmt* delStmt = nullptr;
        if (sqlite3_prepare_v2(db, deleteOld, -1, &delStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(delStmt, 1, userId);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }

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
        doc["username"] = unameStr;
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
    
    const char* sql = "SELECT user_id, created_at FROM Sessions WHERE token=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int userId = sqlite3_column_int(stmt, 0);
        long created = sqlite3_column_int(stmt, 1);
        sqlite3_finalize(stmt);

        // Проверка срока жизни сессии (30 дней).
        // Проверяем только при синхронизированном времени, иначе можно
        // случайно разлогинить всех, пока часы не получили NTP.
        const long SESSION_TTL = 30L * 24 * 3600;
        time_t now = time(nullptr);
        if (timeSynced && created > 0 && ((long)now - created) > SESSION_TTL) {
            // Удаляем протухшую сессию
            const char* del = "DELETE FROM Sessions WHERE token=?;";
            sqlite3_stmt* delStmt = nullptr;
            if (sqlite3_prepare_v2(db, del, -1, &delStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(delStmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(delStmt);
                sqlite3_finalize(delStmt);
            }
            return 0;
        }
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

// ==================== ФУНКЦИИ ДЛЯ РАБОТЫ С РАСТЕНИЯМИ ====================

// Получить все растения пользователя
String getUserPlants(int userId) {
    if (!db || userId == 0) return "[]";
    
    const char* sql = "SELECT id, name, comment, photo_path, temp_param, humidity_param, soil_param, pressure_param, created_at FROM Plants WHERE user_id = ? ORDER BY created_at DESC;";
    sqlite3_stmt* stmt = nullptr;
    
    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.to<JsonArray>();
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, userId);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            JsonObject obj = arr.createNestedObject();
            obj["id"] = sqlite3_column_int(stmt, 0);
            
            const char* name = (const char*)sqlite3_column_text(stmt, 1);
            obj["name"] = name ? String(name) : "";
            
            const char* comment = (const char*)sqlite3_column_text(stmt, 2);
            obj["comment"] = comment ? String(comment) : "";
            
            const char* photo = (const char*)sqlite3_column_text(stmt, 3);
            obj["photo"] = photo ? String(photo) : "";
            
            obj["temp"] = sqlite3_column_double(stmt, 4);
            obj["humidity"] = sqlite3_column_double(stmt, 5);
            obj["soil"] = sqlite3_column_double(stmt, 6);
            obj["pressure"] = sqlite3_column_double(stmt, 7);
            obj["created_at"] = sqlite3_column_int(stmt, 8);
        }
        sqlite3_finalize(stmt);
    }
    
    String out;
    serializeJson(arr, out);
    return out;
}

// Добавить растение
bool addPlant(int userId, const String& name, const String& comment, const String& photoPath,
              float temp, float humidity, float soil, float pressure) {
    if (!db || userId == 0) return false;
    
    const char* sql = "INSERT INTO Plants (user_id, name, comment, photo_path, temp_param, humidity_param, soil_param, pressure_param, created_at) VALUES (?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Serial.printf("Prepare addPlant error: %s\n", sqlite3_errmsg(db));
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, userId);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, comment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, photoPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, temp);
    sqlite3_bind_double(stmt, 6, humidity);
    sqlite3_bind_double(stmt, 7, soil);
    sqlite3_bind_double(stmt, 8, pressure);
    sqlite3_bind_int(stmt, 9, time(nullptr));
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    
    if (!success) {
        Serial.printf("Add plant error: %s\n", sqlite3_errmsg(db));
    }
    return success;
}

// Обновить растение
bool updatePlant(int plantId, int userId, const String& name, const String& comment, const String& photoPath,
                 float temp, float humidity, float soil, float pressure) {
    if (!db || userId == 0) return false;
    
    String sql = "UPDATE Plants SET name=?, comment=?, photo_path=?, temp_param=?, humidity_param=?, soil_param=?, pressure_param=? WHERE id=? AND user_id=?;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        Serial.printf("Prepare updatePlant error: %s\n", sqlite3_errmsg(db));
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, comment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, photoPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, temp);
    sqlite3_bind_double(stmt, 5, humidity);
    sqlite3_bind_double(stmt, 6, soil);
    sqlite3_bind_double(stmt, 7, pressure);
    sqlite3_bind_int(stmt, 8, plantId);
    sqlite3_bind_int(stmt, 9, userId);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

// Удалить растение
bool deletePlant(int plantId, int userId) {
    if (!db || userId == 0) return false;
    
    // Сначала получим путь к фото для удаления файла
    const char* selectSql = "SELECT photo_path FROM Plants WHERE id=? AND user_id=?;";
    sqlite3_stmt* selectStmt = nullptr;
    String photoPath;
    
    if (sqlite3_prepare_v2(db, selectSql, -1, &selectStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(selectStmt, 1, plantId);
        sqlite3_bind_int(selectStmt, 2, userId);
        
        if (sqlite3_step(selectStmt) == SQLITE_ROW) {
            const char* path = (const char*)sqlite3_column_text(selectStmt, 0);
            if (path) photoPath = String(path);
        }
        sqlite3_finalize(selectStmt);
    }
    
    // Удаляем запись из БД
    const char* deleteSql = "DELETE FROM Plants WHERE id=? AND user_id=?;";
    sqlite3_stmt* deleteStmt = nullptr;
    
    if (sqlite3_prepare_v2(db, deleteSql, -1, &deleteStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int(deleteStmt, 1, plantId);
    sqlite3_bind_int(deleteStmt, 2, userId);
    
    bool success = (sqlite3_step(deleteStmt) == SQLITE_DONE);
    sqlite3_finalize(deleteStmt);
    
    // Удаляем файл фото если он существует и не является дефолтным
    if (success && photoPath.length() > 0 && !photoPath.startsWith("http")) {
        String fullPath = "/" + photoPath;
        if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;
        if (SD.exists(fullPath)) {
            SD.remove(fullPath);
            Serial.printf("Удален файл фото: %s\n", fullPath.c_str());
        }
    }
    
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

    // Ограничение размера журнала: удаляем самые старые записи сверх MAX_LOG_ENTRIES,
    // чтобы БД на SD-карте не росла бесконечно.
    String trimSql = "DELETE FROM SensorLog WHERE ID NOT IN "
                     "(SELECT ID FROM SensorLog ORDER BY ID DESC LIMIT " +
                     String(MAX_LOG_ENTRIES) + ");";
    sqlite3_exec(db, trimSql.c_str(), NULL, NULL, NULL);
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
transition: transform 0.2s, box-shadow 0.2s, background 0.3s ease, border-color 0.3s ease;
}
.metric-card:hover { transform: translateY(-2px); box-shadow: 0 8px 30px rgba(0,0,0,0.12); }
.metric-card.alert-danger {
border-color: var(--danger) !important;
box-shadow: 0 0 0 2px rgba(239,68,68,0.25), var(--shadow);
background: rgba(239,68,68,0.07) !important;
}
.metric-card.alert-danger .metric-value { color: var(--danger) !important; }
.metric-card.alert-danger .metric-label { color: var(--danger); opacity: 0.85; }
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
loadActivePlantParams().then(() => loadData());
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
await fetch('/clear', { method: 'POST', headers: {'Authorization': 'Bearer ' + (localStorage.getItem('auth_token') || '')} });
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

// Параметры активного растения для сравнения (загружаются один раз при старте)
let activePlantParams = null;

async function loadActivePlantParams() {
const token = localStorage.getItem('auth_token');
if (!token) return;
try {
    const res = await fetch('/api/plants', { headers: { 'Authorization': 'Bearer ' + token } });
    if (!res.ok) return;
    const plants = await res.json();
    if (plants && plants.length > 0) {
        // Берём первое растение как «активное» (можно расширить на выбор)
        activePlantParams = plants[0];
    }
} catch (e) { console.log('Не удалось загрузить параметры растений'); }
}

function checkMetricAlert(cardEl, value, threshold) {
if (threshold == null || threshold === 0 || value == null) {
    cardEl.classList.remove('alert-danger');
    return;
}
if (value < threshold) {
    cardEl.classList.add('alert-danger');
} else {
    cardEl.classList.remove('alert-danger');
}
}

function updateMetrics(d) {
const tempEl   = document.getElementById('temp');
const humEl    = document.getElementById('hum');
const soilEl   = document.getElementById('soil');
const pressEl  = document.getElementById('pressure');

tempEl.textContent  = d.temp     ?? '--';
humEl.textContent   = d.humidity ?? '--';
soilEl.textContent  = d.soil     ?? '--';
pressEl.textContent = d.pressure ?? '--';

// Подсветка блоков красным если текущее значение хуже параметра растения
if (activePlantParams) {
    const p = activePlantParams;
    checkMetricAlert(tempEl.closest('.metric-card'),  d.temp,     p.temp     || null);
    checkMetricAlert(humEl.closest('.metric-card'),   d.humidity, p.humidity || null);
    checkMetricAlert(soilEl.closest('.metric-card'),  d.soil,     p.soil     || null);
    checkMetricAlert(pressEl.closest('.metric-card'), d.pressure, p.pressure || null);
}
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

        .falling-leaf {
            position: fixed;
            pointer-events: none;
            z-index: 9999;
            opacity: 1;
            animation: fallingLeaf var(--fall-duration, 3s) linear forwards;
            filter: drop-shadow(0 2px 3px rgba(0,0,0,0.1));
        }

        @keyframes fallingLeaf {
            0% {
                transform: translateY(0) translateX(0) rotate(0deg);
                opacity: 1;
            }
            25% {
                transform: translateY(25vh) translateX(var(--sway, 30px)) rotate(var(--rotation, 20deg));
            }
            50% {
                transform: translateY(50vh) translateX(calc(var(--sway, 30px) * -0.5)) rotate(calc(var(--rotation, 20deg) * -0.5));
                opacity: 0.9;
            }
            75% {
                transform: translateY(75vh) translateX(var(--sway, 30px)) rotate(var(--rotation, 20deg));
            }
            100% {
                transform: translateY(110vh) translateX(0) rotate(calc(var(--rotation, 20deg) * 1.5));
                opacity: 0;
            }
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

        .theme-switcher .theme-icon-img {
            width: 28px;
            height: 28px;
            object-fit: contain;
            filter: invert(0);
        }

        body.theme-light .theme-switcher {
            background-color: #1B263B;
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.35);
        }

        body.theme-light .theme-switcher .theme-icon-img {
            filter: invert(1);
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

        .brand-logo {
            width: 500px;
            max-width: 100%;
            height: auto;
            margin-bottom: 0px;
            display: block;
            margin-left: auto;
            margin-right: auto;
        }

        .logo-light {
            display: block;
        }

        .logo-dark {
            display: none;
        }

        body.theme-light .logo-light {
            display: none;
        }

        body.theme-light .logo-dark {
            display: block;
        }

        .auth-card {
            width: 16.67cm;
            height: 9.86cm;
            background-color: var(--card-bg);
            border-radius: 65px;
            padding: 28px 32px 32px;
            box-shadow: var(--shadow);
            transition: background-color 0.4s ease;
            display: flex;
            flex-direction: column;
            justify-content: center;
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
                height: auto;
                padding: 24px 20px;
            }
            .brand-logo { width: 85%; }
        }
    </style>
</head>
<body class="theme-dark">
    <div class="theme-switcher" id="themeSwitcher">
        <img src="/image/topic.png" alt="Переключить тему" class="theme-icon-img">
    </div>
    <div class="wrapper">
        <img src="/image/dark.png" alt="Зеленая полка" class="brand-logo logo-light">
        <img src="/image/light.png" alt="Зеленая полка" class="brand-logo logo-dark">
        <div class="auth-card">
            <h1>Авторизация</h1>
            <form id="loginForm" action="#" method="POST" onsubmit="event.preventDefault();">
                    <div class="form-group">
                        <input type="text" id="loginInput" name="login" placeholder="Логин" required autocomplete="username">
                    </div>
                    
                    <div class="form-group">
                        <input type="password" id="passwordInput" name="password" placeholder="Пароль" required autocomplete="current-password">
                        <img src="/image/open.png" alt="Показать пароль" class="toggle-password" id="togglePassword">
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
            if (isPassword) {
                this.src = '/image/closed.png';
            } else {
                this.src = '/image/open.png';
            }
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

        .falling-leaf {
            position: fixed;
            pointer-events: none;
            z-index: 9999;
            opacity: 1;
            animation: fallingLeaf var(--fall-duration, 3s) linear forwards;
            filter: drop-shadow(0 2px 3px rgba(0,0,0,0.1));
        }

        @keyframes fallingLeaf {
            0% {
                transform: translateY(0) translateX(0) rotate(0deg);
                opacity: 1;
            }
            25% {
                transform: translateY(25vh) translateX(var(--sway, 30px)) rotate(var(--rotation, 20deg));
            }
            50% {
                transform: translateY(50vh) translateX(calc(var(--sway, 30px) * -0.5)) rotate(calc(var(--rotation, 20deg) * -0.5));
                opacity: 0.9;
            }
            75% {
                transform: translateY(75vh) translateX(var(--sway, 30px)) rotate(var(--rotation, 20deg));
            }
            100% {
                transform: translateY(110vh) translateX(0) rotate(calc(var(--rotation, 20deg) * 1.5));
                opacity: 0;
            }
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

        .theme-switcher .theme-icon-img {
            width: 28px;
            height: 28px;
            object-fit: contain;
            filter: invert(0);
        }

        body.theme-light .theme-switcher {
            background-color: #1B263B;
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.35);
        }

        body.theme-light .theme-switcher .theme-icon-img {
            filter: invert(1);
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
            width: 16.67cm;
            min-height: 10cm;
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

        .field-icon {
            position: absolute;
            right: 14px;
            bottom: 10px;
            width: 24px;
            height: 24px;
            cursor: default;
            opacity: 0; 
            transition: opacity 0.3s ease;
            pointer-events: none;
        }

        .field-icon.show {
            opacity: 1;
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
        <img src="/image/topic.png" alt="Переключить тему" class="theme-icon-img">
    </div>

    <div class="wrapper">
        <div class="auth-card">
            <h1>Регистрация</h1>

            <form id="registerForm" action="#" method="POST" onsubmit="event.preventDefault();">
                
                <div class="form-group">
                    <label class="field-label">Логин<span class="required">*</span></label>
                    <input type="text" name="login" id="loginInput" placeholder="от 3 до 50 символов" required minlength="3" maxlength="50">
                </div>

                <div class="form-group">
                    <label class="field-label">Пароль<span class="required">*</span></label>
                    <input type="password" name="password" id="passwordInput" placeholder="от 6 символов" required minlength="6">
                    <img src="/image/warning-icon.png" alt="Внимание" class="field-icon" id="passwordWarning">
                </div>

                <div class="form-group">
                    <label class="field-label">Повторите пароль<span class="required">*</span></label>
                    <input type="password" name="password_confirm" id="passwordConfirmInput" placeholder="" required>
                    <img src="/image/warning-icon.png" alt="Внимание" class="field-icon" id="confirmWarning">
                </div>

                <div class="form-group">
                    <label class="field-label">Почта<span class="required">*</span></label>
                    <input type="email" id="emailInput" name="email" placeholder="your@email.com" required>
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
                    window.location.href = '/settings';
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
                <img src="/image/dpol.png" alt="Avatar Dark" class="user-avatar avatar-dark" id="header-avatar-dark">
                <img src="/image/lpol.png" alt="Avatar Light" class="user-avatar avatar-light" id="header-avatar-light">
                <span class="user-text" id="header-username">Загрузка...</span>
            </a>

            <div class="logo-container">
                <img src="/image/light.png" alt="Зелёная полка (Dark)" class="main-logo logo-dark">
                <img src="/image/dark.png" alt="Зелёная полка (Light)" class="main-logo logo-light">
            </div>

            <div class="theme-switcher" onclick="toggleTheme()">
                <input type="checkbox" id="theme-toggle" class="mode-toggle">
                <label for="theme-toggle">
                    <img src="/image/topic.png" alt="Тема" class="theme-icon-img">
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
                    <img src="/image/doordarkoff.png" alt="Выйти" class="logout-img logout-dark-default">
                    <img src="/image/doordarkon.png" alt="Выйти при наведении" class="logout-img logout-dark-hover">
                    <img src="/image/doorlightoff.png" alt="Выйти" class="logout-img logout-light-default">
                    <img src="/image/doorlighton.png" alt="Выйти при наведении" class="logout-img logout-light-hover">
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
                    <div class="profile-name" id="profile-name">Загрузка...</div>
                    <div class="profile-email" id="profile-email">Загрузка...</div>
                    
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

        const loadProfileFromServer = (retryCount = 0) => {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                window.location.href = '/login';
                return Promise.resolve(null);
            }
            return fetch('/api/profile', {
                headers: {'Authorization': 'Bearer ' + token}
            })
            .then(res => {
                if (res.status === 503 && retryCount < 6) {
                    // БД ещё не готова (SD-карта стартует) — повторить через 3 сек
                    return new Promise(resolve =>
                        setTimeout(() => resolve(loadProfileFromServer(retryCount + 1)), 3000)
                    );
                }
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
            if (!confirm('Вы уверены, что хотите удалить аккаунт? Это действие нельзя отменить.')) return;
            localStorage.removeItem('userData');
            window.location.href = '/register';
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
                <img src="/image/dpol.png" alt="User" class="user-avatar avatar-dark" id="header-avatar-dark">
                <img src="/image/lpol.png" alt="User" class="user-avatar avatar-light" id="header-avatar-light">
                <span class="user-text" id="header-username">Загрузка...</span>
            </div>
            
            <div class="logo-container">
                <img src="/image/light.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/dark.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>
            
            <div class="theme-switcher" onclick="toggleTheme()">
                <input type="checkbox" id="theme-toggle" class="mode-toggle">
                <label for="theme-toggle">
                    <img src="/image/topic.png" alt="Theme" class="theme-icon-img">
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
        // === ИСПРАВЛЕННЫЙ Хелпер для разрешения пути к аватару ===
        function resolveAvatarPath(path) {
            if (!path || path === '') {
                console.log('📷 Нет пути аватара, использую дефолтный');
                return '/image/dpol.png';
            }
            if (path.startsWith('http')) return path;
            // Убираем возможные дублирующиеся слеши и нормализуем путь
            let cleanPath = path.replace(/^\/+/, '');
            console.log('📷 Аватар путь:', cleanPath);
            return '/api/avatar/file?file=' + encodeURIComponent(cleanPath);
        }

        // === Загрузка профиля с сервера (с повтором при старте) ===
        async function loadProfileFromServer(retryCount = 0) {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                window.location.href = '/login';
                return null;
            }
            
            try {
                const res = await fetch('/api/profile', {
                    headers: {'Authorization': 'Bearer ' + token}
                });
                
                if (res.status === 503 && retryCount < 6) {
                    // БД ещё не готова — повторить через 3 сек
                    await new Promise(r => setTimeout(r, 3000));
                    return loadProfileFromServer(retryCount + 1);
                }
                
                if (res.status === 401) {
                    localStorage.removeItem('auth_token');
                    window.location.href = '/login';
                    return null;
                }
                
                const profile = await res.json();
                return profile;
            } catch (e) {
                console.error('Profile load error:', e);
                return null;
            }
        }
        
        let tempAvatarData = null;
        let tempAvatarFile = null; // Добавляем переменную для файла

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
            console.log('🚀 Страница загружена, загружаем профиль...');
            
            // Проверяем токен при загрузке
            const token = localStorage.getItem('auth_token');
            if (!token) {
                console.log('❌ Нет токена, перенаправление на /login');
                window.location.href = '/login';
                return;
            }
            
            const profile = await loadProfileFromServer();
            if (!profile) return;
            
            // Заполняем поля из серверного ответа
            const fields = {
                'header-username': profile.username,
                'edit-username': profile.username,
                'edit-email': profile.email,
                'edit-timezone': profile.timezone
            };
            
            Object.keys(fields).forEach(id => {
                const el = document.getElementById(id);
                if (el && fields[id]) {
                    if (id === 'edit-timezone') {
                        el.value = fields[id];
                    } else if (el.tagName === 'INPUT') {
                        el.value = fields[id];
                    } else {
                        el.textContent = fields[id];
                    }
                    console.log(`✅ Установлено ${id}:`, fields[id]);
                } else if (!el) {
                    console.warn(`⚠️ Элемент ${id} не найден в DOM`);
                }
            });
            
            // === ИСПРАВЛЕННО: Аватары с обработкой ошибок ===
            const avatarSrc = resolveAvatarPath(profile.avatar);
            console.log('📷 Устанавливаю аватар:', avatarSrc);
            
            // Обновляем все элементы аватара
            const avatarElements = ['header-avatar-dark', 'header-avatar-light', 'profile-avatar'];
            avatarElements.forEach(id => {
                const el = document.getElementById(id);
                if (el) {
                    el.src = avatarSrc;
                    // Добавляем обработчик ошибки загрузки
                    el.onerror = function() {
                        console.log('⚠️ Не удалось загрузить аватар:', this.src);
                        this.src = '/image/dpol.png';
                    };
                    // Добавляем обработчик успешной загрузки
                    el.onload = function() {
                        console.log('✅ Аватар загружен:', this.src);
                    };
                }
            });
            
            // Пол
            if (profile.gender) {
                    const femaleEdit = document.getElementById('edit-gender-female');
                    const maleEdit = document.getElementById('edit-gender-male');
                    
                    if (femaleEdit && maleEdit) {
                        if (profile.gender === 'female') {
                            femaleEdit.classList.add('selected');
                            maleEdit.classList.remove('selected');
                        } else {
                            femaleEdit.classList.remove('selected');
                            maleEdit.classList.add('selected');
                        }
                        console.log(`✅ Установлен пол: ${profile.gender}`);
                    }
                }
            
            // Тема
            const savedTheme = localStorage.getItem('theme') || 'dark';
            document.body.classList.remove('theme-dark', 'theme-light');
            document.body.classList.add('theme-' + savedTheme);
        });

        function openAvatarModal() {
            console.log('🖼️ Открытие модального окна аватара');
            document.getElementById('avatar-modal').classList.add('active');
            document.getElementById('avatar-modal').classList.remove('has-preview');
            document.getElementById('hidden-file-input').value = '';
            tempAvatarData = null;
            tempAvatarFile = null;
        }
        
        function closeAvatarModal() {
            console.log('🔒 Закрытие модального окна аватара');
            document.getElementById('avatar-modal').classList.remove('active');
        }

        // === ИСПРАВЛЕННЫЙ обработчик выбора файла ===
        document.getElementById('hidden-file-input').addEventListener('change', function(e) {
            const file = e.target.files[0];
            if (!file) return;
            
            console.log('📁 Выбран файл:', file.name, 'тип:', file.type, 'размер:', file.size);
            
            // Проверяем поддерживаемые форматы
            const supportedTypes = ['image/jpeg', 'image/jpg', 'image/png', 'image/gif', 'image/webp', 'image/bmp'];
            
            if (!supportedTypes.includes(file.type)) {
                alert('Неподдерживаемый формат! Используйте: JPG, PNG, GIF, WEBP или BMP');
                this.value = '';
                return;
            }
            
            if (file.size > 2 * 1024 * 1024) {
                alert('Файл слишком большой! Максимум 2MB');
                this.value = '';
                return;
            }
            
            // Сохраняем оригинальный файл для отправки
            tempAvatarFile = file;
            
            const reader = new FileReader();
            reader.onload = function(ev) {
                tempAvatarData = ev.target.result;
                document.getElementById('modal-preview-img').src = tempAvatarData;
                document.getElementById('modal-preview-img').style.display = 'block';
                document.getElementById('avatar-modal').classList.add('has-preview');
                console.log('✅ Превью аватара создано');
            };
            reader.onerror = function() {
                console.error('❌ Ошибка чтения файла');
                alert('Ошибка чтения файла');
            };
            reader.readAsDataURL(file);
        });

        // === ИСПРАВЛЕННАЯ И УЛУЧШЕННАЯ функция загрузки аватара ===
        const confirmAvatarUpload = async () => {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                alert('Требуется авторизация');
                return;
            }
            
            if (!tempAvatarFile) {
                alert('Выберите файл для загрузки');
                return;
            }
            
            console.log('📤 Отправка аватара:', tempAvatarFile.name);
            
            const formData = new FormData();
            formData.append('file', tempAvatarFile, tempAvatarFile.name);
            
            try {
                const res = await fetch('/api/avatar', {
                    method: 'POST',
                    headers: {
                        'Authorization': 'Bearer ' + token
                    },
                    body: formData
                });
                
                console.log('📡 Ответ сервера:', res.status, res.statusText);
                
                if (!res.ok) {
                    const errorText = await res.text();
                    console.error('Ошибка от сервера:', errorText);
                    throw new Error(`HTTP ${res.status}`);
                }
                
                const data = await res.json();
                console.log('📦 Ответ сервера:', data);
                
                if (data.success && data.avatar) {
                    console.log('✅ Аватар успешно загружен:', data.avatar);
                    
                    const newSrc = resolveAvatarPath(data.avatar) + '&t=' + Date.now();
                    
                    // Обновляем все аватары на странице
                    const avatarElements = ['profile-avatar', 'header-avatar-dark', 'header-avatar-light'];
                    avatarElements.forEach(id => {
                        const el = document.getElementById(id);
                        if (el) {
                            el.src = newSrc;
                        }
                    });
                    
                    closeAvatarModal();
                    alert('✅ Аватар успешно обновлён!');
                    
                    // Перезагружаем профиль для синхронизации
                    await loadProfileFromServer();
                } else {
                    alert('Ошибка: ' + (data.error || 'Неизвестная ошибка сервера'));
                }
            } catch (e) {
                console.error('❌ Ошибка при загрузке аватара:', e);
                // Мягкое уведомление вместо пугающего алерта
                alert('Не удалось загрузить аватар. Проверьте интернет-соединение и попробуйте ещё раз.');
            }
        };

        // === ИСПРАВЛЕННАЯ функция сохранения изменений профиля ===
        async function saveChanges() {
            const token = localStorage.getItem('auth_token');
            if (!token) { 
                window.location.href = '/login'; 
                return; 
            }
            
            const username = document.getElementById('edit-username').value.trim();
            const email = document.getElementById('edit-email').value.trim();
            const timezone = document.getElementById('edit-timezone').value;
            const gender = document.getElementById('edit-gender-female').classList.contains('selected') ? 'female' : 'male';
            
            if (!username || username.length < 3) {
                alert('Имя пользователя должно содержать минимум 3 символа');
                return;
            }
            
            if (!email || !email.includes('@')) {
                alert('Введите корректный email');
                return;
            }
            
            const payload = { username, email, gender, timezone };
            console.log('📤 Сохранение профиля:', payload);
            
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
                console.log('📡 Ответ сервера:', data);
                
                if (data.success) {
                    alert('✅ Профиль успешно сохранен!');
                    window.location.href = '/profile';
                } else {
                    alert('❌ Ошибка: ' + (data.error || 'Неизвестная ошибка'));
                }
            } catch (e) {
                console.error('❌ Ошибка сети:', e);
                alert('Ошибка соединения с сервером');
            }
        }

        // === СМЕНА ПАРОЛЯ ===
        async function changePassword() {
            const oldPass = document.getElementById('old-password').value;
            const newPass = document.getElementById('new-password').value;
            const confirmPass = document.getElementById('confirm-new-password').value;

            if (newPass !== confirmPass) {
                alert('Новые пароли не совпадают');
                return;
            }
            if (newPass.length < 6) {
                alert('Новый пароль должен быть не менее 6 символов');
                return;
            }

            const token = getAuthToken();
            try {
                const res = await fetch('/api/change-password', {
                    method: 'POST',
                    headers: {
                        'Authorization': 'Bearer ' + token,
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({
                        oldPassword: oldPass,
                        newPassword: newPass
                    })
                });

                const data = await res.json();
                if (data.success) {
                    alert('✅ Пароль успешно изменён');
                    // Очищаем поля
                    document.getElementById('old-password').value = '';
                    document.getElementById('new-password').value = '';
                    document.getElementById('confirm-new-password').value = '';
                } else {
                    alert('Ошибка: ' + (data.error || 'Не удалось сменить пароль'));
                }
            } catch (e) {
                console.error(e);
                alert('Ошибка соединения с сервером');
            }
        }

        // === УДАЛЕНИЕ АККАУНТА ===
        async function deleteAccount() {
            if (!confirm('ВЫ УВЕРЕНЫ? Это действие нельзя отменить! Все данные будут удалены.')) {
                return;
            }
            if (!confirm('ПОСЛЕДНЕЕ ПОДТВЕРЖДЕНИЕ: Удалить аккаунт навсегда?')) {
                return;
            }

            const token = getAuthToken();
            try {
                const res = await fetch('/api/delete-account', {
                    method: 'POST',
                    headers: {
                        'Authorization': 'Bearer ' + token,
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ confirm: "DELETE" })
                });

                const data = await res.json();
                if (data.success) {
                    alert('Аккаунт успешно удалён');
                    localStorage.removeItem('auth_token');
                    window.location.href = '/login';
                } else {
                    alert('Ошибка: ' + (data.error || 'Не удалось удалить аккаунт'));
                }
            } catch (e) {
                console.error(e);
                alert('Ошибка соединения');
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
        :root { --card-radius: 24px; --transition-speed: 0.4s; }
        body.theme-dark {
            --page-bg: #0F182B; --text-main: #00674B; --accent-green: #21C85F;
            --card-bg: rgba(255, 255, 255, 0.95); --glow-color: rgba(33, 200, 95, 0.6);
            --glow-soft: rgba(33, 200, 95, 0.3); --user-pill-bg: #1a2d45;
            --user-pill-text: #a0aab5; --nav-bg: #21C85F;
            --nav-btn-bg: rgba(255, 255, 255, 0.25); --label-bg: rgba(33, 200, 95, 0.15);
            --label-text: #21C85F; --footer-bg: #21C85F; --switcher-bg: #1a2d45;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.35);
            --body-bg: var(--page-bg); --title-color: var(--text-main);
            --green-primary: var(--accent-green); --green-hover: #1aab4e;
            --green-glow: var(--accent-green); --card-glow: var(--glow-color);
            --card-shadow: 0 0 20px var(--glow-soft); --subtitle-color: #7f8c8d;
            --network-row-bg: var(--label-bg); --network-row-border: transparent;
            --manual-section-bg: var(--card-bg); --input-border: rgba(33, 200, 95, 0.4);
            --input-focus-border: var(--accent-green); --input-bg: #ffffff;
            --label-color: #95a5a6; --btn-scan-bg: var(--accent-green);
            --btn-scan-text: #ffffff; --btn-connect-bg: var(--accent-green);
            --btn-connect-text: #ffffff; --btn-disconnect-bg: #ffffff;
            --btn-disconnect-text: #0a4d3a; --footer-text: #ffffff;
            --modal-overlay: rgba(15, 24, 43, 0.6); --modal-blur: blur(8px);
            --section-list-bg: var(--label-bg); --section-list-shadow: var(--card-internal-shadow);
            --plus-icon-bg: var(--accent-green); --section-glow: var(--glow-color);
            --empty-row-bg: rgba(33, 200, 95, 0.12); --modal-bg: var(--card-bg);
            --modal-input-bg: #ffffff; --footer-column-title: #ffffff;
            --footer-column-text: rgba(255,255,255,0.9); --footer-copy: rgba(255,255,255,0.8);
        }
        body.theme-light {
            --page-bg: #E8F0F2; --text-main: #00674B; --accent-green: #10B981;
            --card-bg: #ffffff; --glow-color: rgba(16, 185, 129, 0.4);
            --glow-soft: rgba(16, 185, 129, 0.2); --user-pill-bg: #ffffff;
            --user-pill-text: #8899aa; --nav-bg: #10B981;
            --nav-btn-bg: rgba(255, 255, 255, 0.3); --label-bg: rgba(16, 185, 129, 0.15);
            --label-text: #10B981; --footer-bg: #10B981; --switcher-bg: #ffffff;
            --card-internal-shadow: inset 0 0 20px rgba(0, 0, 0, 0.2);
            --body-bg: var(--page-bg); --title-color: var(--text-main);
            --green-primary: var(--accent-green); --green-hover: #0d9668;
            --green-glow: var(--accent-green); --card-glow: var(--glow-soft);
            --card-shadow: 0 0 18px var(--glow-soft); --subtitle-color: #7f8c8d;
            --network-row-bg: var(--label-bg); --network-row-border: transparent;
            --manual-section-bg: var(--card-bg); --input-border: rgba(16, 185, 129, 0.35);
            --input-focus-border: var(--accent-green); --input-bg: #ffffff;
            --label-color: #a0a0a0; --btn-scan-bg: #78d5b2; --btn-scan-text: #0a4d3a;
            --btn-connect-bg: var(--accent-green); --btn-connect-text: #ffffff;
            --btn-disconnect-bg: #ffffff; --btn-disconnect-text: #0a4d3a;
            --footer-text: #ffffff; --modal-overlay: rgba(232, 240, 242, 0.7);
            --modal-blur: blur(8px); --section-list-bg: var(--label-bg);
            --section-list-shadow: var(--card-internal-shadow);
            --plus-icon-bg: var(--accent-green); --section-glow: var(--glow-soft);
            --empty-row-bg: rgba(16, 185, 129, 0.1); --modal-bg: var(--card-bg);
            --modal-input-bg: #ffffff; --footer-column-title: #ffffff;
            --footer-column-text: rgba(255,255,255,0.9); --footer-copy: rgba(255,255,255,0.8);
        }
        *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;
            background: var(--body-bg); color: var(--title-color); min-height: 100vh;
            transition: background var(--transition-speed) ease, color var(--transition-speed) ease;
            line-height: 1.5;
        }
        body.setup-mode .user-pill,
        body.setup-mode .nav-btn:not(.active) {
            pointer-events: none !important; opacity: 0.4 !important;
            cursor: not-allowed !important; filter: grayscale(0.6) !important;
        }
        body:not(.setup-mode) .manual-section {
            pointer-events: none !important; opacity: 0.5 !important;
            filter: grayscale(0.6) !important; cursor: not-allowed !important;
        }
        body.setup-mode .btn-scan,
        body.setup-mode .btn-connect,
        body.setup-mode .btn-add-network {
            pointer-events: auto !important; opacity: 1 !important;
            cursor: pointer !important; filter: none !important;
        }
        .mode-toggle { display: none !important; position: absolute; opacity: 0; pointer-events: none; }
        .page-wrapper { display: flex; flex-direction: column; min-height: calc(100vh - 200px); }
        .page-wrapper > .main-content { flex: 1 0 auto; }
        .top-sticky-wrapper { position: sticky; top: 0; z-index: 1000; width: 100%; background: transparent; pointer-events: none; }
        .top-sticky-wrapper > * { pointer-events: auto; }
        .site-header {
            display: flex; align-items: center; justify-content: space-between;
            padding: 20px 40px; background: transparent; backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px); pointer-events: auto; position: relative; z-index: 2;
        }
        .user-pill {
            display: flex; align-items: center; text-decoration: none;
            background: var(--user-pill-bg); border-radius: 50px;
            padding: 8px 16px 8px 8px; gap: 10px; width: 145px; height: 44px;
            box-shadow: 0 0 8px var(--glow-soft); animation: userPulse 3s infinite alternate; flex-shrink: 0;
        }
        @keyframes userPulse { 0% { box-shadow: 0 0 6px var(--glow-soft); } 100% { box-shadow: 0 0 16px var(--glow-color); } }
        .user-avatar { width: 32px; height: 32px; border-radius: 50%; object-fit: cover; display: none; }
        body.theme-dark .avatar-dark { display: block; } body.theme-light .avatar-light { display: block; }
        .user-text { font-size: 14px; font-weight: 400; color: var(--user-pill-text); letter-spacing: 0.5px; }
        .logo-container { position: absolute; left: 50%; transform: translateX(-50%); display: flex; align-items: center; }
        .main-logo { height: 100px; display: none; transition: height 0.3s ease; }
        body.theme-dark .logo-dark { display: block; } body.theme-light .logo-light { display: block; }
        .theme-switcher {
            width: 48px; height: 48px; background: var(--switcher-bg); border-radius: 14px;
            display: flex; align-items: center; justify-content: center; cursor: pointer;
            box-shadow: 0 0 10px var(--glow-soft); animation: switcherPulse 3s infinite alternate; flex-shrink: 0;
        }
        @keyframes switcherPulse { 0% { box-shadow: 0 0 8px var(--glow-soft); } 100% { box-shadow: 0 0 18px var(--glow-color); } }
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
            padding: 8px 22px; border-radius: 30px; transition: all 0.3s ease; white-space: nowrap;
            position: relative; background: rgba(255, 255, 255, 0.1); backdrop-filter: blur(4px);
            -webkit-backdrop-filter: blur(4px); border: 1px solid rgba(255, 255, 255, 0.1);
        }
        .nav-btn:hover, .nav-btn.active {
            background: rgba(255, 255, 255, 0.35); backdrop-filter: blur(10px);
            -webkit-backdrop-filter: blur(10px); border: 1px solid rgba(255, 255, 255, 0.25);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }
        .main-content { max-width: 1100px; width: 100%; margin: 0 auto; padding: 0 30px 40px; }
        .scan-card {
            background: var(--card-bg); border-radius: var(--card-radius); padding: 60px 40px 50px;
            text-align: center; box-shadow: var(--card-shadow); margin-bottom: 30px;
            transition: background var(--transition-speed) ease, box-shadow var(--transition-speed) ease;
            position: relative;
        }
        .scan-card::before {
            content: ''; position: absolute; inset: -2px; border-radius: calc(var(--card-radius) + 2px);
            box-shadow: 0 0 25px var(--card-glow); z-index: -1;
            transition: box-shadow var(--transition-speed) ease;
        }
        .wifi-icon { width: 150px; height: 150px; margin: 0 auto 20px; display: flex; align-items: center; justify-content: center; }
        .wifi-img { width: 100%; height: 100%; object-fit: contain; display: none; transition: opacity var(--transition-speed) ease; }
        body.theme-dark .wifi-dark { display: block; } body.theme-light .wifi-light { display: block; }
        .scan-title { font-size: 26px; font-weight: 700; color: var(--title-color); margin-bottom: 10px; }
        .scan-subtitle { font-size: 16px; color: var(--subtitle-color); margin-bottom: 30px; }
        .btn-scan {
            display: inline-block; background: var(--btn-scan-bg); color: var(--btn-scan-text);
            border: none; border-radius: 40px; padding: 16px 50px; font-size: 18px;
            font-weight: 600; cursor: pointer; transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(0, 0, 0, 0.15); letter-spacing: 0.3px;
        }
        .btn-scan:hover { transform: translateY(-2px); box-shadow: 0 6px 22px rgba(0, 0, 0, 0.2); filter: brightness(1.05); }
        .btn-scan:active { transform: translateY(0); }
        .btn-scan:disabled { opacity: 0.7; cursor: not-allowed; transform: none !important; }
        .btn-return-ap {
            display: none; margin: 15px auto 0; background: #ffffff; color: #e55a5a;
            border: 1.5px solid #ffb3b3; border-radius: 40px; padding: 14px 40px;
            font-size: 16px; font-weight: 600; cursor: pointer; transition: all 0.3s ease;
            box-shadow: 0 0 15px rgba(229, 90, 90, 0.25);
        }
        .btn-return-ap:hover {
            transform: scale(1.03); background: #fff5f5; color: #d32f2f;
            border-color: #ff8a8a; box-shadow: 0 0 25px rgba(229, 90, 90, 0.6), 0 0 10px rgba(229, 90, 90, 0.4);
        }
        .network-list-section {
            background: var(--section-list-bg); border-radius: 16px; padding: 20px;
            margin-top: 35px; box-shadow: var(--section-list-shadow);
        }
        .network-item {
            display: flex; align-items: center; padding: 16px 20px; border-radius: 14px;
            margin-bottom: 12px; background: var(--network-row-bg);
        }
        .network-item:last-child { margin-bottom: 0; }
        .network-item.empty { background: var(--empty-row-bg); }
        .network-icon { width: 22px; height: 22px; margin-right: 14px; flex-shrink: 0; display: flex; align-items: center; justify-content: center; }
        .wifi-img-small { width: 100%; height: 100%; object-fit: contain; display: none; }
        body.theme-dark .wifi-dark { display: block; } body.theme-light .wifi-light { display: block; }
        .network-name { font-size: 16px; font-weight: 600; color: var(--title-color); min-width: 80px; }
        .network-security { font-size: 15px; color: var(--subtitle-color); flex: 1; padding-left: 10px; }
        .btn-connect, .btn-disconnect {
            border: none; border-radius: 30px; padding: 8px 24px; font-size: 14px;
            font-weight: 600; cursor: pointer; transition: all 0.25s ease; white-space: nowrap; flex-shrink: 0;
        }
        .btn-connect { background: var(--btn-connect-bg); color: var(--btn-connect-text); }
        .btn-connect:hover { filter: brightness(1.1); transform: translateY(-1px); }
        .btn-disconnect { background: var(--btn-disconnect-bg); color: var(--btn-disconnect-text); box-shadow: 0 2px 6px rgba(0,0,0,0.1); }
        .btn-disconnect:hover { transform: translateY(-1px); box-shadow: 0 3px 10px rgba(0,0,0,0.15); }
        .btn-add-network {
            display: inline-block; background: var(--btn-scan-bg); color: var(--btn-scan-text);
            border: none; border-radius: 40px; padding: 12px 32px; font-size: 16px;
            font-weight: 600; cursor: pointer; transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(0, 0, 0, 0.15); letter-spacing: 0.3px;
        }
        .btn-add-network:hover { transform: translateY(-2px); box-shadow: 0 6px 22px rgba(0, 0, 0, 0.2); filter: brightness(1.05); }
        .btn-add-network:disabled { opacity: 0.7; cursor: not-allowed; transform: none !important; }
        .manual-section {
            background: var(--manual-section-bg); border-radius: var(--card-radius); padding: 50px 50px 55px;
            box-shadow: var(--card-shadow); margin-bottom: 30px; position: relative;
        }
        .manual-section::before {
            content: ''; position: absolute; inset: -2px; border-radius: calc(var(--card-radius) + 2px);
            box-shadow: 0 0 20px var(--card-glow); z-index: -1;
        }
        .manual-title {
            display: flex; align-items: center; font-size: 24px; font-weight: 700;
            color: var(--title-color); margin-bottom: 30px;
        }
        .plus-icon {
            width: 28px; height: 28px; background: var(--plus-icon-bg); border-radius: 50%;
            display: inline-flex; align-items: center; justify-content: center; margin-right: 12px; flex-shrink: 0;
        }
        .plus-icon svg { width: 16px; height: 16px; }
        .plus-icon svg line { stroke: #ffffff; stroke-width: 2.5; stroke-linecap: round; }
        .manual-form { display: grid; grid-template-columns: 1fr 1fr; gap: 24px 40px; }
        .form-group { display: flex; flex-direction: column; }
        .form-group.full-width { grid-column: 1 / -1; }
        .form-label { font-size: 16px; color: var(--label-color); margin-bottom: 10px; font-weight: 400; }
        .form-input, .form-select {
            border: 2px solid var(--input-border); border-radius: 20px; padding: 16px 22px;
            font-size: 16px; color: var(--title-color); background: var(--input-bg);
            outline: none; transition: border-color 0.3s ease;
        }
        .form-input::placeholder { color: var(--label-color); }
        .form-input:focus, .form-select:focus { border-color: var(--input-focus-border); }
        .site-footer { background: var(--footer-bg); color: var(--footer-text); padding: 36px 40px 0; flex-shrink: 0; }
        .footer-columns { display: grid; grid-template-columns: 1.3fr 1fr 0.8fr; gap: 30px; max-width: 900px; margin: 0 auto; padding-bottom: 24px; border-bottom: 1px solid rgba(255,255,255,0.2); }
        .footer-column-title { font-size: 20px; font-weight: 700; margin-bottom: 12px; color: var(--footer-column-title); }
        .footer-column-text { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-column-list { list-style: none; padding: 0; }
        .footer-column-list li { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-copyright { text-align: center; padding: 16px 0 12px; font-size: 14px; color: var(--footer-copy); max-width: 900px; margin: 0 auto; }
        .modal-overlay {
            display: none; position: fixed; inset: 0; background: var(--modal-overlay);
            backdrop-filter: var(--modal-blur); -webkit-backdrop-filter: var(--modal-blur);
            z-index: 2000; align-items: center; justify-content: center;
        }
        .modal-overlay.active { display: flex; }
        .modal-card {
            background: var(--modal-bg); border-radius: 20px; padding: 28px 32px;
            width: 90%; max-width: 480px; box-shadow: 0 8px 40px rgba(0,0,0,0.15); position: relative;
        }
        .modal-network-row {
            display: flex; align-items: center; padding: 10px 14px; border-radius: 14px;
            background: var(--network-row-bg); margin-bottom: 18px;
        }
        .modal-network-name { font-size: 15px; font-weight: 600; color: var(--title-color); }
        .modal-network-security { font-size: 14px; color: var(--subtitle-color); margin-left: 16px; }
        .modal-label { font-size: 14px; color: var(--label-color); margin-bottom: 8px; display: block; }
        .modal-input-wrapper { position: relative; display: flex; align-items: center; }
        .modal-input {
            width: 100%; border: 2px solid var(--input-border); border-radius: 16px;
            padding: 12px 48px 12px 18px; font-size: 15px; color: var(--title-color);
            background: var(--modal-input-bg); outline: none;
            transition: border-color 0.3s ease;
        }
        .modal-input:focus { border-color: var(--input-focus-border); }
        .modal-eye-btn {
            position: absolute; right: 14px; background: none; border: none;
            cursor: pointer; padding: 4px; display: flex; align-items: center; justify-content: center;
        }
        #eyeImg { width: 22px; height: 22px; object-fit: contain; }
        .modal-actions { display: flex; gap: 12px; justify-content: center; margin-top: 24px; }
        .btn-cancel-return {
            background: var(--btn-disconnect-bg); color: var(--btn-disconnect-text);
            border: 1.5px solid var(--input-border); border-radius: 30px; padding: 12px 24px;
            font-size: 15px; font-weight: 600; cursor: pointer; transition: all 0.3s ease;
        }
        .btn-cancel-return:hover { transform: scale(1.03); filter: brightness(0.95); }
        .btn-confirm-return {
            background: #ffffff; color: #e55a5a; border: 1.5px solid #ffb3b3;
            border-radius: 30px; padding: 12px 24px; font-size: 15px; font-weight: 600;
            cursor: pointer; transition: all 0.3s ease; box-shadow: 0 0 15px rgba(229, 90, 90, 0.25);
        }
        .btn-confirm-return:hover {
            transform: scale(1.03); background: #fff5f5; color: #d32f2f;
            border-color: #ff8a8a; box-shadow: 0 0 25px rgba(229, 90, 90, 0.6);
        }
        #connectStatus { margin-top: 12px; font-size: 14px; text-align: center; min-height: 20px; }
        #connectStatus.success { color: var(--accent-green); }
        #connectStatus.error { color: #e55a5a; }
        @media (max-width: 700px) {
            .page-wrapper { min-height: calc(100vh - 170px); }
            .site-header { padding: 14px 16px; }
            .main-logo { height: 60px; }
            .navigation { margin: 0 12px 24px; height: 44px; gap: 4px; }
            .nav-btn { font-size: 14px; padding: 6px 14px; }
            .main-content { padding: 0 14px 30px; max-width: 100%; }
            .scan-card { padding: 40px 24px 30px; }
            .manual-section { padding: 25px 20px 30px; }
            .wifi-icon { width: 72px; height: 56px; }
            .scan-title { font-size: 20px; }
            .btn-scan { padding: 12px 36px; font-size: 15px; }
            .network-item { padding: 10px 12px; flex-wrap: wrap; gap: 6px 0; }
            .manual-form { grid-template-columns: 1fr; gap: 14px; }
            .footer-columns { grid-template-columns: 1fr; gap: 20px; }
            .site-footer { padding: 28px 20px 0; }
        }
        @keyframes fadeInUp { from { opacity: 0; transform: translateY(16px); } to { opacity: 1; transform: translateY(0); } }
        .scan-card { animation: fadeInUp 0.5s ease forwards; }
        .manual-section { animation: fadeInUp 0.5s ease 0.15s forwards; opacity: 0; animation-fill-mode: forwards; }
        .site-footer { animation: fadeInUp 0.5s ease 0.3s forwards; opacity: 0; animation-fill-mode: forwards; }
    </style>
</head>
<body class="theme-dark">
    <div class="top-sticky-wrapper">
        <header class="site-header">
            <a href="/profile" class="user-pill" aria-label="Профиль">
                <img src="/image/dpol.png" alt="" class="user-avatar avatar-dark">
                <img src="/image/lpol.png" alt="" class="user-avatar avatar-light">
                <span class="user-text">Загрузка...</span>
            </a>
            <div class="logo-container">
                <img src="/image/light.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/dark.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>
            <label class="theme-switcher" for="themeToggle" aria-label="Сменить тему">
                <img src="/image/topic.png" alt="" class="theme-icon-img">
            </label>
        </header>
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
                    <img src="/image/light_big.png" alt="WiFi" class="wifi-img wifi-light">
                    <img src="/image/dark_big.png" alt="WiFi" class="wifi-img wifi-dark">
                </div>
                <h1 class="scan-title">Сканирование сетей</h1>
                <p class="scan-subtitle">Нажмите кнопку ниже для сканирования доступных Wi-Fi сетей</p>
                <button class="btn-scan" id="scanBtn">Начать сканирование</button>
                <button class="btn-return-ap" id="returnApBtn">Вернуться к настройкам Wi-Fi</button>
                <div class="network-list-section" id="networkList" style="display: none;"></div>
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
                        <input type="text" id="wifiSsid" class="form-input" placeholder="">
                    </div>
                    <div class="form-group">
                        <label class="form-label">Тип безопасности</label>
                        <select id="wifiSec" class="form-select">
                            <option value="WPA2">WPA/WPA2-Personal</option>
                            <option value="WPA">WPA-Personal</option>
                            <option value="WEP">WEP</option>
                            <option value="OPEN">Открытая (без пароля)</option>
                        </select>
                    </div>
                    <div class="form-group full-width" id="passGroup">
                        <label class="form-label">Пароль</label>
                        <input type="password" id="wifiPass" class="form-input" placeholder="">
                    </div>
                    <div class="form-group full-width" style="margin-top: 16px; display: flex; justify-content: flex-end;">
                        <button type="button" class="btn-add-network" id="addNetworkBtn">Добавить</button>
                    </div>
                </div>
                <div id="connectStatus"></div>
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
            <div class="footer-copyright">2026 Зелёная полка</div>
        </footer>
    </div>

    <!-- Модальное окно ввода пароля сети -->
    <div class="modal-overlay" id="passwordModal">
        <div class="modal-card">
            <div class="modal-network-row">
                <div class="network-icon">
                    <img src="/image/light_small.png" alt="WiFi" class="wifi-img-small wifi-light">
                    <img src="/image/dark_small.png" alt="WiFi" class="wifi-img-small wifi-dark">
                </div>
                <span class="modal-network-name" id="modalNetworkName">Сеть</span>
                <span class="modal-network-security">Защищено</span>
            </div>
            <label class="modal-label">Введите код безопасности</label>
            <div class="modal-input-wrapper">
                <input type="password" class="modal-input" id="modalPasswordInput">
                <button type="button" class="modal-eye-btn" id="eyeBtn">
                    <img src="/image/Open.png" id="eyeImg" alt="Показать пароль">
                </button>
            </div>
        </div>
    </div>

    <!-- Модальное окно возврата к AP -->
    <div class="modal-overlay" id="returnApModal">
        <div class="modal-card" style="text-align: center;">
            <div class="wifi-icon" style="width: 64px; height: 64px; margin: 0 auto 15px;">
                <img src="/image/light_big.png" alt="WiFi" class="wifi-img wifi-light" style="width:100%; height:100%;">
                <img src="/image/dark_big.png" alt="WiFi" class="wifi-img wifi-dark" style="width:100%; height:100%;">
            </div>
            <h3 style="font-size: 20px; font-weight: 700; color: var(--title-color); margin-bottom: 12px;">Возврат к настройкам Wi-Fi</h3>
            <p style="font-size: 15px; color: var(--subtitle-color); margin-bottom: 24px; line-height: 1.5;">
                Для возврата к настройкам вам необходимо вручную подключиться к сети <strong>"GreenShelf_Setup"</strong> на вашем устройстве.<br><br>
                После подключения вы будете автоматически перенаправлены на эту страницу.
            </p>
            <div class="modal-actions">
                <button class="btn-cancel-return" id="cancelReturnApBtn">Отмена</button>
                <button class="btn-confirm-return" id="confirmReturnApBtn">Понятно, перейти</button>
            </div>
        </div>
    </div>

    <script>
        // ===== ЗАГРУЗКА ПРОФИЛЯ С СЕРВЕРА =====
        async function loadProfileFromServer(retryCount = 0) {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                return null;
            }
            try {
                const res = await fetch('/api/profile', {
                    headers: {'Authorization': 'Bearer ' + token}
                });
                if (res.status === 503 && retryCount < 6) {
                    await new Promise(r => setTimeout(r, 3000));
                    return loadProfileFromServer(retryCount + 1);
                }
                if (res.status === 401) {
                    localStorage.removeItem('auth_token');
                    window.location.href = '/login';
                    return null;
                }
                const profile = await res.json();
                return profile;
            } catch (e) {
                console.error('Profile load error:', e);
                return null;
            }
        }

        function resolveAvatarPath(path) {
            if (!path || path === '') return '/image/dpol.png';
            if (path.startsWith('http')) return path;
            let cleanPath = path.replace(/^\/+/, '');
            return '/api/avatar/file?file=' + encodeURIComponent(cleanPath);
        }

        async function loadUserToHeader() {
            const profile = await loadProfileFromServer();
            if (!profile) return;
            const userText = document.querySelector('.user-pill .user-text');
            if (userText) userText.textContent = profile.username;
            const avatarSrc = resolveAvatarPath(profile.avatar);
            const avDark = document.querySelector('.user-pill .avatar-dark');
            const avLight = document.querySelector('.user-pill .avatar-light');
            if (avDark) { avDark.src = avatarSrc; avDark.onerror = () => { avDark.src = '/image/dpol.png'; }; }
            if (avLight) { avLight.src = avatarSrc; avLight.onerror = () => { avLight.src = '/image/dpol.png'; }; }
        }

        // ===== ТЕМА =====
        (function initTheme() {
            const toggle = document.getElementById('themeToggle');
            const body = document.body;
            const KEY = 'theme';
            const saved = localStorage.getItem(KEY) || 'dark';
            body.classList.remove('theme-dark', 'theme-light');
            body.classList.add('theme-' + saved);
            if (toggle) toggle.checked = (saved === 'light');
            if (toggle) {
                toggle.addEventListener('change', function() {
                    const next = toggle.checked ? 'light' : 'dark';
                    body.classList.remove('theme-dark', 'theme-light');
                    body.classList.add('theme-' + next);
                    localStorage.setItem(KEY, next);
                });
            }
        })();

        // ===== ПРОВЕРКА АВТОРИЗАЦИИ =====
        function checkAuth() {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                window.location.href = '/login';
                return false;
            }
            return true;
        }

        // ===== WiFi ЛОГИКА =====
        document.addEventListener('DOMContentLoaded', function() {
            if (!checkAuth()) return;
            loadUserToHeader();

            const isSetupMode = window.location.hostname === '192.168.10.1' || window.location.hostname === '';
            if (isSetupMode) {
                document.body.classList.add('setup-mode');
                console.log('[Mode] Setup mode (192.168.10.1)');
            }

            // Кнопка "Вернуться к AP"
            const returnApBtn = document.getElementById('returnApBtn');
            const returnApModal = document.getElementById('returnApModal');
            if (returnApBtn && !isSetupMode) {
                returnApBtn.style.display = 'block';
                returnApBtn.addEventListener('click', function() {
                    returnApModal.classList.add('active');
                });
            }
            document.getElementById('cancelReturnApBtn').addEventListener('click', function() {
                returnApModal.classList.remove('active');
            });
            document.getElementById('confirmReturnApBtn').addEventListener('click', function() {
                returnApModal.classList.remove('active');
                window.location.href = 'http://192.168.10.1/settings';
            });
            returnApModal.addEventListener('click', function(e) {
                if (e.target === e.currentTarget) returnApModal.classList.remove('active');
            });

            // Показать/скрыть пароль в модалке
            const passwordInput = document.getElementById('modalPasswordInput');
            const eyeBtn = document.getElementById('eyeBtn');
            const eyeImg = document.getElementById('eyeImg');
            let passwordVisible = false;
            function updateEyeSource() {
                eyeImg.src = passwordVisible ? '/image/Closed.png' : '/image/Open.png';
                eyeImg.alt = passwordVisible ? 'Скрыть пароль' : 'Показать пароль';
            }
            if (eyeBtn) eyeBtn.addEventListener('click', function() {
                passwordVisible = !passwordVisible;
                passwordInput.type = passwordVisible ? 'text' : 'password';
                updateEyeSource();
            });

            // Закрытие модалки пароля по клику на фон
            const passwordModal = document.getElementById('passwordModal');
            if (passwordModal) {
                passwordModal.addEventListener('click', function(e) {
                    if (e.target === e.currentTarget) passwordModal.classList.remove('active');
                });
            }

            // Тип безопасности — показать/скрыть поле пароля
            const secSelect = document.getElementById('wifiSec');
            const passGroup = document.getElementById('passGroup');
            const passInput = document.getElementById('wifiPass');
            if (secSelect) {
                secSelect.addEventListener('change', function() {
                    if (this.value === 'OPEN') {
                        passGroup.style.display = 'none';
                        passInput.value = '';
                    } else {
                        passGroup.style.display = '';
                    }
                });
            }

            // ===== СКАНИРОВАНИЕ СЕТЕЙ =====
            const scanBtn = document.getElementById('scanBtn');
            const networkList = document.getElementById('networkList');
            const ssidInput = document.getElementById('wifiSsid');

            if (scanBtn) scanBtn.addEventListener('click', async function() {
                scanBtn.disabled = true;
                scanBtn.textContent = 'Сканирование...';
                networkList.style.display = 'block';
                networkList.innerHTML = '<div class="network-item empty"><span style="color:var(--subtitle-color); padding:10px;">Поиск сетей...</span></div>';

                try {
                    const res = await fetch('/api/scan', { cache: 'no-store' });
                    if (!res.ok) throw new Error('HTTP ' + res.status);
                    const nets = await res.json();

                    if (!nets.length) {
                        networkList.innerHTML = '<div class="network-item empty"><span style="color:var(--subtitle-color); padding:10px;">Сети не найдены</span></div>';
                    } else {
                        networkList.innerHTML = nets.map(function(n) {
                            const sec = n.encryption !== 'OPEN' ? 'Защищено' : 'Открытая';
                            const esc = n.ssid.replace(/'/g, "\\'").replace(/"/g, '&quot;');
                            return '<div class="network-item">' +
                                '<div class="network-icon">' +
                                '<img src="/image/light_small.png" alt="WiFi" class="wifi-img-small wifi-light">' +
                                '<img src="/image/dark_small.png" alt="WiFi" class="wifi-img-small wifi-dark">' +
                                '</div>' +
                                '<span class="network-name">' + esc + '</span>' +
                                '<span class="network-security">' + sec + '</span>' +
                                '<button class="btn-connect" onclick="selectNetwork(\'' + esc + '\',\'' + n.encryption + '\')">Выбрать</button>' +
                                '</div>';
                        }).join('');
                    }
                } catch (e) {
                    networkList.innerHTML = '<div class="network-item empty"><span style="color:#e55a5a; padding:10px;">Ошибка: ' + e.message + '</span></div>';
                } finally {
                    scanBtn.disabled = false;
                    scanBtn.textContent = 'Обновить список';
                }
            });

            window.selectNetwork = function(ssid, encryption) {
                ssidInput.value = ssid;
                secSelect.value = encryption || 'WPA2';
                if (encryption === 'OPEN') {
                    passGroup.style.display = 'none';
                    passInput.value = '';
                } else {
                    passGroup.style.display = '';
                    passInput.focus();
                }
                document.querySelector('.manual-section').scrollIntoView({ behavior: 'smooth', block: 'start' });
            };

            // ===== ПОДКЛЮЧЕНИЕ К СЕТИ =====
            const addNetworkBtn = document.getElementById('addNetworkBtn');
            const statusDiv = document.getElementById('connectStatus');

            if (addNetworkBtn) addNetworkBtn.addEventListener('click', async function() {
                const ssid = ssidInput.value.trim();
                const pass = passInput.value;
                const sec = secSelect.value;

                if (!ssid) { alert('Введите название сети'); return; }
                if (sec !== 'OPEN' && !pass) { alert('Введите пароль для защищённой сети'); return; }

                addNetworkBtn.disabled = true;
                addNetworkBtn.textContent = 'Подключение...';
                statusDiv.textContent = '';
                statusDiv.className = '';

                const formData = new URLSearchParams();
                formData.append('ssid', ssid);
                formData.append('pass', pass);
                formData.append('sec', sec);

                try {
                    const res = await fetch('/api/configure', {
                        method: 'POST',
                        body: formData,
                        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }
                    });
                    const data = await res.json();

                    if (data.status === 'success') {
                        statusDiv.textContent = 'Подключено! Сейчас подключитесь к выбранной сети на своём устройстве!';
                        statusDiv.className = 'success';
                        setTimeout(function() {
                            window.location.href = 'http://' + data.ip + '/settings';
                        }, 2000);
                    } else {
                        statusDiv.textContent = data.message || 'Ошибка подключения';
                        statusDiv.className = 'error';
                        addNetworkBtn.disabled = false;
                        addNetworkBtn.textContent = 'Добавить';
                    }
                } catch (e) {
                    statusDiv.textContent = 'Ошибка соединения: ' + e.message;
                    statusDiv.className = 'error';
                    addNetworkBtn.disabled = false;
                    addNetworkBtn.textContent = 'Добавить';
                }
            });
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
                <img src="/image/dpol.png" alt="" class="user-avatar avatar-dark">
                <img src="/image/lpol.png" alt="" class="user-avatar avatar-light">
                <span class="user-text">Загрузка...</span>
            </a>

            <div class="logo-container">
                <img src="/image/light.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/dark.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>

            <label class="theme-switcher" for="themeToggle" aria-label="Сменить тему">
                <img src="/image/topic.png" alt="" class="theme-icon-img">
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
        <div class="cards-area" id="plants-container">
            <div class="plant-card add-card glowing" id="addPlantCard">
                <div class="add-link">
                    <img src="/image/dark_icon.png" alt="Добавить" class="plus-icon icon-dark">
                    <img src="/image/light-icon.png" alt="Добавить" class="plus-icon icon-light">
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
                <input type="text" id="plant-name-input" class="modal-input" placeholder="Введите название">
                
                <div class="modal-photo-area">
                    <a href="#" class="modal-link" onclick="handlePhotoSelect(event)">Выбрать фото</a>
                    <input type="file" id="file-input" style="display: none;" accept="image/*">
                </div>

                <button id="add-btn-step-1" class="modal-btn" disabled>Далее</button>
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
                    <input type="number" id="param-humidity" class="modal-input param-input" placeholder="Начальный параметр влажности воздуха (%)" min="0" max="100" step="0.1">
                    <input type="number" id="param-soil" class="modal-input param-input" placeholder="Начальный параметр влажность почвы (%)" min="0" max="100" step="0.1">
                    <input type="number" id="param-temp" class="modal-input param-input" placeholder="Начальный параметр температуры (°C)" min="-40" max="80" step="0.1">
                    <input type="number" id="param-pressure" class="modal-input param-input" placeholder="Начальный параметр атм. давление (мм рт.ст.)" min="500" max="800" step="0.1">
                </div>

                <button class="modal-btn active" onclick="finalAddPlant()">Добавить</button>
            </div>
        </div>
    </div>

    <!-- Модальное окно редактирования -->
    <div id="editModal" class="modal-overlay" style="display: none;">
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
                <input type="number" id="editParamHumid" class="modal-input" placeholder="Параметр влажности воздуха (%)">
                <input type="number" id="editParamSoil" class="modal-input" placeholder="Параметр влажности почвы (%)">
                <input type="number" id="editParamTemp" class="modal-input" placeholder="Параметр температуры (°C)">
                <input type="number" id="editParamWater" class="modal-input" placeholder="Параметр атм. давление (мм рт.ст.)">
            </div>
            
            <button class="modal-btn active" onclick="updatePlantOnServer()">Сохранить изменения</button>
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
    function getAuthToken() {
        return localStorage.getItem('auth_token');
    }

    function resolvePhotoPath(path) {
        if (!path || path === '') return '/image/default-plant.png';
        if (path.startsWith('http')) return path;
        let cleanPath = path.replace(/^\/+/, '');
        return '/api/plant/photo/file?file=' + encodeURIComponent(cleanPath);
    }

    // === ЗАГРУЗКА ПРОФИЛЯ ===
    async function loadProfileFromServer() {
        const token = getAuthToken();
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

    function resolveAvatarPath(path) {
        if (!path || path === '') return '/image/dpol.png';
        if (path.startsWith('http')) return path;
        let cleanPath = path.replace(/^\/+/, '');
        return '/api/avatar/file?file=' + encodeURIComponent(cleanPath);
    }

    async function loadUserToHeader() {
        const profile = await loadProfileFromServer();
        if (!profile) return;
        
        const userText = document.querySelector('.user-pill .user-text');
        if (userText) userText.textContent = profile.username;
        
        const avatarSrc = resolveAvatarPath(profile.avatar);
        const avDark = document.querySelector('.user-pill .avatar-dark');
        const avLight = document.querySelector('.user-pill .avatar-light');
        
        if (avDark) avDark.src = avatarSrc;
        if (avLight) avLight.src = avatarSrc;
    }

    // === ЗАГРУЗКА РАСТЕНИЙ С СЕРВЕРА ===
    let currentPlants = [];

    async function loadPlantsFromServer() {
        const token = getAuthToken();
        if (!token) return;
        
        try {
            const res = await fetch('/api/plants', {
                headers: {'Authorization': 'Bearer ' + token}
            });
            
            if (res.ok) {
                currentPlants = await res.json();
                renderPlants(currentPlants);
            } else if (res.status === 401) {
                localStorage.removeItem('auth_token');
                window.location.href = '/login';
            }
        } catch (e) {
            console.error('Load plants error:', e);
        }
    }

    function renderPlants(plants) {
        const container = document.getElementById('plants-container');
        if (!container) return;
        
        // Удаляем все динамические карточки, оставляем только add-card
        const addCard = document.getElementById('addPlantCard');
        container.innerHTML = '';
        
        plants.forEach(plant => {
            const card = createPlantCard(plant);
            container.appendChild(card);
        });
        
        // Добавляем карточку добавления в конец
        if (addCard) {
            container.appendChild(addCard);
        }
    }

    function createPlantCard(plant) {
        const card = document.createElement('div');
        card.className = 'plant-card glowing';
        card.setAttribute('data-plant-id', plant.id);
        card.setAttribute('data-plant-name', plant.name);
        
        const photoSrc = resolvePhotoPath(plant.photo);
        
        // Формируем HTML карточки
        card.innerHTML = `
            <img src="${photoSrc}" alt="${escapeHtml(plant.name)}" class="plant-img" onerror="this.src='/image/default-plant.png'">
            <a href="/app" class="plant-label">${escapeHtml(plant.name)}</a>
            <button class="edit-plant-btn" onclick="openEditModal(${plant.id})" title="Редактировать">✎</button>
            <button class="delete-plant-btn" onclick="deletePlant(${plant.id}, '${escapeHtml(plant.name)}')" title="Удалить растение">×</button>
        `;
        
        return card;
    }

    function escapeHtml(str) {
        if (!str) return '';
        return str.replace(/[&<>]/g, function(m) {
            if (m === '&') return '&amp;';
            if (m === '<') return '&lt;';
            if (m === '>') return '&gt;';
            return m;
        });
    }

    // === ЗАГРУЗКА ФОТО НА СЕРВЕР ===
    let currentTempPhotoFile = null;
    let currentEditPhotoFile = null;

    async function uploadPlantPhoto(file) {
        const token = getAuthToken();
        if (!token) return null;
        
        const formData = new FormData();
        formData.append('file', file);
        
        try {
            const res = await fetch('/api/plant/photo', {
                method: 'POST',
                headers: {'Authorization': 'Bearer ' + token},
                body: formData
            });
            
            if (res.ok) {
                const data = await res.json();
                return data.path;
            }
        } catch (e) {
            console.error('Upload error:', e);
        }
        return null;
    }

    // === ДОБАВЛЕНИЕ РАСТЕНИЯ (СОХРАНЯЕМ ВАШУ ЛОГИКУ) ===
    let tempPlantData = {
        name: '',
        comment: '',
        photo: null,
        params: { temp: 0, humidity: 0, soil: 0, pressure: 0 }
    };

    const modal = document.getElementById('plantModal');
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

    function openModal() {
        modal.style.display = 'flex';
        showStep(1);
        resetForm();
    }

    function closeModal() {
        modal.style.display = 'none';
        resetForm();
    }

    function showStep(stepNumber) {
        step1.style.display = 'none';
        step2.style.display = 'none';
        step3.style.display = 'none';
        
        if (stepNumber === 1) step1.style.display = 'block';
        else if (stepNumber === 2) step2.style.display = 'block';
        else if (stepNumber === 3) step3.style.display = 'block';
    }

    function resetForm() {
        plantNameInput.value = '';
        plantNameDisplay.value = '';
        if (commentInput) commentInput.value = '';
        plantPreviewImg.src = '';
        plantPreviewImg.style.display = 'none';
        fileInput.value = '';
        addBtnStep1.disabled = true;
        addBtnStep1.classList.remove('active');
        
        paramInputs.forEach(input => {
            input.value = '';
        });
        
        currentTempPhotoFile = null;
        tempPlantData = {
            name: '',
            comment: '',
            photo: null,
            params: { temp: 0, humidity: 0, soil: 0, pressure: 0 }
        };
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

    fileInput.addEventListener('change', function(e) {
        const file = e.target.files[0];
        if (file) {
            if (!file.type.startsWith('image/')) {
                alert('Пожалуйста, выберите изображение');
                return;
            }
            if (file.size > 2 * 1024 * 1024) {
                alert('Размер файла не должен превышать 2MB');
                return;
            }
            
            currentTempPhotoFile = file;
            const reader = new FileReader();
            reader.onload = function(e) {
                plantPreviewImg.src = e.target.result;
                plantPreviewImg.style.display = 'block';
            };
            reader.readAsDataURL(file);
        }
    });

    // Переход ко второму шагу
    addBtnStep1.addEventListener('click', function() {
        if (!this.disabled) {
            tempPlantData.name = plantNameInput.value.trim();
            plantNameDisplay.value = tempPlantData.name;
            showStep(2);
        }
    });

    // Функция сброса фото — пересоздаём file input чтобы гарантированно сбросить
    function resetPhoto(e) {
        if (e) e.preventDefault();
        plantPreviewImg.src = '';
        plantPreviewImg.style.display = 'none';
        currentTempPhotoFile = null;
        // Пересоздаём file input (надёжнее чем .value = '')
        const oldInput = document.getElementById('file-input');
        const newInput = oldInput.cloneNode(false);
        newInput.addEventListener('change', function(ev) {
            const file = ev.target.files[0];
            if (file) {
                if (!file.type.startsWith('image/')) { alert('Пожалуйста, выберите изображение'); return; }
                if (file.size > 2 * 1024 * 1024) { alert('Размер файла не должен превышать 2MB'); return; }
                currentTempPhotoFile = file;
                const reader = new FileReader();
                reader.onload = function(ev2) {
                    plantPreviewImg.src = ev2.target.result;
                    plantPreviewImg.style.display = 'block';
                };
                reader.readAsDataURL(file);
            }
        });
        oldInput.parentNode.replaceChild(newInput, oldInput);
    }

    // Переход к параметрам
    function goToParameters() {
        tempPlantData.comment = commentInput ? commentInput.value.trim() : '';
        showStep(3);
    }

    // Финальное добавление растения в БД
    async function finalAddPlant() {
        const token = getAuthToken();
        if (!token) {
            window.location.href = '/login';
            return;
        }
        
        // Собираем параметры по именованным ID
        const humVal  = parseFloat(document.getElementById('param-humidity')?.value);
        const soilVal = parseFloat(document.getElementById('param-soil')?.value);
        const tempVal = parseFloat(document.getElementById('param-temp')?.value);
        const presVal = parseFloat(document.getElementById('param-pressure')?.value);

        // Валидация: хотя бы один параметр должен быть задан и находиться в допустимом диапазоне
        if (!isNaN(humVal) && (humVal < 0 || humVal > 100)) {
            alert('Влажность воздуха должна быть от 0 до 100%'); return;
        }
        if (!isNaN(soilVal) && (soilVal < 0 || soilVal > 100)) {
            alert('Влажность почвы должна быть от 0 до 100%'); return;
        }
        if (!isNaN(tempVal) && (tempVal < -40 || tempVal > 80)) {
            alert('Температура должна быть от -40 до 80°C'); return;
        }
        if (!isNaN(presVal) && (presVal < 500 || presVal > 800)) {
            alert('Давление должно быть от 500 до 800 мм рт.ст.'); return;
        }

        tempPlantData.params = {
            humidity: isNaN(humVal)  ? 0 : humVal,
            soil:     isNaN(soilVal) ? 0 : soilVal,
            temp:     isNaN(tempVal) ? 0 : tempVal,
            pressure: isNaN(presVal) ? 0 : presVal
        };
        
        // Загружаем фото если есть
        let photoPath = '';
        if (currentTempPhotoFile) {
            photoPath = await uploadPlantPhoto(currentTempPhotoFile) || '';
        }
        
        // Отправляем данные на сервер
        const plantData = {
            name: tempPlantData.name,
            comment: tempPlantData.comment,
            photo: photoPath,
            temp: tempPlantData.params.temp,
            humidity: tempPlantData.params.humidity,
            soil: tempPlantData.params.soil,
            pressure: tempPlantData.params.pressure
        };
        
        try {
            const res = await fetch('/api/plants', {
                method: 'POST',
                headers: {
                    'Authorization': 'Bearer ' + token,
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(plantData)
            });
            
            if (res.ok) {
                closeModal();
                await loadPlantsFromServer(); // Перезагружаем список
                alert('Растение успешно добавлено!');
            } else {
                const error = await res.json();
                alert('Ошибка: ' + (error.error || 'Не удалось добавить растение'));
            }
        } catch (e) {
            console.error('Add plant error:', e);
            alert('Ошибка соединения с сервером');
        }
    }

    // === РЕДАКТИРОВАНИЕ РАСТЕНИЯ ===
    let currentEditPlantId = null;

    async function openEditModal(plantId) {
        // Находим растение в текущем списке
        const plant = currentPlants.find(p => p.id === plantId);
        if (!plant) return;
        
        currentEditPlantId = plant.id;
        
        // Заполняем форму
        document.getElementById('editPlantName').value = plant.name || '';
        document.getElementById('editParamHumid').value = plant.humidity || '';
        document.getElementById('editParamSoil').value = plant.soil || '';
        document.getElementById('editParamTemp').value = plant.temp || '';
        document.getElementById('editParamWater').value = plant.pressure || '';
        
        const photoSrc = resolvePhotoPath(plant.photo);
        document.getElementById('editPlantPreview').src = photoSrc;
        
        // Сбрасываем файл
        currentEditPhotoFile = null;
        document.getElementById('editPlantPhoto').value = '';
        
        document.getElementById('editModal').style.display = 'flex';
    }

    function closeEditModal() {
        document.getElementById('editModal').style.display = 'none';
        currentEditPlantId = null;
        currentEditPhotoFile = null;
    }

    // Предпросмотр фото при редактировании
    document.getElementById('editPlantPhoto').addEventListener('change', function(e) {
        const file = e.target.files[0];
        if (file) {
            if (!file.type.startsWith('image/')) {
                alert('Пожалуйста, выберите изображение');
                return;
            }
            if (file.size > 2 * 1024 * 1024) {
                alert('Размер файла не должен превышать 2MB');
                return;
            }
            
            currentEditPhotoFile = file;
            const reader = new FileReader();
            reader.onload = function(ev) {
                document.getElementById('editPlantPreview').src = ev.target.result;
            };
            reader.readAsDataURL(file);
        }
    });

    async function updatePlantOnServer() {
        const token = getAuthToken();
        if (!token || !currentEditPlantId) return;
        
        const name = document.getElementById('editPlantName').value.trim();
        if (!name) {
            alert('Введите название растения');
            return;
        }
        
        // Загружаем новое фото если есть
        let photoPath = '';
        if (currentEditPhotoFile) {
            photoPath = await uploadPlantPhoto(currentEditPhotoFile) || '';
        }
        
        const plantData = {
            id: currentEditPlantId,
            name: name,
            comment: document.getElementById('plant-comment-input')?.value || '',
            photo: photoPath,
            temp: parseFloat(document.getElementById('editParamTemp').value) || 0,
            humidity: parseFloat(document.getElementById('editParamHumid').value) || 0,
            soil: parseFloat(document.getElementById('editParamSoil').value) || 0,
            pressure: parseFloat(document.getElementById('editParamWater').value) || 0
        };
        
        try {
            const res = await fetch('/api/plants', {
                method: 'PUT',
                headers: {
                    'Authorization': 'Bearer ' + token,
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(plantData)
            });
            
            if (res.ok) {
                closeEditModal();
                await loadPlantsFromServer();
                alert('Растение обновлено!');
            } else {
                const error = await res.json();
                alert('Ошибка: ' + (error.error || 'Не удалось обновить растение'));
            }
        } catch (e) {
            console.error('Update plant error:', e);
            alert('Ошибка соединения с сервером');
        }
    }

    // === УДАЛЕНИЕ РАСТЕНИЯ ===
    async function deletePlant(plantId, plantName) {
        if (!confirm(`Вы действительно хотите удалить растение "${plantName}"?`)) return;
        
        const token = getAuthToken();
        if (!token) return;
        
        try {
            const res = await fetch('/api/plants?id=' + plantId, {
                method: 'DELETE',
                headers: {'Authorization': 'Bearer ' + token}
            });
            
            if (res.ok) {
                await loadPlantsFromServer();
                alert('Растение удалено');
            } else {
                alert('Ошибка при удалении растения');
            }
        } catch (e) {
            console.error('Delete plant error:', e);
            alert('Ошибка соединения с сервером');
        }
    }

    // === ИНИЦИАЛИЗАЦИЯ ТЕМЫ ===
    (function initTheme() {
        const toggle = document.getElementById('themeToggle');
        const body = document.body;
        const saved = localStorage.getItem('theme') || 'dark';
        body.classList.remove('theme-dark', 'theme-light');
        body.classList.add('theme-' + saved);
        if (toggle) toggle.checked = saved === 'light';
        
        if (toggle) {
            toggle.addEventListener('change', function() {
                const next = toggle.checked ? 'light' : 'dark';
                body.classList.remove('theme-dark', 'theme-light');
                body.classList.add('theme-' + next);
                localStorage.setItem('theme', next);
            });
        }
    })();

    // === ЗАПУСК ===
    document.addEventListener('DOMContentLoaded', async () => {
        const token = getAuthToken();
        if (!token) {
            window.location.href = '/login';
            return;
        }
        
        await loadUserToHeader();
        await loadPlantsFromServer();
        
        // Добавляем обработчик для кнопки добавления
        const addCard = document.getElementById('addPlantCard');
        if (addCard) {
            addCard.addEventListener('click', openModal);
        }
        
        // Закрытие модальных окон по клику на фон
        modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
        });
        
        const editModal = document.getElementById('editModal');
        editModal.addEventListener('click', (e) => {
            if (e.target === editModal) closeEditModal();
        });
        
        // Кнопка закрытия модалки редактирования
        document.getElementById('closeEditModal').addEventListener('click', closeEditModal);
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
                <img src="/image/dpol.png" alt="" class="user-avatar avatar-dark">
                <img src="/image/lpol.png" alt="" class="user-avatar avatar-light">
                <span class="user-text">Загрузка...</span>
            </a>

            <div class="logo-container">
                <img src="/image/light.png" alt="Зелёная полка" class="main-logo logo-dark">
                <img src="/image/dark.png" alt="Зелёная полка" class="main-logo logo-light">
            </div>

            <label class="theme-switcher" for="themeToggle" aria-label="Сменить тему">
                <img src="/image/topic.png" alt="" class="theme-icon-img">
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
        <img src="/image/left.png" alt="Leaf Decor" class="deco-leaf leaf-left">
        <img src="/image/right.png" alt="Leaf Decor" class="deco-leaf leaf-right">
        
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
                <img src="/image/lightly.png" alt="Освещение" class="module-icon">
                <h3 class="module-title">Освещение растений</h3>
                <p class="module-description">
                    Автоматическая подсветка с регулировкой интенсивности и спектра для оптимального фотосинтеза
                </p>
            </div>

            <div class="module-card">
                <img src="/image/water.png" alt="Полив" class="module-icon">
                <h3 class="module-title">Полив растений</h3>
                <p class="module-description">
                    Умная система полива с контролем влажности почвы и автоматическим дозированием воды
                </p>
            </div>

            <div class="module-card" onclick="location.href='/app'">
                <img src="Micro.png" alt="Климат" class="module-icon">
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
        // === ЗАГРУЗКА ПРОФИЛЯ С СЕРВЕРА ===
        async function loadProfileFromServer() {
            const token = localStorage.getItem('auth_token');
            if (!token) {
                console.log('❌ Нет токена, но на лендинге не редиректим');
                return null;
            }
            
            try {
                const res = await fetch('/api/profile', {
                    headers: {'Authorization': 'Bearer ' + token}
                });
                
                if (res.status === 401) {
                    console.log('❌ Токен недействителен');
                    localStorage.removeItem('auth_token');
                    return null;
                }
                
                return await res.json();
            } catch (e) {
                console.error('Profile load error:', e);
                return null;
            }
        }

        function resolveAvatarPath(path) {
            if (!path || path === '') return '/image/dpol.png';
            if (path.startsWith('http')) return path;
            let cleanPath = path.replace(/^\/+/, '');
            return '/api/avatar/file?file=' + encodeURIComponent(cleanPath);
        }

        async function loadUserToHeader() {
            const profile = await loadProfileFromServer();
            if (!profile) return;
            
            const userText = document.querySelector('.user-pill .user-text');
            if (userText) userText.textContent = profile.username;
            
            const avatarSrc = resolveAvatarPath(profile.avatar);
            const avDark = document.querySelector('.user-pill .avatar-dark');
            const avLight = document.querySelector('.user-pill .avatar-light');
            
            if (avDark) avDark.src = avatarSrc;
            if (avLight) avLight.src = avatarSrc;
        }

        // Инициализация темы
        (function initTheme() {
            const toggle = document.getElementById('themeToggle');
            const body = document.body;
            const KEY = 'theme';
            const saved = localStorage.getItem(KEY) || 'dark';
            body.classList.toggle('theme-dark', saved === 'dark');
            body.classList.toggle('theme-light', saved === 'light');
            if (toggle) toggle.checked = saved === 'light';
            
            if (toggle) {
                toggle.addEventListener('change', function() {
                    const next = toggle.checked ? 'light' : 'dark';
                    body.classList.toggle('theme-dark', !toggle.checked);
                    body.classList.toggle('theme-light', toggle.checked);
                    localStorage.setItem(KEY, next);
                });
            }
        })();

        // Дополнительная инициализация темы (через кнопку)
        document.addEventListener('DOMContentLoaded', () => {
            const themeBtn = document.getElementById('themeSwitcher');
            const body = document.body;
            const savedTheme = localStorage.getItem('theme');
            
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
        });

        // Загружаем профиль после загрузки страницы
        document.addEventListener('DOMContentLoaded', async () => {
            await loadUserToHeader();
        });
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
  // Требуем авторизацию: раньше любой POST мог стереть весь журнал
  String auth = server.header("Authorization");
  if (!auth.startsWith("Bearer ") || !getUserIdByToken(auth.substring(7))) {
    server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
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
    if (login.length() < 3 || password.length() < 4 || email.indexOf('@') < 1) {
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
    // Если БД ещё не инициализирована (SD медленно стартует) — вернуть 503,
    // а не 401, чтобы фронтенд не делал редирект на /login
    if (!db) {
        server.sendHeader("Retry-After", "5");
        server.send(503, "application/json", "{\"error\":\"DB not ready\",\"retry\":true}");
        return;
    }
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
    if (server.hasArg("plain") == false) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
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
        // Обновляем часовой пояс в памяти, если пользователь его сменил
        if (timezone.length() > 0) {
            int newOffset = parseTimezoneOffset(timezone);
            if (newOffset != tzOffsetSec) {
                tzOffsetSec = newOffset;
                configTime(tzOffsetSec, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
                Serial.printf("[TZ] Часовой пояс обновлён пользователем: %d сек\n", tzOffsetSec);
            }
        }
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Update failed\"}");
    }
}

void handleAvatarUpload() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        Serial.println("=== НАЧАЛО ЗАГРУЗКИ АВАТАРА ===");

        // Сбрасываем результат предыдущей загрузки
        uploadOk = false;
        uploadResultPath = "";
        uploadErrorMsg = "";
        pendingNewAvatarPath = "";
        
        // Проверяем авторизацию (ответ НЕ отправляем здесь — это сделает финальный хендлер)
        String auth = server.header("Authorization");
        if (!auth.startsWith("Bearer ")) {
            uploadErrorMsg = "Unauthorized";
            return;
        }
        
        String token = auth.substring(7);
        pendingUserId = getUserIdByToken(token);
        
        if (!pendingUserId) {
            uploadErrorMsg = "Invalid token";
            return;
        }
        
        // Определяем формат файла из типа КОНКРЕТНОЙ части (upload.type),
        // а не из Content-Type запроса (там multipart/form-data).
        String fileExt = ".jpg"; // по умолчанию
        String ctype = upload.type;          // напр. "image/png"
        String fname = upload.filename;      // напр. "photo.png"

        if      (ctype.indexOf("image/png")  >= 0 || fname.endsWith(".png"))  fileExt = ".png";
        else if (ctype.indexOf("image/gif")  >= 0 || fname.endsWith(".gif"))  fileExt = ".gif";
        else if (ctype.indexOf("image/webp") >= 0 || fname.endsWith(".webp")) fileExt = ".webp";
        else if (ctype.indexOf("image/bmp")  >= 0 || fname.endsWith(".bmp"))  fileExt = ".bmp";
        else if (ctype.indexOf("image/jpeg") >= 0 || ctype.indexOf("image/jpg") >= 0 ||
                 fname.endsWith(".jpg") || fname.endsWith(".jpeg"))           fileExt = ".jpg";
        
        // Получаем старый аватар
        pendingOldAvatarPath = getCurrentAvatarPath(pendingUserId);
        Serial.printf("User ID: %d\n", pendingUserId);
        Serial.printf("Старый аватар: '%s'\n", pendingOldAvatarPath.c_str());
        Serial.printf("Формат файла: %s\n", fileExt.c_str());
        
        // Генерируем уникальное имя с правильным расширением
        String timestamp = String(millis());
        String randomNum = String((uint32_t)(esp_random() % 9000 + 1000));
        String fileName = "av_" + timestamp + "_" + randomNum + fileExt;
        pendingNewAvatarPath = "/avatar/" + fileName;
        
        Serial.printf("Новый файл: %s\n", pendingNewAvatarPath.c_str());
        
        // Убеждаемся, что папка avatar существует
        if (!SD.exists("/avatar")) {
            Serial.println("Создаю папку /avatar");
            SD.mkdir("/avatar");
        }
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (pendingNewAvatarPath.length() == 0) return;
        
        File f = SD.open(pendingNewAvatarPath, FILE_APPEND);
        if (f) {
            f.write(upload.buf, upload.currentSize);
            f.close();
            Serial.printf("Записано %d байт\n", upload.currentSize);
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.println("=== ЗАГРУЗКА ЗАВЕРШЕНА ===");

        // Если авторизация не прошла — файл мог не создаться, выходим
        if (pendingNewAvatarPath.length() == 0) {
            if (uploadErrorMsg.length() == 0) uploadErrorMsg = "Upload failed";
            return;
        }
        
        if (SD.exists(pendingNewAvatarPath)) {
            Serial.printf("Файл сохранен: %s\n", pendingNewAvatarPath.c_str());
            
            // Удаляем старый аватар
            if (pendingOldAvatarPath.length() > 0 && 
                !pendingOldAvatarPath.startsWith("http") &&
                pendingOldAvatarPath != "avatar/default.jpg") {
                
                String oldFullPath = "/" + pendingOldAvatarPath;
                if (!oldFullPath.startsWith("/")) oldFullPath = "/" + oldFullPath;
                
                if (SD.exists(oldFullPath)) {
                    if (SD.remove(oldFullPath)) {
                        Serial.println("Старый аватар удален");
                    }
                }
            }
            
            // Сохраняем путь в БД
            String relPath = pendingNewAvatarPath.substring(1);
            
            if (updateUserAvatarInDB(server.header("Authorization").substring(7), relPath)) {
                uploadOk = true;
                uploadResultPath = relPath;
            } else {
                uploadErrorMsg = "Database update failed";
            }
        } else {
            uploadErrorMsg = "File not saved";
        }
        
        pendingUserId = 0;
        pendingOldAvatarPath = "";
        pendingNewAvatarPath = "";
    }
}

// Финальный ответ для /api/avatar (вызывается ОДИН раз после завершения загрузки)
void handleAvatarUploadDone() {
    if (uploadOk) {
        DynamicJsonDocument doc(256);
        doc["success"] = true;
        doc["avatar"] = uploadResultPath;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    } else {
        String msg = uploadErrorMsg.length() ? uploadErrorMsg : "Upload failed";
        int code = (msg == "Unauthorized" || msg == "Invalid token") ? 401 : 500;
        String body = "{\"success\":false,\"error\":\"" + msg + "\"}";
        server.send(code, "application/json", body);
    }
}

// === Получение файла аватара ===
void handleGetAvatar() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing 'file' parameter");
        return;
    }
    
    String filePath = server.arg("file");
    Serial.printf("📸 Запрос аватара: %s\n", filePath.c_str());
    
    // Защита от path traversal
    if (filePath.indexOf("..") >= 0 || filePath.indexOf("//") >= 0) {
        server.send(403, "text/plain", "Forbidden");
        return;
    }
    
    // Нормализация пути
    String fullPath = filePath;
    if (!fullPath.startsWith("/")) {
        fullPath = "/" + fullPath;
    }
    
    if (!SD.exists(fullPath)) {
        Serial.printf("❌ Файл не найден: %s\n", fullPath.c_str());
        
        // Пытаемся отдать дефолтный аватар
        if (SD.exists("/image/dpol.png")) {
            File defaultFile = SD.open("/image/dpol.png", FILE_READ);
            server.streamFile(defaultFile, "image/jpeg");
            defaultFile.close();
            return;
        }
        
        server.send(404, "text/plain", "Avatar not found");
        return;
    }
    
    // 🔥 РАСШИРЕННОЕ ОПРЕДЕЛЕНИЕ MIME ТИПА
    String contentType = "application/octet-stream";
    if (filePath.endsWith(".jpg") || filePath.endsWith(".jpeg")) {
        contentType = "image/jpeg";
    } else if (filePath.endsWith(".png")) {
        contentType = "image/png";
    } else if (filePath.endsWith(".gif")) {
        contentType = "image/gif";
    } else if (filePath.endsWith(".webp")) {
        contentType = "image/webp";
    } else if (filePath.endsWith(".bmp")) {
        contentType = "image/bmp";
    } else if (filePath.endsWith(".svg")) {
        contentType = "image/svg+xml";
    }
    
    Serial.printf("✅ Отправляю: %s (%s)\n", fullPath.c_str(), contentType.c_str());
    
    server.sendHeader("Cache-Control", "public, max-age=3600");
    server.sendHeader("Content-Type", contentType);
    
    File file = SD.open(fullPath, FILE_READ);
    if (file) {
        server.streamFile(file, contentType);
        file.close();
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

// ==================== API ДЛЯ РАСТЕНИЙ ====================

// Получить все растения пользователя
void handleGetPlants() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    int userId = getUserIdByToken(token);
    
    if (!userId) {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
        return;
    }
    
    String plants = getUserPlants(userId);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(200, "application/json", plants);
}

// Добавить растение
void handleAddPlant() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    int userId = getUserIdByToken(token);
    
    if (!userId) {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
        return;
    }
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing data\"}");
        return;
    }
    
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    String name = doc["name"] | "";
    String comment = doc["comment"] | "";
    String photo = doc["photo"] | "";
    float temp = doc["temp"] | 0;
    float humidity = doc["humidity"] | 0;
    float soil = doc["soil"] | 0;
    float pressure = doc["pressure"] | 0;
    
    if (name.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Plant name required\"}");
        return;
    }
    
    bool success = addPlant(userId, name, comment, photo, temp, humidity, soil, pressure);
    
    if (success) {
        server.sendHeader("Connection", "close");
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.sendHeader("Connection", "close");
        server.send(500, "application/json", "{\"error\":\"Failed to add plant\"}");
    }
}

// Обновить растение
void handleUpdatePlant() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    int userId = getUserIdByToken(token);
    
    if (!userId) {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
        return;
    }
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing data\"}");
        return;
    }
    
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    int plantId = doc["id"] | 0;
    String name = doc["name"] | "";
    String comment = doc["comment"] | "";
    String photo = doc["photo"] | "";
    float temp = doc["temp"] | 0;
    float humidity = doc["humidity"] | 0;
    float soil = doc["soil"] | 0;
    float pressure = doc["pressure"] | 0;
    
    if (plantId == 0 || name.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Invalid data\"}");
        return;
    }
    
    bool success = updatePlant(plantId, userId, name, comment, photo, temp, humidity, soil, pressure);
    
    if (success) {
        server.sendHeader("Connection", "close");
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.sendHeader("Connection", "close");
        server.send(500, "application/json", "{\"error\":\"Failed to update plant\"}");
    }
}

// Удалить растение
void handleDeletePlant() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    int userId = getUserIdByToken(token);
    
    if (!userId) {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
        return;
    }
    
    if (!server.hasArg("id")) {
        server.send(400, "application/json", "{\"error\":\"Plant ID required\"}");
        return;
    }
    
    int plantId = server.arg("id").toInt();
    
    if (plantId == 0) {
        server.send(400, "application/json", "{\"error\":\"Invalid plant ID\"}");
        return;
    }
    
    bool success = deletePlant(plantId, userId);
    
    if (success) {
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to delete plant\"}");
    }
}

// Загрузка фото растения
void handlePlantPhotoUpload() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        Serial.println("=== НАЧАЛО ЗАГРУЗКИ ФОТО РАСТЕНИЯ ===");

        uploadOk = false;
        uploadResultPath = "";
        uploadErrorMsg = "";
        pendingNewAvatarPath = "";
        
        String auth = server.header("Authorization");
        if (!auth.startsWith("Bearer ")) {
            uploadErrorMsg = "Unauthorized";
            return;
        }
        
        String token = auth.substring(7);
        int userId = getUserIdByToken(token);
        
        if (!userId) {
            uploadErrorMsg = "Invalid token";
            return;
        }
        
        // Определяем формат файла из типа конкретной части / имени файла
        String fileExt = ".jpg";
        String ctype = upload.type;
        String fname = upload.filename;

        if      (ctype.indexOf("image/png")  >= 0 || fname.endsWith(".png"))  fileExt = ".png";
        else if (ctype.indexOf("image/gif")  >= 0 || fname.endsWith(".gif"))  fileExt = ".gif";
        else if (ctype.indexOf("image/webp") >= 0 || fname.endsWith(".webp")) fileExt = ".webp";
        else if (ctype.indexOf("image/jpeg") >= 0 || ctype.indexOf("image/jpg") >= 0 ||
                 fname.endsWith(".jpg") || fname.endsWith(".jpeg"))           fileExt = ".jpg";
        
        // Генерируем уникальное имя
        String timestamp = String(millis());
        String randomNum = String((uint32_t)(esp_random() % 9000 + 1000));
        String fileName = "plant_" + String(userId) + "_" + timestamp + "_" + randomNum + fileExt;
        pendingNewAvatarPath = "/plant/" + fileName; // Переиспользуем переменную
        
        // Создаем папку если нужно
        if (!SD.exists("/plant")) {
            SD.mkdir("/plant");
        }
        
        pendingUserId = userId;
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (pendingNewAvatarPath.length() == 0) return;
        
        File f = SD.open(pendingNewAvatarPath, FILE_APPEND);
        if (f) {
            f.write(upload.buf, upload.currentSize);
            f.close();
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        if (pendingNewAvatarPath.length() == 0) {
            if (uploadErrorMsg.length() == 0) uploadErrorMsg = "Upload failed";
            return;
        }
        if (SD.exists(pendingNewAvatarPath)) {
            uploadOk = true;
            uploadResultPath = pendingNewAvatarPath.substring(1);
        } else {
            uploadErrorMsg = "File not saved";
        }
        
        pendingUserId = 0;
        pendingNewAvatarPath = "";
    }
}

// Финальный ответ для /api/plant/photo
void handlePlantPhotoUploadDone() {
    if (uploadOk) {
        DynamicJsonDocument doc(256);
        doc["success"] = true;
        doc["path"] = uploadResultPath;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    } else {
        String msg = uploadErrorMsg.length() ? uploadErrorMsg : "Upload failed";
        int code = (msg == "Unauthorized" || msg == "Invalid token") ? 401 : 500;
        String body = "{\"success\":false,\"error\":\"" + msg + "\"}";
        server.send(code, "application/json", body);
    }
}

// Получить фото растения
void handleGetPlantPhoto() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing 'file' parameter");
        return;
    }
    
    String filePath = server.arg("file");
    
    if (filePath.indexOf("..") >= 0 || filePath.indexOf("//") >= 0) {
        server.send(403, "text/plain", "Forbidden");
        return;
    }
    
    String fullPath = filePath;
    if (!fullPath.startsWith("/")) {
        fullPath = "/" + fullPath;
    }
    
    if (!SD.exists(fullPath)) {
        server.send(404, "text/plain", "Photo not found");
        return;
    }
    
    String contentType = "image/jpeg";
    if (filePath.endsWith(".png")) contentType = "image/png";
    else if (filePath.endsWith(".gif")) contentType = "image/gif";
    else if (filePath.endsWith(".webp")) contentType = "image/webp";
    
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.sendHeader("Content-Type", contentType);
    
    File file = SD.open(fullPath, FILE_READ);
    if (file) {
        server.streamFile(file, contentType);
        file.close();
    } else {
        server.send(500, "text/plain", "Failed to open file");
    }
}

// Универсальный обработчик статических файлов
void handleStaticImage() {
    String uri = server.uri();
    
    // Убираем параметры запроса
    int qMark = uri.indexOf('?');
    if (qMark > 0) uri = uri.substring(0, qMark);
    
    Serial.printf("📁 Serving static: %s\n", uri.c_str());
    
    // Нормализация пути для SD карты
    String path = uri;
    
    // Если путь начинается с /sd/ - убираем этот префикс
    if (path.startsWith("/sd/")) {
        path = path.substring(3);
    }
    
    // Убеждаемся, что путь начинается с /
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    
    // Защита от взлома (path traversal)
    if (path.indexOf("..") >= 0 || path.indexOf("//") >= 0) {
        server.send(403, "text/plain", "Forbidden");
        return;
    }
    
    // Проверяем существование файла
    if (!SD.exists(path)) {
        Serial.printf("❌ File not found: %s\n", path.c_str());
        server.send(404, "text/plain", "File not found");
        return;
    }
    
    // Определяем MIME тип по расширению
    String contentType = "application/octet-stream";
    if (path.endsWith(".png"))      contentType = "image/png";
    else if (path.endsWith(".gif")) contentType = "image/gif";
    else if (path.endsWith(".jpg") || path.endsWith(".jpeg")) 
        contentType = "image/jpeg";
    else if (path.endsWith(".svg")) contentType = "image/svg+xml";
    else if (path.endsWith(".css")) contentType = "text/css";
    else if (path.endsWith(".js"))  contentType = "application/javascript";
    else if (path.endsWith(".html"))contentType = "text/html";
    else if (path.endsWith(".json"))contentType = "application/json";
    
    Serial.printf("✅ Sending: %s [%s]\n", path.c_str(), contentType.c_str());
    
    // Добавляем кэширование для изображений (1 день)
    if (path.endsWith(".png") || path.endsWith(".jpg") || path.endsWith(".jpeg")) {
        server.sendHeader("Cache-Control", "public, max-age=86400");
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


void handleNotFound() {
    String uri = server.uri();
    
    // Убираем параметры запроса (если есть)
    int qMark = uri.indexOf('?');
    if (qMark > 0) uri = uri.substring(0, qMark);
    
    // Логируем запрос
    Serial.printf("🔍 404 handler: %s\n", uri.c_str());
    
    // === ПРОВЕРЯЕМ: может это статический файл? ===
    bool isStaticFile = false;
    
    // 1. Проверяем по пути
    if (uri.startsWith("/image/") || uri.startsWith("/sd/")) {
        isStaticFile = true;
    }
    
    // 2. Проверяем по расширению
    if (uri.endsWith(".png") || uri.endsWith(".jpg") || 
        uri.endsWith(".jpeg") || uri.endsWith(".gif") ||
        uri.endsWith(".svg") || uri.endsWith(".css") ||
        uri.endsWith(".js") || uri.endsWith(".ico")) {
        isStaticFile = true;
    }
    
    // === ЕСЛИ ЭТО СТАТИЧЕСКИЙ ФАЙЛ - ОТДАЁМ ЕГО ===
    if (isStaticFile) {
        handleStaticImage();  // ваша функция для отдачи файлов
        return;
    }
    
    // === ИНАЧЕ - 404 ОШИБКА ===
    Serial.printf("❌ 404 Not Found: %s\n", uri.c_str());
    
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>404</title>";
    html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#0F182B;color:#21C85F;}";
    html += "h1{font-size:48px;}p{font-size:18px;}a{color:#21C85F;}</style>";
    html += "</head><body>";
    html += "<h1>404</h1>";
    html += "<p>Страница не найдена</p>";
    html += "<p>Запрошенный URL: " + uri + "</p>";
    html += "<p><a href='/'>Вернуться на главную</a></p>";
    html += "</body></html>";
    
    server.send(404, "text/html", html);
}

// ==================== СМЕНА ПАРОЛЯ ====================
void handleChangePassword() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    int userId = getUserIdByToken(token);
    if (!userId) {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
        return;
    }

    String body = server.arg("plain");
    if (body.length() == 0) body = server.arg(0);  // fallback

    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String oldPassword = doc["oldPassword"] | "";
    String newPassword = doc["newPassword"] | "";

    if (oldPassword.length() < 4 || newPassword.length() < 6) {
        server.send(400, "application/json", "{\"error\":\"Invalid password length\"}");
        return;
    }

    // Проверка старого пароля (с учётом соли)
    String currentSalt = "";
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT password, salt FROM Users WHERE id = ?;";
    bool verified = false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, userId);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* storedHashC = (const char*)sqlite3_column_text(stmt, 0);
            const char* saltC = (const char*)sqlite3_column_text(stmt, 1);
            String storedHash = storedHashC ? String(storedHashC) : "";
            currentSalt = saltC ? String(saltC) : "";
            verified = (storedHash == hashPassword(currentSalt + oldPassword));
        }
        sqlite3_finalize(stmt);
    }
    if (!verified) {
        server.send(401, "application/json", "{\"error\":\"Old password is incorrect\"}");
        return;
    }

    // Ротация соли при смене пароля
    String newSalt = generateSalt();

    // Обновление пароля через prepared statement (безопасно)
    const char* updateSql = "UPDATE Users SET password = ?, salt = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(db, updateSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hashPassword(newSalt + newPassword).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, newSalt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, userId);
        
        bool success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);

        if (success) {
            // Удаляем все сессии пользователя — заставляем заново залогиниться
            const char* deleteSessions = "DELETE FROM Sessions WHERE user_id = ?;";
            sqlite3_stmt* delStmt = nullptr;
            if (sqlite3_prepare_v2(db, deleteSessions, -1, &delStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(delStmt, 1, userId);
                sqlite3_step(delStmt);
                sqlite3_finalize(delStmt);
            }

            server.send(200, "application/json", "{\"success\":true,\"message\":\"Password changed successfully. Please login again.\"}");
        } else {
            server.send(500, "application/json", "{\"error\":\"Failed to update password\"}");
        }
    } else {
        server.send(500, "application/json", "{\"error\":\"Database error\"}");
    }
}

// ==================== УДАЛЕНИЕ АККАУНТА ====================
void handleDeleteAccount() {
    String auth = server.header("Authorization");
    if (!auth.startsWith("Bearer ")) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String token = auth.substring(7);
    int userId = getUserIdByToken(token);
    if (!userId) {
        server.send(401, "application/json", "{\"error\":\"Invalid token\"}");
        return;
    }

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Confirmation required\"}");
        return;
    }

    DynamicJsonDocument doc(128);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err || doc["confirm"] != "DELETE") {
        server.send(400, "application/json", "{\"error\":\"Invalid confirmation\"}");
        return;
    }

    // Удаляем пользователя (каскадно удалятся растения и сессии)
    char sql[64];
    sprintf(sql, "DELETE FROM Users WHERE id = %d;", userId);
    rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);

    if (rc != SQLITE_OK) {
        Serial.printf("Delete account error: %s\n", zErrMsg ? zErrMsg : "unknown");
        if (zErrMsg) sqlite3_free(zErrMsg);
        server.send(500, "application/json", "{\"error\":\"Failed to delete account\"}");
        return;
    }

    Serial.printf("✅ User %d deleted successfully\n", userId);
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Account deleted\"}");
}


// === DEBUG: Просмотр БД и SD без логина (по умолчанию ВЫКЛЮЧЕН) ===
void handleDebug() {
#if !ENABLE_DEBUG_ENDPOINT
    server.send(404, "text/plain", "Not found");
    return;
#else
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>DEBUG DB & SD</title>";
    html += "<style>body{font-family:monospace;background:#111;color:#0f0;padding:20px;}</style></head><body>";
    html += "<h1>🔍 Debug Info</h1><hr>";

    // Пользователи
    html += "<h2>📋 Таблица Users</h2><pre>";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, username, email, gender, timezone, avatar, created_at FROM Users;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            html += "ID: " + String(sqlite3_column_int(stmt, 0)) +
                    " | User: " + String((const char*)sqlite3_column_text(stmt, 1)) +
                    " | Email: " + String((const char*)sqlite3_column_text(stmt, 2)) +
                    " | Gender: " + String((const char*)sqlite3_column_text(stmt, 3)) +
                    " | TZ: " + String((const char*)sqlite3_column_text(stmt, 4)) +
                    " | Avatar: " + String((const char*)sqlite3_column_text(stmt, 5)) + "<br>";
        }
        sqlite3_finalize(stmt);
    } else {
        html += "Ошибка запроса Users: " + String(sqlite3_errmsg(db));
    }
    html += "</pre>";

// === ТАБЛИЦА РАСТЕНИЙ ===
    html += "<h2>🌱 Растения (Plants)</h2>";
    if (sqlite3_prepare_v2(db, "SELECT id, user_id, name, comment, photo_path, temp_param, humidity_param, soil_param, pressure_param, created_at FROM Plants ORDER BY id;", -1, &stmt, nullptr) == SQLITE_OK) {
        html += "<table><tr><th>ID</th><th>UserID</th><th>Название</th><th>Комментарий</th><th>Фото</th><th>Temp</th><th>Hum</th><th>Soil</th><th>Press</th><th>Создано</th></tr>";
        bool hasPlants = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            hasPlants = true;
            html += "<tr>";
            html += "<td>" + String(sqlite3_column_int(stmt, 0)) + "</td>";
            html += "<td>" + String(sqlite3_column_int(stmt, 1)) + "</td>";
            html += "<td>" + String((const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "") + "</td>";
            html += "<td>" + String((const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "") + "</td>";
            html += "<td>" + String((const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "") + "</td>";
            html += "<td>" + String(sqlite3_column_double(stmt, 5)) + "</td>";
            html += "<td>" + String(sqlite3_column_double(stmt, 6)) + "</td>";
            html += "<td>" + String(sqlite3_column_double(stmt, 7)) + "</td>";
            html += "<td>" + String(sqlite3_column_double(stmt, 8)) + "</td>";
            html += "<td>" + String(sqlite3_column_int(stmt, 9)) + "</td>";
            html += "</tr>";
        }
        sqlite3_finalize(stmt);
        if (!hasPlants) html += "<tr><td colspan='10' class='empty'>— Растений пока нет —</td></tr>";
        html += "</table>";
    }
/*    // Сессии
    html += "<h2> Сессии</h2><pre>";
    if (sqlite3_prepare_v2(db, "SELECT token, user_id, created_at FROM Sessions;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            html += "Token: " + String((const char*)sqlite3_column_text(stmt, 0)).substring(0,12) + "..." +
                    " | User: " + String(sqlite3_column_int(stmt, 1)) + "<br>";
        }
        sqlite3_finalize(stmt);
    }
    html += "</pre>";
*/
    // === СЕССИИ ===
    html += "<h2>🔑 Сессии (Sessions)</h2>";
    if (sqlite3_prepare_v2(db, "SELECT id, token, user_id, created_at FROM Sessions ORDER BY id;", -1, &stmt, nullptr) == SQLITE_OK) {
        html += "<table><tr><th>ID</th><th>Токен</th><th>UserID</th><th>Создано</th></tr>";
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            html += "<tr>";
            html += "<td>" + String(sqlite3_column_int(stmt, 0)) + "</td>";
            html += "<td>" + String((const char*)sqlite3_column_text(stmt, 1)) + "</td>";
            html += "<td>" + String(sqlite3_column_int(stmt, 2)) + "</td>";
            html += "<td>" + String(sqlite3_column_int(stmt, 3)) + "</td>";
            html += "</tr>";
        }
        sqlite3_finalize(stmt);
        html += "</table>";
    }
    // Содержимое SD
    html += "<h2>💾 Содержимое SD-карты</h2><pre>";
    File root = SD.open("/");
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            html += "📁 " + String(file.name()) + "<br>";
        } else {
            html += "📄 " + String(file.name()) + " (" + String(file.size()) + " байт)<br>";
        }
        file = root.openNextFile();
    }
    root.close();
    html += "</pre>";

    html += "<a href='/'>← Назад</a>";
    html += "</body></html>";

    server.send(200, "text/html", html);
#endif
}

// ====================== WiFi API ======================

// GET /api/scan — сканирует доступные Wi-Fi сети и возвращает JSON
void handleScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "\\\"");
        String enc = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
        json += "{\"ssid\":\"" + ssid + "\",\"encryption\":\"" + enc + "\"}";
        if (i < n - 1) json += ",";
    }
    json += "]";
    WiFi.scanDelete();
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "application/json", json);
}

// POST /api/configure — сохраняет SSID/пароль и подключается к новой сети
void handleConfigure() {
    if (!server.hasArg("ssid")) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Отсутствует SSID\"}");
        return;
    }

    String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    // Сохраняем учётные данные в NVS
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);

    WiFi.disconnect(true);
    delay(500);

    if (pass.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
    } else {
        WiFi.begin(ssid.c_str());
    }

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 50) {
        delay(500);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        Serial.println("[WiFi] Connected to " + ssid + " | IP: " + ip);
        server.send(200, "application/json", "{\"status\":\"success\",\"ip\":\"" + ip + "\",\"message\":\"Подключено!\"}");
    } else {
        server.send(200, "application/json", "{\"status\":\"error\",\"message\":\"Не удалось подключиться. Проверьте пароль.\"}");
    }
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(800);
  Serial.println("\nESP32-C3 Sensor Module Starting...");

  // Открываем хранилище Wi-Fi учётных данных
  prefs.begin("wifi-config", false);
  String savedSsid = prefs.getString("wifi_ssid", String(STA_SSID));
  String savedPass = prefs.getString("wifi_pass", String(STA_PASS));

  // Запускаем AP + STA одновременно
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(ap_ip, ap_ip, ap_mask);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.println("[WiFi] AP started: " + String(AP_SSID) + " | IP: " + WiFi.softAPIP().toString());

  WiFi.setHostname(MDNS_NAME);
  if (savedSsid.length() > 0) {
    if (savedPass.length() > 0) {
      WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    } else {
      WiFi.begin(savedSsid.c_str());
    }
    Serial.print("Connecting to WiFi: " + savedSsid);
    int attempts = 30;
    while (attempts-- && WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
    } else {
      Serial.println("\n[WiFi] STA connection failed, running AP-only mode");
    }
  } else {
    Serial.println("[WiFi] No STA credentials, running AP-only mode");
  }
  
  if (MDNS.begin(MDNS_NAME)) {
    Serial.println("mDNS: http://" + String(MDNS_NAME) + ".local");
  }
  
  // Инициализация SD-карты и БД — делается ДО NTP чтобы загрузить часовой пояс
  initDB();
  createSDDirectories();

  if (!db) {
    Serial.println("Внимание: БД не инициализирована! Сервер запустится, но API будет возвращать 503 до готовности.");
  } else {
    reloadTimezoneFromDB();
  }

  // NTP — после DB, чтобы использовать сохранённый часовой пояс
  configTime(tzOffsetSec, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
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
  // HTML страницы
    server.on("/", handleLogin);
    server.on("/login", handleLogin);
    server.on("/register", handleRegister);
    server.on("/app", handleRoot);
    server.on("/profile", handleProfile);
    server.on("/edit-profile", handleEditProfile);
    server.on("/settings", handleSettings);
    server.on("/plant", handlePlant);
    server.on("/index", handleIndex);

     // API маршруты
    server.on("/api/login", HTTP_POST, handleApiLogin);
    server.on("/api/register", HTTP_POST, handleApiRegister);
    server.on("/api/profile", HTTP_GET, handleGetProfile);
    server.on("/api/profile", HTTP_POST, handleUpdateProfile);
    server.on("/api/avatar", HTTP_POST, handleAvatarUploadDone, handleAvatarUpload);
    server.on("/api/avatar/file", HTTP_GET, handleGetAvatar);
    server.on("/api/logout", HTTP_POST, handleApiLogout);
    server.on("/api/change-password", HTTP_POST, handleChangePassword);
    server.on("/api/delete-account", HTTP_POST, handleDeleteAccount);

    // API для растений (добавьте вместе с другими API маршрутами)
    server.on("/api/plants", HTTP_GET, handleGetPlants);
    server.on("/api/plants", HTTP_POST, handleAddPlant);
    server.on("/api/plants", HTTP_PUT, handleUpdatePlant);
    server.on("/api/plants", HTTP_DELETE, handleDeletePlant);
    server.on("/api/plant/photo", HTTP_POST, handlePlantPhotoUploadDone, handlePlantPhotoUpload);
    server.on("/api/plant/photo/file", HTTP_GET, handleGetPlantPhoto);

    // Data API
    server.on("/data", handleData);
    server.on("/hist", handleHist);
    server.on("/time", handleTime);
    server.on("/clear", HTTP_POST, handleClear);
    server.on("/debug", handleDebug);

    // WiFi API (для страницы настроек)
    server.on("/api/scan", HTTP_GET, handleScan);
    server.on("/api/configure", HTTP_POST, handleConfigure);

    // ВАЖНО: WebServer по умолчанию НЕ сохраняет произвольные заголовки.
    // Без этого server.header("Authorization") вернёт "" и вся авторизация сломается.
    const char* headerKeys[] = {"Authorization", "Content-Type"};
    server.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(headerKeys[0]));

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
