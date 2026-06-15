// ================================================
//  СКЕТЧ ДЛЯ ПОЛНОЙ ОЧИСТКИ SD-КАРТЫ
//  ESP32-C3 (или любой ESP32)
// ================================================

#include <SPI.h>
#include <SD.h>

#define CS_SD_PIN  SS   // Обычно пин 4 или 5, у тебя был SS

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=========================================");
  Serial.println("   СКРИПТ ОЧИСТКИ SD-КАРТЫ");
  Serial.println("=========================================\n");
  
  // Инициализация SD-карты
  if (!SD.begin(CS_SD_PIN)) {
    Serial.println("❌ Ошибка: SD-карта не обнаружена!");
    Serial.println("Проверьте подключение и пин CS.");
    while (true) delay(1000);
  }
  
  Serial.println("✅ SD-карта успешно инициализирована");
  Serial.println("Начинаем полную очистку...\n");
  
  int deleted = deleteAllFiles("/");
  
  Serial.println("\n=========================================");
  Serial.printf("✅ ОЧИСТКА ЗАВЕРШЕНА!\n");
  Serial.printf("Удалено объектов: %d\n", deleted);
  Serial.println("=========================================");
  
  Serial.println("\nТеперь можно безопасно извлечь SD-карту.");
}

void loop() {
  // Ничего не делаем
  delay(1000);
}

// ====================== РЕКУРСИВНОЕ УДАЛЕНИЕ ======================
int deleteAllFiles(const char* path) {
  File dir = SD.open(path);
  if (!dir) {
    Serial.printf("Не удалось открыть: %s\n", path);
    return 0;
  }

  int count = 0;

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;

    String entryName = entry.name();
    String fullPath = String(path) + (path[strlen(path)-1] == '/' ? "" : "/") + entryName;

    if (entry.isDirectory()) {
      Serial.printf("📁 Удаляем папку: %s\n", fullPath.c_str());
      count += deleteAllFiles(fullPath.c_str());
      SD.rmdir(fullPath.c_str());
      count++;
    } else {
      Serial.printf("🗑️  Удаляем файл: %s\n", fullPath.c_str());
      SD.remove(fullPath.c_str());
      count++;
    }
    
    entry.close();
  }
  
  dir.close();
  return count;
}
