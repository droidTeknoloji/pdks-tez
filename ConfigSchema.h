// ConfigSchema.h
#pragma once
#include <Arduino.h>

// ----------------------------------------------------
//  GLOBAL DEVICE ID — tek merkez
// ----------------------------------------------------
static const char* DEVICE_ID = "PDKS_02";

// Disable şifresi: 010203 + DEVICE_ID son 4 hane
inline String getDisablePassword() {
    String idStr(DEVICE_ID);
    if (idStr.length() < 2) return "0102030000";           // 4 olacak
    String numPart = idStr.substring(idStr.length() - 2);  // 4 olacak
    return "010203" + numPart;  // 010203XXXX
}

// Ağ ayarları
struct NetConfig {
    bool   wifi_enabled;     // WiFi kullanılacak mı
    String wifi_ssid;
    String wifi_pass;

    bool   eth_enabled;      // ENC28J60 aktif mi

    String ip;
    String gateway;
    String subnet;
    String dns;
};

// PDKS sunucu ayarları
struct PdksConfig {
    String   server_host;    // "192.168.1.50" veya "pdks.example.com"
    uint16_t server_port;    // Örn 7777
};

// Cihaz / işyeri ayarları
struct DevConfig {
    String   device_no = DEVICE_ID;      // Eski kssid (device_no)
    uint32_t company_id;                 // Sirket id
    uint8_t  direction;                  // 0=giriş, 1=çıkış
};

// Ana config
struct Config {
    NetConfig  net;
    PdksConfig pdks;
    DevConfig  dev;
};

// ----------------------------------------------------
//  ETHERNET MAC & RESET YARDIMCILARI
//  (Bu projede ekstra .h istemediğin için buraya gömdük)
// ----------------------------------------------------

// device_no içindeki son 4 rakamdan seri numarası üret
inline uint16_t getSerialFromDeviceNo(const String& devNo) {
    int len = devNo.length();
    if (len == 0) return 0;

    // Sondan başlayarak en fazla 4 rakam topla
    String digits;
    for (int i = len - 1; i >= 0; --i) {
        char c = devNo[i];
        if (c >= '0' && c <= '9') {
            digits = String(c) + digits;
            if (digits.length() >= 4) break;
        } else if (digits.length() > 0) {
            // Rakam dizisi bittiyse dur
            break;
        }
    }
    if (digits.length() == 0) return 0;
    return (uint16_t)digits.toInt();
}

// ENC28J60 için deterministik MAC üret
// Eskide yaptığımız gibi: 02:AA:BB:CB:hi(serial):lo(serial)
inline void buildEthMac(const Config& cfg, uint8_t outMac[6]) {
    uint16_t serial = getSerialFromDeviceNo(cfg.dev.device_no);

    outMac[0] = 0x02;
    outMac[1] = 0xAA;
    outMac[2] = 0xBB;
    outMac[3] = 0xCB;
    outMac[4] = (serial >> 8) & 0xFF;
    outMac[5] = serial & 0xFF;
}

// ENC28J60 donanım pin sabitleri (ESP8266 üzerinde)
static const uint8_t ENC_CS_PIN  = 15;  // D8
static const uint8_t ENC_RST_PIN = 16;  // D0, reset için kullanacağız

// Reset pinini hazırla (setup'ta 1 defa çağır)
inline void encResetPinInit() {
    pinMode(ENC_RST_PIN, OUTPUT);
    digitalWrite(ENC_RST_PIN, HIGH); // Normalde HIGH = ENC çalışır
}

// Donanımsal reset (ESP32'deki mantığın aynısı)
inline void encHardwareReset() {
    digitalWrite(ENC_RST_PIN, LOW);
    delay(10);
    digitalWrite(ENC_RST_PIN, HIGH);
    delay(100); // toparlanma
}
class ConfigStore {
public:
    ConfigStore() {}

    // LittleFS hazır olduktan sonra 1 kez çağır
    void begin();

    // config.json varsa onu yükler, yoksa default üretir
    void load(Config &cfg);

    // cfg'yi json olarak kaydeder
    bool save(const Config &cfg);

private:
    void applyDefaults(Config &cfg);   // ilk açılıştaki varsayılanlar
};
