#include "ConfigSchema.h"
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ENC28J60lwIP.h>      // ETH için
#include <ESP8266WiFi.h>       // wl_status_t vs için

static const char* CONFIG_FILE = "/config.json";

// ----------------------------------------------------
//  ConfigStore
// ----------------------------------------------------

void ConfigStore::begin() {
    // LittleFS.begin() zaten setup'ta çağrılıyorsa burada şart değil,
    // ama defansif olsun:
    if (!LittleFS.begin()) {
        Serial.println("[CFG] LittleFS.begin() FAIL (ConfigStore)");
    }
}

void ConfigStore::applyDefaults(Config &cfg) {
    // Ağ varsayılanları
    cfg.net.wifi_enabled  = false;
    cfg.net.wifi_ssid     = "GATE_WIFI";     // boş -> önceki WiFi kredilerini kullanabilirsin
    cfg.net.wifi_pass     = "selaswrls09";

    cfg.net.eth_enabled   = true;   // ENC28J60 da aktif
    cfg.net.ip            = "";
    cfg.net.gateway       = "";
    cfg.net.subnet        = "";
    cfg.net.dns           = "";

    // PDKS varsayılan
    cfg.pdks.server_host = "185.134.185.8";
    cfg.pdks.server_port = 1344;

    // Cihaz / şirket varsayılan
    cfg.dev.device_no  = DEVICE_ID;  // tek merkezden
    cfg.dev.company_id = 1;
    cfg.dev.direction  = 0;  // giriş
}

void ConfigStore::load(Config &cfg) {
    if (!LittleFS.begin()) {
        Serial.println("[CFG] LittleFS.begin() FAIL (load)");
        applyDefaults(cfg);
        return;
    }

    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("[CFG] config.json yok, defaults uygulanıyor");
        applyDefaults(cfg);
        save(cfg);   // ilk defa oluştur
        return;
    }

    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        Serial.println("[CFG] config.json açılamadı, defaults");
        applyDefaults(cfg);
        return;
    }

    // Dosya boyutuna göre makul bir JSON buffer
    size_t size = f.size();
    if (size == 0 || size > 4096) {
        Serial.println("[CFG] config.json boyutu hatalı, defaults");
        f.close();
        applyDefaults(cfg);
        return;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.print("[CFG] JSON parse FAIL: ");
        Serial.println(err.c_str());
        applyDefaults(cfg);
        return;
    }

    // --- Net ---
    JsonObject jnet = doc["net"] | JsonObject();

    cfg.net.wifi_enabled = jnet["wifi_enabled"] | true;
    cfg.net.wifi_ssid    = (const char*)(jnet["wifi_ssid"] | "");
    cfg.net.wifi_pass    = (const char*)(jnet["wifi_pass"] | "");

    cfg.net.eth_enabled  = jnet["eth_enabled"]  | true;

    cfg.net.ip            = (const char*)(jnet["ip"]       | "");
    cfg.net.gateway       = (const char*)(jnet["gateway"]  | "");
    cfg.net.subnet        = (const char*)(jnet["subnet"]   | "");
    cfg.net.dns           = (const char*)(jnet["dns"]      | "");

    // --- Pdks ---
    JsonObject jpdks = doc["pdks"] | JsonObject();

    cfg.pdks.server_host = (const char*)(jpdks["server_host"] | "185.134.185.8");
    cfg.pdks.server_port = jpdks["server_port"] | 1344;

    // --- Dev ---
    JsonObject jdev = doc["dev"] | JsonObject();

    cfg.dev.device_no  = (const char*)(jdev["device_no"]  | DEVICE_ID);
    cfg.dev.company_id = jdev["company_id"] | 1;
    cfg.dev.direction = jdev["direction"] | 0;
    //cfg.dev.direction = 1 - (jdev["direction"] | 0);

    Serial.println("[CFG] config.json yüklendi");
}

