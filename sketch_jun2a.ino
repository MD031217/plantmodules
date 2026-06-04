#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <WebServer.h>
#include <sqlite3.h>
#include <ArduinoJson.h>

#include "AmperkaFET.h"

#define AP_SSID "CollectorModuleNet"
#define AP_PASS "Qwe123!!"

#define PIN_CS_FET   1
#define PIN_CS_SD    7
#define NUM_VALVES   8

#define FLOW_RATE_ML 2.23
#define DB_PATH "/sd/watering.db"

FET mosfet(PIN_CS_FET);
WebServer server(80);

bool valveStates[NUM_VALVES] = {false};
volatile uint32_t flowmetr = 0;
sqlite3 *db = NULL;

char ap_ssid[]     = "RoboLab";
char ap_password[] = "Qwe123!!";
const char* espName = "mWatering";

IPAddress ap_ip(192, 168, 5, 1);
IPAddress ap_subnet(255, 255, 255, 0);
IPAddress ap_leaseStart(192, 168, 5, 2);
IPAddress ap_dns(192, 168, 5, 1);


bool initDatabase() {
  if (!SD.exists("/sd")) {
    SD.mkdir("/sd");
  }
  
  int rc = sqlite3_open(DB_PATH, &db);
  if (rc != SQLITE_OK) {
    Serial.printf("[SQLite] Ошибка открытия БД: %s\n", sqlite3_errmsg(db));
    return false;
  }

  const char* createValvesSQL = 
    "CREATE TABLE IF NOT EXISTS valves ("
    "  id INTEGER PRIMARY KEY,"
    "  plant_name TEXT DEFAULT '—',"
    "  active INTEGER DEFAULT 0"
    ");";
  
  const char* createJournalSQL = 
    "CREATE TABLE IF NOT EXISTS journal ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts TEXT NOT NULL,"
    "  valve_id INTEGER NOT NULL,"
    "  type TEXT NOT NULL,"
    "  duration_sec INTEGER NOT NULL,"
    "  volume_ml REAL NOT NULL,"
    "  status TEXT DEFAULT 'completed'"
    ");";

  char* errMsg = NULL;
  if (sqlite3_exec(db, createValvesSQL, NULL, NULL, &errMsg) != SQLITE_OK) {
    Serial.printf("[SQLite] Ошибка создания таблицы valves: %s\n", errMsg);
    sqlite3_free(errMsg);
  }
  if (sqlite3_exec(db, createJournalSQL, NULL, NULL, &errMsg) != SQLITE_OK) {
    Serial.printf("[SQLite] Ошибка создания таблицы journal: %s\n", errMsg);
    sqlite3_free(errMsg);
  }

  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM valves;", -1, &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      int count = sqlite3_column_int(stmt, 0);
      if (count == 0) {
        sqlite3_finalize(stmt);
        const char* insertSQL = "INSERT INTO valves (id, plant_name, active) VALUES (?, '—', 0);";
        sqlite3_prepare_v2(db, insertSQL, -1, &stmt, NULL);
        for (int i = 1; i <= NUM_VALVES; i++) {
          sqlite3_bind_int(stmt, 1, i);
          sqlite3_step(stmt);
          sqlite3_reset(stmt);
        }
        Serial.println("[SQLite] Таблица valves инициализирована 8 клапанами");
      } else {
        Serial.printf("[SQLite] В базе уже есть %d записей клапанов\n", count);
      }
    }
    sqlite3_finalize(stmt);
  }

  Serial.println("[SQLite] База данных успешно инициализирована");
  return true;
}

