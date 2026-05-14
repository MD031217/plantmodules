#include "ClientNTP.h"

//Инициализация данных и объектов для синхронизации часов ESP8266
IPAddress timeServer(88,147,254,230); //NTP-сервер для синхронизации контроллера
const int NTP_PACKET_SIZE = 48;       //Длина (в байтах) NTP-данных
byte packetBuffer[NTP_PACKET_SIZE];   //Буфер отправляемых и принимаемых NTP-данных
WiFiUDP udp;                          //Объект для работы с UDP-сокетом

//Процедура отправки NTP-запроса
void sendNTPpacket(IPAddress& address) {
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;   
  packetBuffer[1] = 0;     
  packetBuffer[2] = 6;     
  packetBuffer[3] = 0xEC;  
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;
  udp.beginPacket(address, 123); 
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}

//Процедура получения NTP-ответа
time_t getNtpTime()
{
  while (udp.parsePacket() > 0) ;                               //отбраковывать все пакеты, полученные ранее 
  sendNTPpacket(timeServer);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      udp.read(packetBuffer, NTP_PACKET_SIZE);                  //чтение пакета в буфер 
      unsigned long secsSince1900;
      // конвертируем 4 байта (начиная с позиции 40) в длинное целое число: 
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + 5 * SECS_PER_HOUR;
    }
  }
  return 0;                                                     //вернуть 0,если синхронизации неудачна
}

//Процедура форматирования вывода значений даты и времени
String timeDigits(int digits){
  String td="";
  if (digits<10) td+="0";
  td+=String(digits);
  return td;
}

//Процедура синхронизация часов контроллера по протоколу NTP
void InitNTP(){
  udp.begin(2390);                  //Настройка сокета-источника для обмена сообщениями по протоколу NTP через порт 2390
  setSyncProvider(getNtpTime);      //Назначение callback-функции синхронизации часов контроллера по протоколу UDP
  setSyncInterval(300);             //Установка интервала NTP-запросов
}
