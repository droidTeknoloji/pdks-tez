// ConfigSchema.h
#pragma once
#include <Arduino.h>

// ----------------------------------------------------
//  GLOBAL DEVICE ID - tek merkez
// ----------------------------------------------------
static const char* DEVICE_ID = "PDKS_03";

struct OfflineIndex;

inline uint16_t getSerialFromDeviceNo(const String& devNo);

// Disable sifresi: 010203 + DEVICE_ID son 4 hane
inline String getDisablePassword() {
    uint16_t serial = getSerialFromDeviceNo(String(DEVICE_ID));
    char buf[5];
    snprintf(buf, sizeof(buf), "%04u", (unsigned)serial);
    return String("010203") + buf;
}

// Ag ayarlari
struct NetConfig {
    bool   wifi_enabled;     // WiFi kullanilacak mi
    String wifi_ssid;
    String wifi_pass;

    bool   eth_enabled;      // ENC28J60 aktif mi

    String ip;
    String gateway;
    String subnet;
    String dns;
};

// PDKS sunucu ayarlari
struct PdksConfig {
    String   server_host;    // "192.168.1.50" veya "pdks.example.com"
    uint16_t server_port;    // Ornek 7777
};

// Cihaz / isyeri ayarlari
struct DevConfig {
    String   device_no = DEVICE_ID;
    uint32_t company_id;
    uint8_t  direction;      // 0=giris, 1=cikis
};

// Ana config
struct Config {
    NetConfig  net;
    PdksConfig pdks;
    DevConfig  dev;
};

// ----------------------------------------------------
//  WIFI/ETH MAC & RESET YARDIMCILARI
// ----------------------------------------------------

// device_no icindeki son 4 rakamdan seri numarasi uret
inline uint16_t getSerialFromDeviceNo(const String& devNo) {
    int len = devNo.length();
    if (len == 0) return 0;

    String digits;
    for (int i = len - 1; i >= 0; --i) {
        char c = devNo[i];
        if (c >= '0' && c <= '9') {
            digits = String(c) + digits;
            if (digits.length() >= 4) break;
        } else if (digits.length() > 0) {
            break;
        }
    }
    if (digits.length() == 0) return 0;
    return (uint16_t)digits.toInt();
}

// DEVICE_ID icindeki son 4 rakamdan seri numarasi uret
inline uint16_t getSerialFromDeviceId() {
    String idStr(DEVICE_ID);
    return getSerialFromDeviceNo(idStr);
}

// WiFi icin deterministik MAC uret
// 02:AA:BB:CA:hi(serial):lo(serial)
inline void buildWifiMac(const Config& cfg, uint8_t outMac[6]) {
    (void)cfg;
    uint16_t serial = getSerialFromDeviceId();

    outMac[0] = 0x02;
    outMac[1] = 0xAA;
    outMac[2] = 0xBB;
    outMac[3] = 0xCA;
    outMac[4] = (serial >> 8) & 0xFF;
    outMac[5] = serial & 0xFF;
}

// ENC28J60 icin deterministik MAC uret
// 02:AA:BB:CB:hi(serial):lo(serial)
inline void buildEthMac(const Config& cfg, uint8_t outMac[6]) {
    (void)cfg;
    uint16_t serial = getSerialFromDeviceId();

    outMac[0] = 0x02;
    outMac[1] = 0xAA;
    outMac[2] = 0xBB;
    outMac[3] = 0xCB;
    outMac[4] = (serial >> 8) & 0xFF;
    outMac[5] = serial & 0xFF;
}

// ENC28J60 donanim pin sabitleri (ESP8266 uzerinde)
static const uint8_t ENC_CS_PIN  = 15;  // D8
static const uint8_t ENC_RST_PIN = 16;  // D0

// Reset pinini hazirla (setup'ta 1 defa cagir)
inline void encResetPinInit() {
    pinMode(ENC_RST_PIN, OUTPUT);
    digitalWrite(ENC_RST_PIN, HIGH);
}

// Donanimsal reset
inline void encHardwareReset() {
    digitalWrite(ENC_RST_PIN, LOW);
    delay(10);
    digitalWrite(ENC_RST_PIN, HIGH);
    delay(100);
}

class ConfigStore {
public:
    ConfigStore() {}

    void begin();
    void load(Config &cfg);
    bool save(const Config &cfg);

private:
    void applyDefaults(Config &cfg);
};