const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Модуль полива</title>
    <style>
        :root {
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
        * { box-sizing: border-box; margin: 0; padding: 0; }
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
        .mode-toggle { display: none !important; position: absolute; opacity: 0; pointer-events: none; }
        .top-sticky-wrapper { position: sticky; top: 0; z-index: 1000; width: 100%; background: transparent; pointer-events: none; }
        .top-sticky-wrapper > * { pointer-events: auto; }
        .site-header { display: flex; align-items: center; justify-content: space-between; padding: 20px 40px; background: transparent; backdrop-filter: blur(12px); -webkit-backdrop-filter: blur(12px); pointer-events: auto; position: relative; z-index: 2; }
        .user-pill { display: flex; align-items: center; text-decoration: none; background: var(--user-pill-bg); border-radius: 50px; padding: 8px 16px 8px 8px; gap: 10px; width: 145px; height: 44px; box-shadow: 0 0 8px var(--glow-soft); animation: userPulse 3s infinite alternate; flex-shrink: 0; }
        @keyframes userPulse { 0% { box-shadow: 0 0 6px var(--glow-soft); } 100% { box-shadow: 0 0 16px var(--glow-color); } }
        .user-avatar { width: 32px; height: 32px; border-radius: 50%; object-fit: cover; display: none; }
        body.theme-dark .avatar-dark { display: block; }
        body.theme-light .avatar-light { display: block; }
        .user-text { font-size: 14px; font-weight: 400; color: var(--user-pill-text); letter-spacing: 0.5px; }
        .logo-container { position: absolute; left: 50%; transform: translateX(-50%); display: flex; align-items: center; }
        .main-logo { height: 100px; display: none; transition: height 0.3s ease; }
        body.theme-dark .logo-dark { display: block; }
        body.theme-light .logo-light { display: block; }
        .theme-switcher { width: 48px; height: 48px; background: var(--switcher-bg); border-radius: 14px; display: flex; align-items: center; justify-content: center; cursor: pointer; box-shadow: 0 0 10px var(--glow-soft); animation: switcherPulse 3s infinite alternate; flex-shrink: 0; }
        @keyframes switcherPulse { 0% { box-shadow: 0 0 8px var(--glow-soft); } 100% { box-shadow: 0 0 18px var(--glow-color); } }
        .theme-icon-img { width: 24px; height: 24px; filter: invert(1); }
        .theme-light .theme-icon-img { filter: invert(0); }
        .navigation { display: flex; justify-content: center; align-items: center; gap: 8px; background: var(--nav-bg); border-radius: 40px; padding: 6px; margin: 0px 30px 35px 30px; height: 52px; pointer-events: auto; box-shadow: 0 6px 18px rgba(0, 0, 0, 0.3); }
        .nav-btn { color: #ffffff; text-decoration: none; font-size: 18px; font-weight: 600; padding: 8px 22px; border-radius: 30px; transition: all 0.3s ease; white-space: nowrap; position: relative; background: rgba(255, 255, 255, 0.1); backdrop-filter: blur(4px); -webkit-backdrop-filter: blur(4px); border: 1px solid rgba(255, 255, 255, 0.1); }
        .nav-btn:hover, .nav-btn.active { background: rgba(255, 255, 255, 0.35); backdrop-filter: blur(10px); -webkit-backdrop-filter: blur(10px); border: 1px solid rgba(255, 255, 255, 0.25); box-shadow: 0 4px 12px rgba(0,0,0,0.15); }
        .page-wrapper { flex: 1; padding: 0 20px; }
        .cards-area { display: flex; justify-content: center; align-items: center; gap: 40px; padding: 20px 0 60px 0; flex-wrap: wrap; }
        .plant-card { width: 350px; height: 420px; background: var(--card-bg); border-radius: 32px; display: flex; flex-direction: column; align-items: center; justify-content: center; position: relative; transition: transform 0.3s; box-shadow: var(--card-internal-shadow); }
        .plant-card:hover { transform: translateY(-5px); }
        .glowing { animation: glowPulse 3s infinite alternate; }
        @keyframes glowPulse { 0% { box-shadow: 0 0 12px var(--glow-color), var(--card-internal-shadow); } 100% { box-shadow: 0 0 28px var(--glow-color), var(--card-internal-shadow); } }
        .plant-img { width: 300px; height: 300px; object-fit: contain; margin-bottom: 16px; }
        .plant-label { background: var(--label-bg); color: var(--label-text); font-size: 19px; font-weight: 300; padding: 8px 32px; border-radius: 24px; text-decoration: none; display: inline-block; }
        .add-card { cursor: pointer; }
        .plus-icon { display: none; width: 90px; height: 90px; object-fit: contain; }
        body.theme-dark .icon-dark { display: block; }
        body.theme-light .icon-light { display: block; }
        .site-footer { background: var(--footer-bg); color: var(--footer-text); padding: 36px 40px 0; transition: background var(--transition-speed) ease; }
        .footer-columns { display: grid; grid-template-columns: 1.3fr 1fr 0.8fr; gap: 30px; max-width: 900px; margin: 0 auto; padding-bottom: 24px; border-bottom: 1px solid rgba(255,255,255,0.2); }
        .footer-column-title { font-size: 20px; font-weight: 700; margin-bottom: 12px; color: var(--footer-column-title); }
        .footer-column-text { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-column-list { list-style: none; padding: 0; }
        .footer-column-list li { font-size: 14px; line-height: 1.7; color: var(--footer-column-text); }
        .footer-copyright { text-align: center; padding: 16px 0 12px; font-size: 14px; color: var(--footer-copy); max-width: 900px; margin: 0 auto; }
        .falling-leaf { position: fixed; pointer-events: none; z-index: 9999; animation: fallingLeaf var(--fall-duration, 3s) linear forwards; }
        .add-link { display: flex; align-items: center; justify-content: center; }
        .modal-overlay { position: fixed; top: 0; left: 0; width: 100%; height: 100%; background-color: rgba(15, 24, 43, 0.6); backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px); z-index: 2000; display: flex; justify-content: center; align-items: center; }
        .modal-content { background: var(--card-bg); padding: 30px; border-radius: 32px; width: 90%; max-width: 450px; position: relative; box-shadow: 0 0 30px var(--glow-soft); animation: modalFadeIn 0.3s ease; text-align: center; color: var(--text-main); }
        .modal-content h3, .modal-content .modal-title, .modal-content .modal-title-sub { color: var(--accent-green); font-size: 18px; font-weight: 600; margin-bottom: 15px; line-height: 1.4; }
        .modal-title-sub { margin-top: 20px; font-size: 16px; }
        @keyframes modalFadeIn { from { opacity: 0; transform: translateY(20px); } to { opacity: 1; transform: translateY(0); } }
        .modal-close { position: absolute; top: 15px; right: 20px; background: var(--accent-green); border: none; color: white; width: 28px; height: 28px; border-radius: 50%; cursor: pointer; font-size: 16px; display: flex; align-items: center; justify-content: center; }
        .modal-input { width: 100%; padding: 14px 20px; border-radius: 24px; border: 2px solid var(--accent-green); background: transparent; font-size: 16px; color: var(--accent-green); box-sizing: border-box; margin-bottom: 15px; outline: none; }
        .modal-input::placeholder { color: var(--accent-green); opacity: 0.5; }
        .modal-photo-area { margin: 20px 0; }
        .modal-preview-area { margin: 15px 0 20px 0; display: flex; flex-direction: column; align-items: center; }
        .plant-preview { width: 120px; height: 120px; object-fit: contain; margin-bottom: 5px; }
        .modal-caption { font-size: 12px; color: var(--text-main); opacity: 0.6; display: block; margin-bottom: 5px; }
        .modal-link { color: var(--accent-green); font-size: 18px; font-weight: 600; text-decoration: underline; cursor: pointer; }
        .modal-btn { width: 100%; padding: 14px; border-radius: 24px; border: none; background: rgba(16, 185, 129, 0.3); color: rgba(255, 255, 255, 0.5); font-size: 20px; font-weight: 700; cursor: not-allowed; margin-top: 10px; transition: all 0.3s ease; }
        .modal-btn.active { background: var(--accent-green); color: #ffffff; cursor: pointer; box-shadow: 0 4px 15px rgba(0,0,0,0.2); }
        @media (max-width: 480px) { .modal-content { padding: 20px; width: 95%; } .modal-input { padding: 12px 16px; font-size: 14px; } }
        .plants-container { display: contents; }
        .delete-plant-btn { position: absolute; top: 15px; right: 15px; width: 32px; height: 32px; border-radius: 50%; border: none; background: rgba(255, 0, 0, 0.8); color: white; font-size: 22px; font-weight: bold; cursor: pointer; display: flex; align-items: center; justify-content: center; opacity: 0; transition: all 0.3s ease; z-index: 10; box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3); }
        .plant-card:hover .delete-plant-btn { opacity: 1; }
        .delete-plant-btn:hover { background: rgba(255, 0, 0, 1); transform: scale(1.1); box-shadow: 0 4px 12px rgba(255, 0, 0, 0.4); }
        .modal-overlay { display: none; }
        .edit-plant-btn { position: absolute; top: 15px; left: 15px; width: 32px; height: 32px; border-radius: 50%; border: none; background: rgba(33, 200, 95, 0.8); color: white; font-size: 16px; cursor: pointer; display: flex; align-items: center; justify-content: center; opacity: 0; transition: all 0.3s ease; z-index: 10; box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3); }
        .plant-card:hover .edit-plant-btn { opacity: 1; }
        .edit-plant-btn:hover { background: var(--accent-green); transform: scale(1.1); box-shadow: 0 4px 12px rgba(33, 200, 95, 0.4); }
        @media (max-width: 768px) { .edit-plant-btn { opacity: 1; top: 10px; left: 10px; width: 28px; height: 28px; font-size: 14px; } }
        @media (max-width: 480px) { .edit-plant-btn { width: 26px; height: 26px; font-size: 12px; } }
        @media (max-width: 768px) { .delete-plant-btn { opacity: 1; top: 10px; right: 10px; width: 28px; height: 28px; font-size: 18px; } }
        @media (max-width: 480px) { .delete-plant-btn { width: 26px; height: 26px; font-size: 16px; } }
        .modal-params-list { display: flex; flex-direction: column; gap: 12px; margin-bottom: 20px; }
        .modal-step { animation: modalFadeIn 0.3s ease; }
        @keyframes fallingLeaf { 0% { transform: translateY(0) translateX(0) rotate(0deg); opacity: 1; } 100% { transform: translateY(110vh) translateX(var(--sway, 30px)) rotate(var(--rotation, 20deg)); opacity: 0; } }
        @media (max-width: 768px) {
            .site-header { padding: 12px 16px; flex-wrap: wrap; gap: 12px; }
            .logo-container { position: relative !important; left: auto !important; transform: none !important; order: -1; width: 100%; justify-content: center; margin-bottom: 0; }
            .main-logo { height: 80px; }
            .user-pill { width: auto; padding: 6px 12px 6px 6px; height: 38px; order: 1; margin-right: auto; animation: none; }
            .user-avatar { width: 26px; height: 26px; }
            .user-text { font-size: 13px; }
            .theme-switcher { position: relative !important; top: auto !important; right: auto !important; width: 42px; height: 42px; order: 2; animation: none; }
            .theme-icon-img { width: 20px; height: 20px; }
            .navigation { order: 3; margin: 8px 12px 20px 12px; flex-wrap: wrap; height: auto; gap: 6px; padding: 6px; justify-content: center; }
            .nav-btn { padding: 8px 16px; font-size: 15px; flex: 0 1 auto; }
            .page-wrapper { padding: 0 12px; }
            .cards-area { gap: 20px; padding: 10px 0 40px 0; }
            .plant-card { width: 100%; max-width: 340px; height: auto; min-height: 360px; padding: 20px; }
            .plant-img { width: 200px; height: 200px; }
            .plant-label { font-size: 17px; padding: 6px 24px; }
            .plus-icon { width: 70px; height: 70px; }
            .footer-grid { flex-direction: column; text-align: center; gap: 30px; padding: 30px 20px 20px 20px; }
            .footer-col { text-align: center !important; }
            .footer-col h3 { font-size: 19px; margin-bottom: 10px; }
            .footer-col p { font-size: 13px; line-height: 1.6; }
            .copyright-bar { padding: 12px; font-size: 12px; }
            .falling-leaf { display: none; }
        }
        @media (max-width: 480px) {
            .site-header { padding: 10px 12px; }
            .main-logo { height: 80px; }
            .user-pill { padding: 5px 10px 5px 5px; height: 34px; gap: 8px; }
            .user-avatar { width: 24px; height: 24px; }
            .user-text { font-size: 12px; }
            .theme-switcher { width: 38px; height: 38px; }
            .navigation { margin: 0px 8px 16px 8px; padding: 5px; gap: 5px; }
            .nav-btn { padding: 7px 12px; font-size: 14px; border-radius: 24px; }
            .plant-card { padding: 16px; min-height: 320px; border-radius: 24px; }
            .plant-img { width: 160px; height: 160px; margin-bottom: 12px; }
            .plant-label { font-size: 16px; padding: 6px 20px; border-radius: 20px; }
            .plus-icon { width: 60px; height: 60px; }
            .footer-grid { padding: 25px 16px 16px 16px; gap: 24px; }
            .footer-col h3 { font-size: 17px; }
            .footer-col p { font-size: 12px; }
        }
        .irrigation-container { display: flex; gap: 20px; padding: 20px 40px; min-height: calc(100vh - 300px); }
        .valves-sidebar { width: 300px; flex-shrink: 0; }
        .valve-item { background: var(--card-bg); border-radius: 16px; padding: 16px; margin-bottom: 12px; box-shadow: var(--card-internal-shadow); cursor: pointer; transition: all 0.3s ease; color: #1e293b; }
        .valve-item:hover { transform: translateX(5px); box-shadow: 0 0 20px var(--glow-soft); }
        .valve-item.active { border: 2px solid var(--accent-green); background: rgba(33, 200, 95, 0.05); }
        .valve-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
        .valve-title { font-size: 16px; font-weight: 600; color: var(--accent-green); }
        .valve-toggle { width: 40px; height: 22px; background: rgba(0,0,0,0.15); border-radius: 11px; position: relative; cursor: pointer; transition: background 0.3s; }
        .valve-toggle.on { background: var(--accent-green); }
        .valve-toggle::after { content: ''; position: absolute; width: 18px; height: 18px; background: white; border-radius: 50%; top: 2px; left: 2px; transition: transform 0.3s; }
        .valve-toggle.on::after { transform: translateX(18px); }
        .valve-info { font-size: 13px; color: #374151; margin: 4px 0; }
        .valve-status { display: inline-flex; align-items: center; gap: 5px; font-size: 12px; padding: 3px 8px; background: rgba(33, 200, 95, 0.15); border-radius: 8px; margin-top: 6px; color: var(--accent-green); font-weight: 600; }
        .valve-status.inactive { background: rgba(100, 116, 139, 0.15); color: #64748b; }
        .main-content { flex: 1; background: var(--card-bg); border-radius: 24px; padding: 30px; box-shadow: var(--card-internal-shadow); max-height: calc(100vh - 220px); overflow-y: auto; color: #1e293b; }
        .content-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 30px; }
        .page-title { font-size: 28px; font-weight: 700; color: var(--accent-green) !important; }
        .btn-add-task { background: var(--accent-green); color: white; border: none; padding: 12px 24px; border-radius: 20px; font-size: 16px; font-weight: 600; cursor: pointer; transition: all 0.3s; }
        .btn-add-task:hover { transform: translateY(-2px); box-shadow: 0 4px 15px rgba(33, 200, 95, 0.4); }
        .tabs { display: flex; gap: 10px; margin-bottom: 25px; border-bottom: 2px solid rgba(0,0,0,0.05); padding-bottom: 10px; }
        .tab { padding: 10px 20px; border: none; border-radius: 12px; font-size: 15px; cursor: pointer; transition: all 0.3s; background: rgba(0,0,0,0.05); color: #1e293b; }
        .tab.active { background: var(--accent-green); color: white !important; }
        .form-group { margin-bottom: 20px; }
        .form-label { display: block; font-size: 14px; color: #374151 !important; margin-bottom: 8px; font-weight: 600; }
        .form-hint { font-size: 12px; color: #64748b !important; margin-top: 4px; font-style: italic; opacity: 0.9; }
        .form-input, .form-select { width: 100%; padding: 12px 16px; background: rgba(0,0,0,0.03); border: 2px solid rgba(0,0,0,0.1); border-radius: 12px; color: #1e293b !important; font-size: 15px; font-weight: 500; outline: none; transition: border-color 0.3s; }
        .form-input:focus, .form-select:focus { border-color: var(--accent-green); }
        .form-input::placeholder, .form-select::placeholder { color: #64748b; }
        .form-input:disabled, .form-select:disabled { background: rgba(0,0,0,0.01); border-color: rgba(0,0,0,0.05); color: #94a3b8 !important; cursor: not-allowed; }
        .form-row { display: grid; grid-template-columns: repeat(3, 1fr); gap: 15px; }
        .form-row-2 { display: grid; grid-template-columns: repeat(2, 1fr); gap: 15px; }
        .operations-list { margin: 25px 0; }
        .operation-item { background: rgba(0,0,0,0.03); border-radius: 12px; padding: 15px; margin-bottom: 12px; display: flex; align-items: center; gap: 15px; }
        .operation-type { padding: 8px 16px; border-radius: 10px; font-weight: 600; font-size: 13px; min-width: 120px; text-align: center; }
        .operation-type.water { background: rgba(33, 200, 95, 0.2); color: var(--accent-green); }
        .operation-type.pause { background: rgba(255, 193, 7, 0.2); color: #b45309; }
        .operation-type.sensor { background: rgba(0, 188, 212, 0.2); color: #0e7490; }
        .operation-inputs { flex: 1; display: flex; gap: 10px; }
        .operation-input { flex: 1; padding: 8px 12px; background: rgba(0,0,0,0.03); border: 1px solid rgba(0,0,0,0.1); border-radius: 8px; color: #1e293b !important; font-size: 14px; font-weight: 500; }
        .btn-operation { padding: 8px 16px; border: none; border-radius: 8px; cursor: pointer; font-size: 13px; font-weight: 600; transition: all 0.3s; }
        .btn-edit { background: var(--accent-green); color: white; }
        .btn-delete { background: rgba(244, 67, 54, 0.85); color: white; }
        .btn-operation:hover { transform: translateY(-1px); box-shadow: 0 2px 8px rgba(0,0,0,0.2); }
        .sidebar-right { width: 340px; flex-shrink: 0; }
        .moisture-panel { background: var(--card-bg); border-radius: 24px; padding: 25px; box-shadow: var(--card-internal-shadow); color: #1e293b; }
        .panel-title { font-size: 20px; font-weight: 600; color: var(--accent-green) !important; margin-bottom: 20px; padding-bottom: 15px; border-bottom: 2px solid rgba(0,0,0,0.05); }
        .toggle-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; }
        .toggle-label { font-size: 14px; color: #374151 !important; }
        .global-limits { margin-top: 20px; padding-top: 20px; border-top: 2px solid rgba(0,0,0,0.05); }
        .action-buttons { display: flex; gap: 12px; margin-top: 25px; }
        .btn-action { flex: 1; padding: 14px; border: none; border-radius: 16px; font-size: 15px; font-weight: 600; cursor: pointer; transition: all 0.3s; background: rgba(0,0,0,0.05); color: #1e293b; }
        .btn-action.btn-save { background: var(--accent-green); color: white; }
        .btn-action.btn-apply { background: rgba(33, 200, 95, 0.15); color: var(--accent-green); }
        .btn-action.btn-history { background: rgba(0,0,0,0.05); color: #1e293b; }
        .btn-action:hover { transform: translateY(-2px); box-shadow: 0 4px 12px rgba(0,0,0,0.15); }
        .valve-number { display: inline-block; width: 28px; height: 28px; background: var(--accent-green); color: white; border-radius: 8px; text-align: center; line-height: 28px; font-weight: 700; margin-right: 10px; }
        .section-title { font-size: 18px; font-weight: 600; color: var(--accent-green) !important; margin: 25px 0 15px 0; padding-bottom: 8px; border-bottom: 1px solid rgba(0,0,0,0.05); }
        #journalPanel { display: none; }
        .journal-table { width: 100%; border-collapse: collapse; margin-top: 15px; font-size: 14px; }
        .journal-table th, .journal-table td { padding: 12px; text-align: left; border-bottom: 1px solid rgba(0,0,0,0.1); }
        .journal-table th { color: var(--accent-green); font-weight: 600; }
        .journal-table tr:hover { background: rgba(33, 200, 95, 0.05); }
        .status-badge { padding: 4px 8px; border-radius: 6px; font-size: 12px; font-weight: 600; }
        .status-success { background: rgba(33, 200, 95, 0.2); color: var(--accent-green); }
        .status-fail { background: rgba(244, 67, 54, 0.2); color: #dc2626; }
        .status-pending { background: rgba(255, 193, 7, 0.2); color: #b45309; }
        .empty-journal { text-align: center; padding: 40px; color: #64748b; font-style: italic; }
        .edit-valve-btn { position: absolute; bottom: 12px; right: 12px; width: 32px; height: 32px; border-radius: 50%; border: none; background: rgba(33, 200, 95, 0.85); color: white; font-size: 16px; cursor: pointer; display: flex; align-items: center; justify-content: center; opacity: 0; transition: all 0.25s ease; z-index: 15; box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3); padding: 0; line-height: 1; }
        .edit-valve-btn:hover { background: var(--accent-green, #21C85F); transform: scale(1.1); }
        .valve-card:hover .edit-valve-btn, [id*="valve"]:hover .edit-valve-btn, .card:hover .edit-valve-btn, .module-card:hover .edit-valve-btn, div[class*="valve"]:hover .edit-valve-btn { opacity: 1; }
        .valve-editor-mini { position: fixed; background: var(--card-bg, #ffffff); padding: 24px 28px; border-radius: 32px; width: 90%; max-width: 320px; box-shadow: 0 0 30px var(--glow-soft, rgba(33, 200, 95, 0.3)); animation: modalFadeIn 0.3s ease; z-index: 2147483640; font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; color: var(--text-main, #000); }
        .valve-editor-title { color: var(--accent-green, #21C85F); font-size: 18px; font-weight: 600; margin-bottom: 18px; text-align: center; }
        .valve-editor-field { margin-bottom: 14px; }
        .valve-editor-field label { display: block; font-size: 13px; color: var(--text-main, #333); opacity: 0.85; margin-bottom: 6px; }
        .valve-editor-field input { width: 100%; padding: 12px 16px; border-radius: 24px; border: 2px solid var(--accent-green, #21C85F); background: transparent; font-size: 14px; color: var(--accent-green, #21C85F); box-sizing: border-box; outline: none; }
        .valve-editor-actions { display: flex; gap: 10px; margin-top: 20px; }
        .valve-editor-actions button { flex: 1; padding: 12px; border-radius: 24px; border: none; font-size: 14px; font-weight: 600; cursor: pointer; transition: all 0.3s ease; }
        .valve-save-btn { background: var(--accent-green, #21C85F); color: #ffffff; }
        .valve-save-btn:hover { box-shadow: 0 4px 15px rgba(33, 200, 95, 0.4); transform: translateY(-1px); }
        .valve-cancel-btn { background: rgba(255, 255, 255, 0.15); color: var(--text-main, #333); border: 1px solid rgba(255, 255, 255, 0.2); }
        .valve-cancel-btn:hover { background: rgba(255, 255, 255, 0.25); }
        .valve-editor-backdrop { position: fixed; top: 0; left: 0; width: 100%; height: 100%; background-color: rgba(15, 24, 43, 0.6); backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px); z-index: 2147483639; }
        .btn-add-task { background: var(--accent-green, #21C85F); color: white !important; border: none; padding: 10px 20px; border-radius: 20px; font-size: 14px; font-weight: 600; cursor: pointer; transition: all 0.3s; display: inline-flex; align-items: center; gap: 6px; }
        .btn-add-task:hover { transform: translateY(-2px); box-shadow: 0 4px 15px rgba(33, 200, 95, 0.4); }
        .btn-add-task:active { transform: translateY(0); }

        .esp-status {
            display: inline-flex;
            align-items: center;
            gap: 6px;
            padding: 4px 12px;
            border-radius: 12px;
            font-size: 12px;
            font-weight: 600;
            margin-left: 12px;
            vertical-align: middle;
        }
        .esp-status.online  { background: rgba(33,200,95,0.15); color: var(--accent-green); }
        .esp-status.offline { background: rgba(244,67,54,0.15); color: #dc2626; }
        .esp-dot {
            width: 8px; height: 8px; border-radius: 50%;
            display: inline-block;
        }
        .esp-status.online  .esp-dot { background: var(--accent-green); box-shadow: 0 0 6px var(--accent-green); }
        .esp-status.offline .esp-dot { background: #dc2626; }

        .journal-filter-bar {
            display: flex;
            gap: 12px;
            align-items: center;
            flex-wrap: nowrap;
            margin-bottom: 15px;
            padding: 10px 14px;
            background: rgba(33, 200, 95, 0.04);
            border: 1px solid rgba(33, 200, 95, 0.15);
            border-radius: 14px;
        }
        .journal-filter-bar label {
            font-size: 14px;
            color: #374151;
            font-weight: 600;
            flex-shrink: 0;
        }
        .journal-filter-bar .form-select {
            width: auto;
            min-width: 150px;
            padding: 7px 12px;
            margin: 0;
            flex-shrink: 0;
        }
        .journal-total-box {
            display: inline-flex;
            align-items: center;
            gap: 6px;
            padding: 6px 14px;
            background: rgba(33, 200, 95, 0.12);
            border-radius: 12px;
            flex-shrink: 0;
        }
        .journal-total-box .label {
            font-size: 13px;
            color: #374151;
            font-weight: 500;
        }
        .journal-total-box .value {
            font-weight: 700;
            color: var(--accent-green);
            font-size: 15px;
            white-space: nowrap;
        }
        .journal-count-box {
            display: inline-flex;
            align-items: center;
            gap: 5px;
            padding: 6px 10px;
            background: rgba(0, 0, 0, 0.04);
            border-radius: 10px;
            font-size: 13px;
            color: #374151;
            flex-shrink: 0;
            white-space: nowrap;
        }
        .journal-count-box b { color: var(--accent-green); }

        .btn-clear-journal {
            background: rgba(244, 67, 54, 0.12);
            color: #dc2626;
            border: 1px solid rgba(244, 67, 54, 0.3);
            width: 36px;
            height: 36px;
            border-radius: 10px;
            font-size: 16px;
            cursor: pointer;
            transition: all 0.3s;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            flex-shrink: 0;
            padding: 0;
            line-height: 1;
        }
        .btn-clear-journal:hover {
            background: rgba(244, 67, 54, 0.22);
            transform: translateY(-1px);
            box-shadow: 0 2px 8px rgba(244, 67, 54, 0.25);
        }
        .btn-clear-journal:active {
            transform: translateY(0);
        }
        .btn-clear-journal:disabled {
            opacity: 0.4;
            cursor: not-allowed;
            transform: none;
        }
        
        .ntp-clock {
            text-align: center;
            font-size: 14px;
            color: var(--user-pill-text);
            margin-bottom: 8px;
            font-weight: 500;
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
                <div class="ntp-clock" id="ntpClock">Загрузка времени...</div>
                <div class="content-header">
                    <h1 class="page-title">Конфигурация модуля полива
                        <span class="esp-status offline" id="espStatus"><span class="esp-dot"></span>Контроллер</span>
                    </h1>
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
                                <span style="color: #64748b; font-size: 14px;">Объём воды:</span>
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
                                <option value="1">Клапан 1</option><option value="2">Клапан 2</option>
                                <option value="3">Клапан 3</option><option value="4">Клапан 4</option>
                                <option value="5">Клапан 5</option><option value="6">Клапан 6</option>
                                <option value="7">Клапан 7</option><option value="8">Клапан 8</option>
                            </select>
                        </div>
                        <div class="form-row-2">
                            <div class="form-group">
                                <label class="form-label">Тип расписания</label>
                                <select class="form-select" id="input-schedule-type">
                                    <option value="daily">Ежедневно</option><option value="weekly">Еженедельно</option>
                                    <option value="interval">По интервалу</option><option value="once">Однократно</option>
                                    <option value="sunrise">На рассвете</option><option value="sunset">На закате</option>
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

                    <div class="journal-filter-bar">
                        <label for="journal-valve-filter">Показать:</label>
                        <select id="journal-valve-filter" class="form-select">
                            <option value="0">Все клапаны</option>
                            <option value="1">Клапан 1</option>
                            <option value="2">Клапан 2</option>
                            <option value="3">Клапан 3</option>
                            <option value="4">Клапан 4</option>
                            <option value="5">Клапан 5</option>
                            <option value="6">Клапан 6</option>
                            <option value="7">Клапан 7</option>
                            <option value="8">Клапан 8</option>
                        </select>
                        <div class="journal-count-box">
                            Записей: <b id="journal-count">0</b>
                        </div>
                        <div class="journal-total-box">
                            <span class="label">Всего вылилось:</span>
                            <span class="value" id="journal-total-volume">0.00 мл</span>
                        </div>
                        <button class="btn-clear-journal" id="btn-clear-journal" onclick="clearValveJournal()" title="Очистить записи">
                            🗑
                        </button>
                    </div>

                    <div id="journal-content">
                        <table class="journal-table">
                            <thead><tr>
                                <th>Дата/Время</th><th>Клапан</th><th>Тип запуска</th>
                                <th>Длительность</th><th>Объём</th><th>Статус</th>
                            </tr></thead>
                            <tbody id="journal-tbody"></tbody>
                        </table>
                        <div id="journal-empty" class="empty-journal">Журнал пока пуст.</div>
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
                            <option value="15">15 минут</option><option value="30">30 минут</option><option value="60">1 час</option>
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
                <p class="footer-column-text">Система контроля микроклимата растений.</p>
            </div>
            <div>
                <h3 class="footer-column-title">Модули</h3>
                <ul class="footer-column-list">
                    <li>Освещения растений</li><li>Сбор данных о растениях</li><li>Полив растений</li>
                </ul>
            </div>
            <div>
                <h3 class="footer-column-title">Контакты</h3>
                <ul class="footer-column-list"><li>Почта</li><li>Наши сети</li></ul>
            </div>
        </div>
        <div class="footer-copyright">2026 Зелёная полка</div>
    </footer>

<script>
let appState = {
    valves: {},
    journal: [],
    currentValve: 1,
    manualState: { isActive: false, startTime: null, valveId: null, timerId: null }
};

async function init() {
    loadTheme();
    setupThemeToggle();
    await loadFromDB();
    renderSidebar();
    renderJournal();
    setupListeners();
    startClock();
    syncWithESP();
}

function loadTheme() {
    const savedTheme = localStorage.getItem('theme') || 'dark';
    document.body.className = 'theme-' + savedTheme;
}

function setupThemeToggle() {
    const themeSwitcher = document.querySelector('.theme-switcher');
    if (themeSwitcher) {
        themeSwitcher.addEventListener('click', () => {
            const isDark = document.body.classList.contains('theme-dark');
            const newTheme = isDark ? 'light' : 'dark';
            document.body.className = 'theme-' + newTheme;
            localStorage.setItem('theme', newTheme);
        });
    }
}

function startClock() {
    function updateClock() {
        const now = new Date();
        const dateStr = now.toLocaleDateString('ru-RU', { 
            day: '2-digit', month: '2-digit', year: 'numeric' 
        });
        const timeStr = now.toLocaleTimeString('ru-RU');
        document.getElementById('ntpClock').textContent = `${dateStr} ${timeStr}`;
    }
    updateClock();
    setInterval(updateClock, 1000);
}

async function loadFromDB() {
    try {
        const [valvesRes, journalRes] = await Promise.all([
            fetch('/api/valves'),
            fetch('/api/journal')
        ]);
        
        const valvesData = await valvesRes.json();
        const journalData = await journalRes.json();
        
        appState.valves = {};
        valvesData.forEach(v => {
            appState.valves[v.id] = {
                active: v.active,
                plant_name: v.plant_name,
                max_duration_sec: v.max_duration_sec,
                daily_limit_ml: v.daily_limit_ml
            };
        });
        
        appState.journal = journalData;
        
        document.getElementById('espStatus').className = 'esp-status online';
        document.getElementById('espStatus').innerHTML = '<span class="esp-dot"></span>Подключён';
    } catch (e) {
        console.error('Ошибка загрузки из БД', e);
        document.getElementById('espStatus').className = 'esp-status offline';
        document.getElementById('espStatus').innerHTML = '<span class="esp-dot"></span>Нет связи';
    }
}

async function saveValveToDB(valveId, data) {
    try {
        await fetch('/api/valves', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({ id: valveId, ...data })
        });
        appState.valves[valveId] = { ...appState.valves[valveId], ...data };
    } catch(e) {
        console.error('Ошибка сохранения клапана', e);
    }
}

async function addJournalEntryToDB(entry) {
    try {
        await fetch('/api/journal', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(entry)
        });
        await loadFromDB();
        renderJournal();
    } catch(e) {
        console.error('Ошибка записи в журнал', e);
    }
}

async function clearJournalInDB(valveId) {
    try {
        const url = valveId === 0 ? '/api/journal?all=1' : `/api/journal?valve_id=${valveId}`;
        await fetch(url, { method: 'DELETE' });
        await loadFromDB();
        renderJournal();
    } catch(e) {
        console.error('Ошибка очистки журнала', e);
    }
}

function renderSidebar() {
    const container = document.getElementById('valvesSidebar');
    if (!container) return;
    container.innerHTML = '';
    
    for (let i = 1; i <= 8; i++) {
        const d = appState.valves[i] || { active: 0, plant_name: '—'};
        const statusText = d.active ? 'Active' : 'Неактивен';
        const statusClass = d.active ? '' : 'inactive';
        
        container.innerHTML += `
            <div class="valve-item ${appState.currentValve === i ? 'active' : ''}" data-valve="${i}" onclick="switchValve(${i})">
                <div class="valve-header"><span class="valve-title"><span class="valve-number">${i}</span>Клапан ${i}</span></div>
                <div class="valve-info">Растение: ${d.plant_name}</div>
                <span class="valve-status ${statusClass}">${statusText}</span>
            </div>`;
    }
}

function renderJournal() {
    const filterValve = parseInt(document.getElementById('journal-valve-filter').value) || 0;
    const filtered = filterValve === 0 ? appState.journal : appState.journal.filter(l => l.valve_id === filterValve);
    
    const totalVolume = filtered.reduce((sum, log) => sum + (Number(log.volume_ml) || 0), 0);
    document.getElementById('journal-total-volume').textContent = totalVolume.toFixed(2) + ' мл';
    document.getElementById('journal-count').textContent = filtered.length;

    const tbody = document.getElementById('journal-tbody');
    const emptyMsg = document.getElementById('journal-empty');
    tbody.innerHTML = '';
    
    if (filtered.length === 0) {
        emptyMsg.style.display = 'block';
        return;
    }
    emptyMsg.style.display = 'none';

    filtered.sort((a, b) => new Date(b.ts) - new Date(a.ts)).forEach(log => {
        const statusClass = log.status === 'completed' ? 'status-success' : 'status-fail';
        const typeText = { schedule: 'Расписание', sensor: 'Датчик', manual: 'Вручную' }[log.type] || log.type;
        const volStr = Number(log.volume_ml).toFixed(2);
        
        tbody.innerHTML += `<tr>
            <td>${new Date(log.ts).toLocaleString('ru-RU')}</td>
            <td><b>Клапан ${log.valve_id}</b></td>
            <td>${typeText}</td>
            <td>${log.duration_sec} сек</td>
            <td>${volStr} мл</td>
            <td><span class="status-badge ${statusClass}">${log.status === 'completed' ? 'Успешно' : 'Провалено'}</span></td>
        </tr>`;
    });
}

function switchValve(id) {
    appState.currentValve = id;
    renderSidebar();
    document.getElementById('input-valve-select').value = id;
}

async function startWatering() {
    if (appState.manualState.isActive) return;
    const sel = document.getElementById('manual-valve-select');
    appState.manualState.valveId = parseInt(sel.value);

    try {
        const res = await fetch(`/valve?id=${appState.manualState.valveId - 1}&state=1`);
        if (!(await res.json()).ok) throw new Error('Ошибка');
    } catch (e) {
        showNotification('Контроллер недоступен');
        return;
    }

    appState.manualState.startTime = Date.now();
    appState.manualState.isActive = true;
    
    await saveValveToDB(appState.manualState.valveId, { active: 1 });
    renderSidebar();

    appState.manualState.timerId = setInterval(() => {
        const elapsed = Math.floor((Date.now() - appState.manualState.startTime) / 1000);
        const m = Math.floor(elapsed / 60).toString().padStart(2, '0');
        const s = (elapsed % 60).toString().padStart(2, '0');
        document.getElementById('manual-timer').textContent = `${m}:${s}`;
    }, 1000);

    document.getElementById('manual-status').textContent = 'Работает';
    document.getElementById('manual-status').style.color = '#22c55e';
    document.getElementById('manual-volume').textContent = 'Измерение...';
    document.getElementById('btn-manual-on').disabled = true;
    document.getElementById('btn-manual-off').disabled = false;
    showNotification(`Клапан ${appState.manualState.valveId} открыт`);
}

async function stopWatering() {
    if (!appState.manualState.isActive) return;
    let realVolumeMl = 0;
    let pulses = 0;

    try { await fetch(`/valve?id=${appState.manualState.valveId - 1}&state=0`); } catch(e) {}

    try {
        const flowRes = await fetch('/flowmeter');
        const flowData = await flowRes.json();
        if (flowData.ok) {
            pulses = flowData.pulses || 0;
            realVolumeMl = flowData.volume_ml || 0;
        }
    } catch(e) {}

    clearInterval(appState.manualState.timerId);
    const duration = Math.floor((Date.now() - appState.manualState.startTime) / 1000);
    appState.manualState.isActive = false;

    await saveValveToDB(appState.manualState.valveId, { active: 0 });
    renderSidebar();

    const volRounded = Number(realVolumeMl.toFixed(2));
    const entry = {
        ts: new Date().toISOString(),
        valve_id: appState.manualState.valveId,
        type: 'manual',
        duration_sec: duration,
        volume_ml: volRounded,
        status: 'completed'
    };

    await addJournalEntryToDB(entry);

    document.getElementById('manual-timer').textContent = '00:00';
    document.getElementById('manual-volume').textContent = volRounded.toFixed(2) + ' мл';
    document.getElementById('manual-status').textContent = 'Ожидание';
    document.getElementById('manual-status').style.color = 'var(--accent-green)';
    document.getElementById('btn-manual-on').disabled = false;
    document.getElementById('btn-manual-off').disabled = true;
    
    showNotification(`Клапан ${appState.manualState.valveId}: ${Math.floor(duration/60)}м ${duration%60}с, ${volRounded.toFixed(2)} мл`);
}

function clearValveJournal() {
    const filterValve = parseInt(document.getElementById('journal-valve-filter').value) || 0;
    const count = filterValve === 0 ? appState.journal.length : appState.journal.filter(l => l.valve_id === filterValve).length;
    
    if (count === 0) { showNotification('Журнал уже пуст'); return; }
    if (!confirm(`Удалить ${count} записей?`)) return;
    
    clearJournalInDB(filterValve).then(() => {
        showNotification(filterValve === 0 ? 'Журнал полностью очищен' : `Записи клапана ${filterValve} удалены`);
    });
}

function saveAllData() {
    showNotification('Настройки сохранены в БД');
}

function showNotification(msg) {
    const ex = document.getElementById('gs-notify'); if (ex) ex.remove();
    const n = document.createElement('div');
    n.id = 'gs-notify';
    n.style.cssText = 'position:fixed;top:20px;right:20px;background:var(--accent-green);color:#fff;padding:12px 24px;border-radius:12px;z-index:9999;box-shadow:0 4px 12px rgba(0,0,0,.2);transition:opacity .3s;';
    n.textContent = msg;
    document.body.appendChild(n);
    setTimeout(() => { n.style.opacity = '0'; setTimeout(() => n.remove(), 300); }, 2500);
}

function setupListeners() {
    document.querySelectorAll('.tab').forEach(tab => tab.onclick = function() {
        document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
        this.classList.add('active');
        document.getElementById('manualPanel').style.display = this.dataset.tab === 'manual' ? 'block' : 'none';
        document.getElementById('configPanel').style.display = this.dataset.tab === 'config' ? 'block' : 'none';
        document.getElementById('journalPanel').style.display = this.dataset.tab === 'journal' ? 'block' : 'none';
        if (this.dataset.tab === 'journal') renderJournal();
    });
    document.getElementById('journal-valve-filter').onchange = renderJournal;
}

async function syncWithESP() {
    setInterval(async () => {
        try {
            const res = await fetch('/states');
            const data = await res.json();
            let changed = false;
            data.states.forEach((state, idx) => {
                const id = idx + 1;
                if (appState.valves[id] && appState.valves[id].active !== (state ? 1 : 0)) {
                    appState.valves[id].active = state ? 1 : 0;
                    changed = true;
                }
            });
            if (changed) renderSidebar();
        } catch(e) {}
    }, 3000);
}

document.addEventListener('DOMContentLoaded', init);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

void handleApiValvesGet() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  
  const char* sql = "SELECT id, plant_name, max_duration_sec, daily_limit_ml, active FROM valves ORDER BY id;";
  sqlite3_stmt* stmt;
  
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      JsonObject obj = arr.add<JsonObject>();
      obj["id"] = sqlite3_column_int(stmt, 0);
      obj["plant_name"] = (const char*)sqlite3_column_text(stmt, 1);
      obj["max_duration_sec"] = sqlite3_column_int(stmt, 2);
      obj["daily_limit_ml"] = sqlite3_column_int(stmt, 3);
      obj["active"] = sqlite3_column_int(stmt, 4);
    }
    sqlite3_finalize(stmt);
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleApiValvesPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  
  const char* sql = "UPDATE valves SET plant_name=?, max_duration_sec=?, daily_limit_ml=?, active=? WHERE id=?;";
  sqlite3_stmt* stmt;
  
  sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, doc["plant_name"] | "—", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, doc["active"] | 0);
  sqlite3_bind_int(stmt, 5, doc["id"] | 1);
  
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiJournalGet() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  
  const char* sql = "SELECT ts, valve_id, type, duration_sec, volume_ml, status FROM journal ORDER BY id DESC;";
  sqlite3_stmt* stmt;
  
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      JsonObject obj = arr.add<JsonObject>();
      obj["ts"] = (const char*)sqlite3_column_text(stmt, 0);
      obj["valve_id"] = sqlite3_column_int(stmt, 1);
      obj["type"] = (const char*)sqlite3_column_text(stmt, 2);
      obj["duration_sec"] = sqlite3_column_int(stmt, 3);
      obj["volume_ml"] = sqlite3_column_double(stmt, 4);
      obj["status"] = (const char*)sqlite3_column_text(stmt, 5);
    }
    sqlite3_finalize(stmt);
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleApiJournalPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  
  const char* sql = "INSERT INTO journal (ts, valve_id, type, duration_sec, volume_ml, status) VALUES (?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* stmt;
  
  sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, doc["ts"] | "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, doc["valve_id"] | 0);
  sqlite3_bind_text(stmt, 3, doc["type"] | "manual", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, doc["duration_sec"] | 0);
  sqlite3_bind_double(stmt, 5, doc["volume_ml"] | 0.0);
  sqlite3_bind_text(stmt, 6, doc["status"] | "completed", -1, SQLITE_TRANSIENT);
  
  if (sqlite3_step(stmt) == SQLITE_DONE) {
    long long newId = sqlite3_last_insert_rowid(db);
    
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════╗");
    Serial.println("║         НОВАЯ ЗАПИСЬ ДОБАВЛЕНА В БД              ║");
    Serial.println("╠══════════════════════════════════════════════════╣");
    Serial.printf("║ ID записи : %-42lld ║\n", newId);
    Serial.printf("║ Время     : %-42s ║\n", (const char*)(doc["ts"] | "N/A"));
    Serial.printf("║ Клапан    : %-42d ║\n", doc["valve_id"] | 0);
    Serial.printf("║ Тип       : %-42s ║\n", (const char*)(doc["type"] | "N/A"));
    Serial.printf("║ Длительность: %-39d сек ║\n", doc["duration_sec"] | 0);
    Serial.printf("║ Объём     : %-39.2f мл  ║\n", (double)(doc["volume_ml"] | 0.0));
    Serial.printf("║ Статус    : %-42s ║\n", (const char*)(doc["status"] | "N/A"));
    Serial.println("╚══════════════════════════════════════════════════╝");
    Serial.println();
  }
  
  sqlite3_finalize(stmt);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiJournalDelete() {
  int valveId = server.arg("valve_id").toInt();
  int all = server.arg("all").toInt();
  
  char sql[256];
  if (all == 1) {
    strcpy(sql, "DELETE FROM journal;");
  } else {
    snprintf(sql, sizeof(sql), "DELETE FROM journal WHERE valve_id = %d;", valveId);
  }
  
  char* errMsg = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &errMsg) == SQLITE_OK) {
    Serial.printf("[SQLite] Журнал очищен (valve_id=%d, all=%d)\n", valveId, all);
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    Serial.printf("[SQLite] Ошибка очистки: %s\n", errMsg);
    sqlite3_free(errMsg);
    server.send(500, "application/json", "{\"ok\":false}");
  }
}

void handleValve() {
  if (!server.hasArg("id") || !server.hasArg("state")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing args\"}");
    return;
  }
  int id = server.arg("id").toInt();
  int st = server.arg("state").toInt();

  if (id < 0 || id >= NUM_VALVES) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad id\"}");
    return;
  }

  if (st) {
    flowmetr = 0;
    mosfet.digitalWrite(id, HIGH);
    valveStates[id] = true;
    Serial.printf("Клапан %d -> OPEN (расходометр обнулён)\n", id);
  } else {
    mosfet.digitalWrite(id, LOW);
    valveStates[id] = false;
    Serial.printf("Клапан %d -> CLOSE\n", id);
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAll() {
  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing state\"}");
    return;
  }
  int st = server.arg("state").toInt();
  for (int i = 0; i < NUM_VALVES; i++) {
    mosfet.digitalWrite(i, st ? HIGH : LOW);
    valveStates[i] = st ? true : false;
  }
  Serial.printf("Все клапаны -> %s\n", st ? "OPEN" : "CLOSE");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStates() {
  String json = "{\"states\":[";
  for (int i = 0; i < NUM_VALVES; i++) {
    json += valveStates[i] ? "true" : "false";
    if (i < NUM_VALVES - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleFlowmeter() {
  noInterrupts();
  uint32_t pulses = flowmetr;
  interrupts();

  float volumeMl = pulses * FLOW_RATE_ML;

  String json = "{\"ok\":true,\"pulses\":" + String(pulses) +
                ",\"volume_ml\":" + String(volumeMl, 2) + "}";

  Serial.printf("[Flowmeter] Отправлено: %u импульсов, %.2f мл\n", pulses, volumeMl);
  server.send(200, "application/json", json);
}

void ISR_Flow() {
  flowmetr++;
  Serial.printf("\n %d\n", flowmetr);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  mosfet.begin();
  mosfet.digitalWrite(ALL, LOW);

  for (int i = 0; i < NUM_VALVES; i++) {
    mosfet.digitalWrite(i, HIGH);
    delay(300);
    mosfet.digitalWrite(i, LOW);
  }

  if (!SD.begin(PIN_CS_SD)) {
    Serial.printf("\nFlash-память не обнаружена\n");
  } else {
    uint8_t cardType = SD.cardType();
    if (cardType != CARD_NONE) {
      uint64_t cardSize = SD.cardSize() / (1024 * 1024);
      Serial.printf("SD: %llu MB\n", cardSize);
    }
  }

  initDatabase();

  pinMode(0, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(0), ISR_Flow, RISING);

  Serial.printf("\nТочка доступа Wifi:\n");
  WiFi.AP.begin();
  WiFi.AP.config(ap_ip, ap_ip, ap_subnet, ap_leaseStart, ap_dns);
  WiFi.AP.create(AP_SSID, AP_PASS);
  if (!WiFi.AP.waitStatusBits(ESP_NETIF_STARTED_BIT, 1000)) {
    Serial.printf("\tнедоступна\n");
    return;
  }
  Serial.println(WiFi.AP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/valves", HTTP_GET, handleApiValvesGet);
  server.on("/api/valves", HTTP_POST, handleApiValvesPost);
  server.on("/api/journal", HTTP_GET, handleApiJournalGet);
  server.on("/api/journal", HTTP_POST, handleApiJournalPost);
  server.on("/api/journal", HTTP_DELETE, handleApiJournalDelete);
  server.on("/valve", HTTP_GET, handleValve);
  server.on("/all", HTTP_GET, handleAll);
  server.on("/states", HTTP_GET, handleStates);
  server.on("/flowmeter", HTTP_GET, handleFlowmeter);
  server.begin();
  Serial.println("HTTP-сервер запущен: http://192.168.5.1");
}

void loop() {
  server.handleClient();
  delay(2);
}