bool ConfigStore::save(const Config &cfg) {
    if (!LittleFS.begin()) {
        Serial.println("[CFG] LittleFS.begin() FAIL (save)");
        return false;
    }

    DynamicJsonDocument doc(4096);

    // --- Net ---
    JsonObject jnet = doc.createNestedObject("net");
    jnet["wifi_enabled"]  = cfg.net.wifi_enabled;
    jnet["wifi_ssid"]     = cfg.net.wifi_ssid;
    jnet["wifi_pass"]     = cfg.net.wifi_pass;
    jnet["eth_enabled"]   = cfg.net.eth_enabled;
    jnet["ip"]            = cfg.net.ip;
    jnet["gateway"]       = cfg.net.gateway;
    jnet["subnet"]        = cfg.net.subnet;
    jnet["dns"]           = cfg.net.dns;

    // --- Pdks ---
    JsonObject jpdks = doc.createNestedObject("pdks");
    jpdks["server_host"] = cfg.pdks.server_host;
    jpdks["server_port"] = cfg.pdks.server_port;

    // --- Dev ---
    JsonObject jdev = doc.createNestedObject("dev");
    jdev["device_no"]   = cfg.dev.device_no;
    jdev["company_id"]  = cfg.dev.company_id;
    jdev["direction"]   = cfg.dev.direction;

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        Serial.println("[CFG] config.json yazılamadı");
        return false;
    }

    if (serializeJson(doc, f) == 0) {
        Serial.println("[CFG] serializeJson FAIL");
        f.close();
        return false;
    }
    f.close();
    Serial.println("[CFG] config.json kaydedildi");
    return true;
}

// ----------------------------------------------------
//  ETHERNET (ENC28J60) — eski .ino'dan uyarlanmış hali
//  NOT: MAC üretimi ve reset helper'lar ConfigSchema.h içinde:
//   - buildEthMac(const Config&, uint8_t mac[6])
//   - ENC_CS_PIN, ENC_RST_PIN
//   - encResetPinInit(), encHardwareReset()
// ----------------------------------------------------

// Global eth objesi
ENC28J60lwIP eth(ENC_CS_PIN);

// ESP32'deki mantığa benzer, sadece lwIP/ESP8266 API ile
bool ethInitFromConfig(const Config& cfg) {
    if (!cfg.net.eth_enabled) {
        Serial.println("[ETH] Disabled in config");
        return false;
    }

    if (cfg.net.ip.length()) {
        IPAddress ip;
        if (ip.fromString(cfg.net.ip)) {
            IPAddress gateway;
            IPAddress subnet;
            IPAddress dns;

            if (!cfg.net.gateway.length() || !gateway.fromString(cfg.net.gateway)) {
                gateway = IPAddress(ip[0], ip[1], ip[2], 1);
            }
            if (!cfg.net.subnet.length() || !subnet.fromString(cfg.net.subnet)) {
                subnet = IPAddress(255, 255, 255, 0);
            }
            if (!cfg.net.dns.length() || !dns.fromString(cfg.net.dns)) {
                dns = gateway;
            }

            eth.config(ip, gateway, subnet, dns);
        }
    }

    // MAC üret
    uint8_t mac[6];
    buildEthMac(cfg, mac);

    Serial.printf("[ETH] MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    encHardwareReset(); // init öncesi reset (GPIO16 üzerinden)

    // Varsayılan route olarak ethernet'i kullan
    eth.setDefault();

    int present = eth.begin(mac);
    if (!present) {
        Serial.println("[ETH] no ethernet hardware present");
        return false;
    }

    // IP alma / link up bekleme
    uint32_t t0 = millis();
    const uint32_t timeoutMs = 10UL * 1000UL; // 10 sn

    Serial.println("[ETH] Waiting for link...");
    while (millis() - t0 < timeoutMs) {
        wl_status_t st = eth.status();
        Serial.printf("[ETH] status=%d\n", (int)st);
        if (st == WL_CONNECTED) break;
        delay(500);
    }

    if (eth.status() != WL_CONNECTED) {
        Serial.println("[ETH] Ethernet link DOWN");
        return false;
    }

    Serial.print("[ETH] IP=");
    Serial.println(eth.localIP());
    return true;
}

