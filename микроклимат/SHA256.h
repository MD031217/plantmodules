// ==================== SHA256.h ====================
// Простая и надёжная обёртка над mbedtls для ESP32

#ifndef SHA256_H
#define SHA256_H

#include "mbedtls/sha256.h"
#include <Arduino.h>

inline String hashPassword(const String& password) {
    if (password.length() == 0) return "";

    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);           // 0 = SHA-256
    mbedtls_sha256_update(&ctx, (const unsigned char*)password.c_str(), password.length());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    // Конвертируем в HEX строку
    String hashStr = "";
    for (int i = 0; i < 32; i++) {
        if (hash[i] < 0x10) hashStr += "0";
        hashStr += String(hash[i], HEX);
    }
    
    return hashStr;
}

#endif