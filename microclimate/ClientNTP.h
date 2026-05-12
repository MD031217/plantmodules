#ifndef CLIENT_NTP_H_
#define CLIENT_NTP_H_

#include <Arduino.h>
#include <WiFiUdp.h>
#include "TimeLib.h"

void sendNTPpacket(IPAddress&);
time_t getNtpTime();
String timeDigits(int);
void InitNTP();

#endif  // CLIENT_NTP_H_
