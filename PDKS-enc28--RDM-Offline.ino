#include <SPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ENC28J60lwIP.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <stdlib.h>

#include "ConfigSchema.h"
#include "Web.h"        // ESP32’deki gibi: /config UI + AP portal burada

#include <rdm6300.h>
#define kapi     D2
#define mavi     D4
#define yesil    D3
#define kirmizi  D1
// ----------------- Donanım Objeleri -----------------
Rdm6300 rdm6300;

ESP8266WebServer      server(80);
ESP8266HTTPUpdateServer httpUpdater;



// Eski projeden kalma MAC; istersen Config’e de koyabiliriz
byte mac_hw[] = {0x02, 0xAA, 0xBB, 0xCC, 0x00, 0x03};

// ----------------- Global Konfig / Store -----------------
::Config      cfg;
ConfigStore store;

extern ENC28J60lwIP eth;
extern bool ethInitFromConfig(const ::Config& cfg);

extern bool startWiFiFromConfig(const NetConfig &net);

// ---- Forward declarations (merged from .h/.cpp) ----
struct OfflineRecord {
  uint32_t ts;
  uint32_t cardId;
  uint8_t  punchType;
  uint8_t  reserved[3];
};

void timeServiceInit();
void offlineUpdateTimeFromServerEpoch(uint32_t serverEpoch);
uint32_t nowTs();
bool timeIsSynced();
bool syncTimeFromHttp(const String& host, const String& path = "/");

void offlineInit();
bool offlineEnqueue(uint32_t ts, uint32_t cardId, uint8_t punchType);
bool offlineHasPending();
bool offlinePeek(OfflineRecord &rec);
bool offlinePop();
uint16_t offlinePendingCount();
void offlineClear();

bool sendPunchTCP(const Config &cfg, const String &cardHex, uint8_t punchType, uint32_t ts);
void punchNowOrQueue(const Config &cfg, const String &cardHex, uint8_t punchType);
void flushOfflinePunches(const Config &cfg);

void initRDM6300();
void rdmLoop();
bool rdmCardAvailable();
String rdmGetCard();

bool wifiInitFromConfig(const ::Config& cfg) {
    return startWiFiFromConfig(cfg.net);
}

void netInit(const ::Config& cfg) {
    bool wifiOk = false;
    bool ethOk  = false;

    if (cfg.net.wifi_enabled) {
        wifiOk = wifiInitFromConfig(cfg);
    }

    // ETH reset pini (ConfigSchema.h içindeki helper)
    encResetPinInit();

    if (cfg.net.eth_enabled) {
        ethOk = ethInitFromConfig(cfg);
    }

    if (!wifiOk && !ethOk) {
        Serial.println("[NET] No network up (WiFi+ETH both failed)");
    }
}

bool netIsOnline() {
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    bool ethUp  = (eth.status()   == WL_CONNECTED);
    return wifiUp || ethUp;
}

// ---- Net state: gönderme izni ----
static bool s_canSendNow       = false;    // Sağlık kontrolü bunu yönetir
static uint32_t s_lastHealthMs = 0;
static uint32_t s_lastNetOkMs  = 0;

static const uint32_t NET_HEALTH_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 dk

bool netCanSendNow() {
  return s_canSendNow;
}


void netHealthLoop(const ::Config& cfg) {
  uint32_t now = millis();

  // İlk çalışmada hemen kontrol yapsın
  if (s_lastHealthMs == 0) {
    s_lastHealthMs = now;
  } else {
    // 10 dk dolmadıysa sağlık kontrolü yapma
    if (now - s_lastHealthMs < NET_HEALTH_INTERVAL_MS) {
      return;
    }
    s_lastHealthMs = now;
  }

  bool online = netIsOnline();

  if (online) {
    // Online ise: gönderme serbest
    if (!s_canSendNow) {
  flushOfflinePunches(cfg);
      Serial.println("[NET] Health: online, sending ENABLED");
    }
    s_canSendNow = true;
    s_lastNetOkMs = now;

    // Kuyrukta veri varsa, SADECE sağlık kontrolü flush denesin
    Serial.println("[NET] Health: trying to flush offline queue...");
    flushOfflinePunches(cfg);     // kendi fonksiyon adını kullan
  } else {
    // Offline ise: gönderme yasak
    if (s_canSendNow) {
      Serial.println("[NET] Health: offline, sending DISABLED");
    }
    s_canSendNow = false;

    // Offline süresi 10 dk'yı geçtiyse ENC reset + re-init
    if (s_lastNetOkMs != 0 && (now - s_lastNetOkMs) >= NET_HEALTH_INTERVAL_MS) {
      Serial.println("[NET] Health: offline >= 10 min, resetting ENC...");
      if (cfg.net.eth_enabled) {
        encHardwareReset();
        ethInitFromConfig(cfg);
      }
      // tekrar ne olacağını sonraki health turunda göreceğiz
    } else {
      uint32_t secSinceOk = (s_lastNetOkMs == 0) ? 0 : ((now - s_lastNetOkMs) / 1000UL);
      Serial.printf("[NET] Health: offline, last OK %u saniye önce\n", secSinceOk);
    }
  }
}

// ----------------- Zaman Senkronizasyonu -----------------
unsigned long lastTimeSyncMs = 0;
const unsigned long TIME_SYNC_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 dk

// ----------------- Zamanlayıcılar ve flag'ler -----------------
unsigned long INTERVAL0  = 1000,  timer0;   bool timedOut0  = true;
unsigned long INTERVAL3  = 10000, timer3;   bool timedOut3  = true;
unsigned long INTERVAL5  = 3000,  timer5;   bool timedOut5  = false;
bool          oku         = true;
unsigned long INTERVAL12 = 1000,  timer12;  bool timedOut12 = true;
unsigned long INTERVAL   = 1000,  timer1;   bool timedOut1  = true;
unsigned long INTERVAL10 = 300,   timer10;  bool timedOut10 = true;
unsigned long previousMillis = 0;
unsigned long interval       = 60000;

bool rst      = false;
bool sistem   = true;
bool kablosuz = false;

int buttonState, rv, kd = 0;

unsigned int replay = 0;   // şimdilik tutuluyor ama kullanılmıyor

String kart  = "000000";
String okart = "000000";

String trimRdm(const String &s) {
    if (s.length() <= 2) return s;
    return s.substring(2);
}

char macs[32] = "";   // MAC string'i (Web UI’de göstermek istersen)

// Kart okuma buffer’ları
String incomingByte = "";
String kartveri = "";
String patch = "";
String mychar = "";
unsigned long ms;

String content, st, bigStr, obigStr, esid, epass, sinyal;



// ----------------- Ağ Başlatma Yardımcıları -----------------
bool startWiFiFromConfig(const NetConfig &net) {
  if (!net.wifi_enabled) {
    Serial.println("[NET] WiFi disabled in config");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  if (net.ip.length()) {
    IPAddress ip;
    if (ip.fromString(net.ip)) {
      IPAddress gateway;
      IPAddress subnet;
      IPAddress dns;

      if (!net.gateway.length() || !gateway.fromString(net.gateway)) {
        gateway = IPAddress(ip[0], ip[1], ip[2], 1);
      }
      if (!net.subnet.length() || !subnet.fromString(net.subnet)) {
        subnet = IPAddress(255, 255, 255, 0);
      }
      if (!net.dns.length() || !dns.fromString(net.dns)) {
        dns = gateway;
      }

      WiFi.config(ip, gateway, subnet, dns);
    }
  }

  if (net.wifi_ssid.length()) {
    Serial.printf("[NET] WiFi STA: SSID='%s'\n", net.wifi_ssid.c_str());
    WiFi.begin(net.wifi_ssid.c_str(), net.wifi_pass.c_str());
  } else {
    Serial.println("[NET] WiFi STA: Boş SSID, eski krediler deneniyor");
    WiFi.begin();   // eski kaydedilmiş AP bilgilerini kullan
  }

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[NET] WiFi connected, IP=%s\n",
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("[NET] WiFi connect FAIL");
  return false;
}

bool startEthernetFromConfig(const NetConfig &net) {
  if (!net.eth_enabled) {
    Serial.println("[NET] ETH disabled in config");
    return false;
  }

  Serial.println("[NET] ETH enabled but no implementation yet");
  return false;   // Fonksiyon stub olduğu için default değer döndürüyoruz
}



// Bu fonksiyon şu an kullanılmıyor; istersen utility olarak dursun
void array_to_string(byte array[], unsigned int len, char buffer[]) {
  for (unsigned int i = 0; i < len; i++) {
    byte nib1 = (array[i] >> 4) & 0x0F;
    byte nib2 = (array[i] >> 0) & 0x0F;
    buffer[i * 2 + 0] = nib1 < 0xA ? '0' + nib1 : 'A' + nib1 - 0xA;
    buffer[i * 2 + 1] = nib2 < 0xA ? '0' + nib2 : 'A' + nib2 - 0xA;
  }
  buffer[len * 2] = '\0';
}

void createWebServer(int webtype){
  auto setupDoorEndpoints = [](){
      server.on("/G1", HTTP_GET, [&]() {
    Serial.println("KAPI ACIK (manuel)");
    digitalWrite(kapi, HIGH);
    // LED'ler sende nasıl ise öyle bırakabilirsin
    server.send(200, "application/json", "true");
  });

  server.on("/G0", HTTP_GET, [&]() {
    Serial.println("KAPI KAPALI");
    digitalWrite(mavi, HIGH);
    digitalWrite(yesil, LOW);
    digitalWrite(kirmizi, LOW);
    digitalWrite(kapi, HIGH);
    delay(200);
    digitalWrite(mavi, LOW);
    server.send(200, "application/json", "false");
  });

  server.on("/G2", HTTP_GET, [&]() {
    Serial.println("KAPI SURELI ACIK");
    timedOut1 = false;
    timer1    = millis();
    digitalWrite(kapi, HIGH);
    digitalWrite(kirmizi, LOW);
    digitalWrite(yesil, HIGH);
    digitalWrite(mavi, HIGH);
    delay(200);
    digitalWrite(mavi, LOW);
    server.send(200, "application/json", "true");
  });

  // 404
  server.onNotFound([&]() {
    String js = "{\"ok\":false,\"error\":\"not_found\"}";
    server.send(404, "application/json", js);
  });
  };

 
}

// Basit helper
static void setLED(bool m, bool y, bool k) {
  digitalWrite(mavi,    m);
  digitalWrite(yesil,   y);
  digitalWrite(kirmizi, k);
}

// ---- LED state machine ----
enum LedPhase {
  LED_PHASE_BOOT_SEQ = 0,     // Mavi → Yeşil → Kırmızı (1 sn arayla)
  LED_PHASE_NET_OK_BLINK,     // 7x kısa kırmızı blink
  LED_PHASE_NET_FAIL_BLINK,   // 3x uzun kırmızı blink
  LED_PHASE_IDLE,             // direction'a göre sabit LED
  LED_PHASE_DOOR              // Kapı açma + kırmızı blink
};

static LedPhase  s_ledPhase            = LED_PHASE_BOOT_SEQ;
static bool      s_ledNetUpOnBoot      = false;   // netInit sonucu
static uint32_t  s_ledLastMs           = 0;
static uint8_t   s_ledStep             = 0;
static bool      s_ledOn               = false;

// Kapı (door) için ayrı süre
static uint32_t  s_doorEndMs           = 0;
static uint32_t  s_doorBlinkLastMs     = 0;
static uint8_t   s_doorBlinkStep       = 0;
static bool      s_doorLedOn           = false;

void ledApplyBaseFromDirection(const ::Config& cfg) {
  if (cfg.dev.direction == 1) {
    // Çıkış ürünü: YEŞİL sürekli
    setLED(false, true, false);
  } else {
    // Giriş ürünü: MAVİ sürekli
    setLED(true, false, false);
  }
}

void ledControllerInit(const ::Config& cfg, bool netUp) {
  s_ledNetUpOnBoot = netUp;

  // Boot sıralı LED gösterimine başla
  s_ledPhase   = LED_PHASE_BOOT_SEQ;
  s_ledStep    = 0;
  s_ledLastMs  = 0;
  s_ledOn      = false;

  s_doorEndMs       = 0;
  s_doorBlinkLastMs = 0;
  s_doorBlinkStep   = 0;
  s_doorLedOn       = false;

  // Pinler zaten setup'ta OUTPUT yapılıyor, LED'leri kapat
  setLED(false, false, false);
  digitalWrite(kapi, LOW);
}
void ledStartDoorSequence(const ::Config& cfg) {
  s_ledPhase        = LED_PHASE_DOOR;
  s_doorEndMs       = millis() + 1500;   // kapı 1.5 sn açık
  s_doorBlinkLastMs = 0;
  s_doorBlinkStep   = 0;
  s_doorLedOn       = false;

  digitalWrite(kapi, HIGH);              // kapıyı aç
  setLED(false, false, false);           // başta hepsini kapat
}
void ledUpdate(const ::Config& cfg) {
  uint32_t now = millis();

  switch (s_ledPhase) {

    // 1) BOOT: Mavi → Yeşil → Kırmızı (her biri 1sn)
    case LED_PHASE_BOOT_SEQ: {
      if (s_ledLastMs == 0) {
        // İlk giriş: mavi ON
        setLED(true, false, false);
        s_ledLastMs = now;
        s_ledStep   = 0;
        return;
      }

      if (now - s_ledLastMs < 1000) {
        return; // henüz sıradaki renge geçme zamanı değil
      }

      s_ledLastMs = now;
      s_ledStep++;

      if (s_ledStep == 1) {
        // Yeşil
        setLED(false, true, false);
      } else if (s_ledStep == 2) {
        // Kırmızı
        setLED(false, false, true);
      } else {
        // Boot gösterimi bitti → network durumuna göre blink'e geç
        setLED(false, false, false);
        s_ledLastMs = 0;
        s_ledStep   = 0;
        s_ledOn     = false;
        s_ledPhase  = s_ledNetUpOnBoot ? LED_PHASE_NET_OK_BLINK
                                       : LED_PHASE_NET_FAIL_BLINK;
      }
      return;
    }

    // 2) NET OK: 7 kere 150ms aralıkla kırmızı blink
    case LED_PHASE_NET_OK_BLINK: {
      const uint8_t totalSteps = 7 * 2; // ON+OFF
      const uint16_t intervalMs = 150;

      if (s_ledStep >= totalSteps) {
        // bittiyse idle'a geç
        s_ledPhase = LED_PHASE_IDLE;
        ledApplyBaseFromDirection(cfg);
        return;
      }

      if (s_ledLastMs == 0 || (now - s_ledLastMs) >= intervalMs) {
        s_ledLastMs = now;
        s_ledOn = !s_ledOn;
        s_ledStep++;

        if (s_ledOn) {
          setLED(false, false, true); // kırmızı ON
        } else {
          setLED(false, false, false);
        }
      }
      return;
    }

    // 3) NET FAIL: 3 kere 500ms aralıkla kırmızı blink
    case LED_PHASE_NET_FAIL_BLINK: {
      const uint8_t totalSteps = 3 * 2; // ON+OFF
      const uint16_t intervalMs = 500;

      if (s_ledStep >= totalSteps) {
        s_ledPhase = LED_PHASE_IDLE;
        ledApplyBaseFromDirection(cfg);
        return;
      }

      if (s_ledLastMs == 0 || (now - s_ledLastMs) >= intervalMs) {
        s_ledLastMs = now;
        s_ledOn = !s_ledOn;
        s_ledStep++;

        if (s_ledOn) {
          setLED(false, false, true); // kırmızı ON (buzzer da çalar)
        } else {
          setLED(false, false, false);
        }
      }
      return;
    }

    // 4) DOOR: kapı 1.5sn açık + kırmızı 3x 500ms blink
    case LED_PHASE_DOOR: {
      // Kapı süresi kontrolü
      if ((int32_t)(now - s_doorEndMs) >= 0) {
        digitalWrite(kapi, LOW);            // kapıyı kapa
        setLED(false, false, false);
        s_ledPhase = LED_PHASE_IDLE;
        ledApplyBaseFromDirection(cfg);
        return;
      }

      // LED blink: 3 kez ON/OFF (500ms)
      const uint8_t totalSteps = 3 * 2;
      const uint16_t intervalMs = 500;

      if (s_doorBlinkStep < totalSteps &&
          (s_doorBlinkLastMs == 0 || (now - s_doorBlinkLastMs) >= intervalMs)) {

        s_doorBlinkLastMs = now;
        s_doorLedOn = !s_doorLedOn;
        s_doorBlinkStep++;

        if (s_doorLedOn) {
          setLED(false, false, true);  // kırmızı ON (buzzer da)
        } else {
          setLED(false, false, false);
        }
      }
      return;
    }

    // 5) IDLE: direction'a göre sabit LED (hiçbir şey yapma)
    case LED_PHASE_IDLE:
    default:
      // Burada sürekli setLED yapmaya gerek yok, zaten doğru renkte
      return;
  }
}

// ----------------- SETUP -----------------
void setup() {
  Serial.begin(9600);
  delay(300);
  Serial.println();
  Serial.println("==== PDKS ESP8266 SETUP ====");

  // FS
  if (!LittleFS.begin()) {
    Serial.println("[FS] LittleFS.begin() FAIL, devam ediyoruz...");
  } else {
    Serial.println("[FS] LittleFS OK");
  }

  // Config yükle
  store.begin();
  store.load(cfg);          // /config.json yoksa default üretir

  // Time + Offline
  timeServiceInit();        // /time.dat varsa epoch offset'i kurar
  offlineInit();            // offline.bin + offline.idx hazırlar

  // Kapı/Röle ve LED pinleri
  pinMode(kapi,    OUTPUT);
  pinMode(mavi,    OUTPUT);
  pinMode(yesil,   OUTPUT);
  pinMode(kirmizi, OUTPUT);

  digitalWrite(kapi,    LOW);
  setLED(false, false, false);   // başta hepsi sönük
  // Ağ bağlantıları (WiFi + ETH birlikte)
  netInit(cfg);
  bool netUp = netIsOnline();

  if (!netUp) {
    Serial.println("[NET] UYARI: Hiçbir ağ arayüzü bağlanamadı (offline mod).");
  }
ledControllerInit(cfg, netUp);
  // İlk zaman senkronizasyonu (sunucu HTTP HEAD/GET ile Date header’dan)
  if (netUp && cfg.pdks.server_host.length()) {
    syncTimeFromHttp(cfg.pdks.server_host, "/");
  }

  // RDM
  initRDM6300();

  // OTA
  String otaPass = getDisablePassword();
  httpUpdater.setup(&server,
                    DEVICE_ID,        // kullanıcı adı
                    otaPass.c_str()); // şifre

  // Web arayüz / config
  if (!netUp) {
    // Sadece AP config portal
    startApConfigPortal(server, store, cfg);
  } else {
    // Hem normal çalışma hem /config UI
    attachConfigRoutes(server, store, cfg);
    server.begin();
    Serial.println("[HTTP] Web server started");
  }

  Serial.println("==== SETUP DONE ====");
  netHealthLoop(cfg); 
}


// ----------------- LOOP -----------------
void loop() {

  rdmLoop();
  server.handleClient();

  unsigned long nowMs = millis();

  // --- Kapı durum LED resetleri vb. ---
  if (!timedOut5 && (nowMs - timer5) > INTERVAL5) {
    oku       = true;
    timedOut5 = true;

    // GİRİŞ/ÇIKIŞ yönüne göre LED durumu (type yerine cfg.dev.direction)
    if (cfg.dev.direction == 0) {      // ÇIKIŞ
      digitalWrite(yesil, LOW);
      digitalWrite(mavi, HIGH);
    } else {                           // GİRİŞ
      digitalWrite(mavi, LOW);
      digitalWrite(yesil, HIGH);
    }
  }

  if (!timedOut3 && (nowMs - timer3) > INTERVAL3) {
    rst       = true;
    timedOut3 = true;
  }

  if (!timedOut0 && (nowMs - timer0) > INTERVAL0) {
    okart     = "000000";
    timedOut0 = true;
  }

  // --- RDM6300’den kart okuma ---
  if (oku) {
    if (rdmCardAvailable()) {
    String card = rdmGetCard();
   // punchNowOrQueue(card);

incomingByte = trimRdm(card);   // → "009B2E7C"

      timedOut5 = false;
      timer5    = nowMs;
      oku       = false;

    }
  }

  // --- Kart işleme ---
  if (incomingByte.length()) {
    kart = incomingByte;

    if (kart != okart) {
      // KAPI PULSE
      ledStartDoorSequence(cfg);   // Kapı 1.5sn + kırmızı 3x blink

      Serial.print("[PUNCH] Kart: ");
      Serial.println(kart);

      // ESP32’deki gibi: PunchSender devreye giriyor (online/offline)
      punchNowOrQueue(cfg, kart, (uint8_t)cfg.dev.direction);

      okart     = kart;
      timedOut0 = false;
      timer0    = nowMs;
    }

    incomingByte = "";
  }

  // --- Reset istemi ---
  if (rst) {
    Serial.println("Reset..");
    delay(500);
    ESP.restart();
  }

  

  // --- Periyodik zaman senkronu ---
  if (nowMs - lastTimeSyncMs >= TIME_SYNC_INTERVAL_MS) {
    lastTimeSyncMs = nowMs;
    if (cfg.pdks.server_host.length() && netIsOnline()) {
      syncTimeFromHttp(cfg.pdks.server_host, "/");
    }
  }
  ledUpdate(cfg);
netHealthLoop(cfg); 
  delay(3);
}

// ===================================================
// Merged from TimeService.cpp
// ===================================================

// netIsOnline() main .ino'da tanimli, burada kullanacagiz
extern bool netIsOnline();
static const char* TIME_FILE = "/time.dat";

static bool     s_timeSynced   = false;
static uint32_t s_epochOffset  = 0;   // serverEpoch - millis()/1000

bool timeIsSynced() {
  return s_timeSynced;
}

static int monthFromStr(const char* mon) {
  if (!mon) return 0;
  if (strcmp(mon, "Jan") == 0) return 1;
  if (strcmp(mon, "Feb") == 0) return 2;
  if (strcmp(mon, "Mar") == 0) return 3;
  if (strcmp(mon, "Apr") == 0) return 4;
  if (strcmp(mon, "May") == 0) return 5;
  if (strcmp(mon, "Jun") == 0) return 6;
  if (strcmp(mon, "Jul") == 0) return 7;
  if (strcmp(mon, "Aug") == 0) return 8;
  if (strcmp(mon, "Sep") == 0) return 9;
  if (strcmp(mon, "Oct") == 0) return 10;
  if (strcmp(mon, "Nov") == 0) return 11;
  if (strcmp(mon, "Dec") == 0) return 12;
  return 0;
}

// yil-ay-gun epoch'a ceviren, mktime kullanmayan fonksiyon
static bool ymdToEpochUtc(int year, int month, int day,
                          int hour, int min, int sec,
                          uint32_t* outEpoch) {
  if (!outEpoch) return false;
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
    return false;

  int y = year;
  int m = month;
  if (m <= 2) {
    y -= 1;
    m += 12;
  }

  int64_t a    = y / 4 - y / 100 + y / 400;
  int64_t days = 365LL * y + a + (153 * (m - 3) + 2) / 5 + day - 719469LL;

  if (days < 0) return false;

  int64_t epoch = days * 86400LL + hour * 3600LL + min * 60LL + sec;
  if (epoch <= 0) return false;

  *outEpoch = (uint32_t)epoch;
  return true;
}

// "Fri, 05 Dec 2025 10:26:30 GMT" -> epoch
static bool parseHttpDateToEpoch(const String& dateStr, uint32_t* outEpoch) {
  if (!outEpoch) return false;

  if (dateStr.length() < 29) {
    Serial.printf("[TIME] Date string too short: '%s'\n", dateStr.c_str());
    return false;
  }

  String dayStr  = dateStr.substring(5, 7);
  String monStr  = dateStr.substring(8, 11);
  String yearStr = dateStr.substring(12, 16);
  String hStr    = dateStr.substring(17, 19);
  String mStr    = dateStr.substring(20, 22);
  String sStr    = dateStr.substring(23, 25);

  dayStr.trim();
  monStr.trim();
  yearStr.trim();
  hStr.trim();
  mStr.trim();
  sStr.trim();

  int day   = dayStr.toInt();
  int year  = yearStr.toInt();
  int hour  = hStr.toInt();
  int min   = mStr.toInt();
  int sec   = sStr.toInt();

  int month = monthFromStr(monStr.c_str());

  Serial.printf("[TIME] Parsed parts: day=%d mon='%s'(%d) year=%d %02d:%02d:%02d\n",
                day, monStr.c_str(), month, year, hour, min, sec);

  if (day <= 0 || month == 0 || year < 1970) {
    Serial.println("[TIME] Date parts invalid");
    return false;
  }

  uint32_t epoch = 0;
  if (!ymdToEpochUtc(year, month, day, hour, min, sec, &epoch)) {
    Serial.printf("[TIME] ymdToEpochUtc FAIL for '%s'\n", dateStr.c_str());
    return false;
  }

  *outEpoch = epoch;
  Serial.printf("[TIME] ymdToEpochUtc OK, epoch=%u\n", epoch);
  return true;
}

void timeServiceInit() {
  if (!LittleFS.begin()) {
    Serial.println("[TIME] LittleFS.begin() FAIL");
    s_timeSynced = false;
    return;
  }

  if (!LittleFS.exists(TIME_FILE)) {
    Serial.println("[TIME] time.dat yok, zaman senkronize degil.");
    s_timeSynced = false;
    return;
  }

  File f = LittleFS.open(TIME_FILE, "r");
  if (!f) {
    Serial.println("[TIME] time.dat acilamadi.");
    s_timeSynced = false;
    return;
  }

  uint32_t storedEpoch = 0;
  if (f.read((uint8_t*)&storedEpoch, sizeof(storedEpoch)) != sizeof(storedEpoch)) {
    Serial.println("[TIME] time.dat boyut hatali.");
    f.close();
    s_timeSynced = false;
    return;
  }
  f.close();

  if (storedEpoch == 0) {
    Serial.println("[TIME] time.dat icinde 0 var, zaman senkronize degil.");
    s_timeSynced = false;
    return;
  }

  s_epochOffset = storedEpoch - (millis() / 1000UL);
  s_timeSynced  = true;
  Serial.printf("[TIME] time.dat okundu, storedEpoch=%u\n", storedEpoch);
}

void offlineUpdateTimeFromServerEpoch(uint32_t serverEpoch) {
  if (!LittleFS.begin()) {
    Serial.println("[TIME] LittleFS.begin() FAIL (update)");
    return;
  }

  s_epochOffset = serverEpoch - (millis() / 1000UL);
  s_timeSynced  = true;

  File f = LittleFS.open(TIME_FILE, "w");
  if (!f) {
    Serial.println("[TIME] time.dat yazilamadi.");
    return;
  }
  if (f.write((const uint8_t*)&serverEpoch, sizeof(serverEpoch)) != sizeof(serverEpoch)) {
    Serial.println("[TIME] time.dat write FAIL");
    f.close();
    return;
  }
  f.close();

  Serial.printf("[TIME] epoch guncellendi ve kaydedildi: %u\n", serverEpoch);
}

uint32_t nowTs() {
  if (!s_timeSynced) {
    return 0;
  }
  uint32_t nowSec = millis() / 1000UL;
  return s_epochOffset + nowSec;
}

bool syncTimeFromHttp(const String& host, const String& path) {
  if (!netIsOnline()) {
    Serial.println("[TIME] syncTimeFromHttp: net offline");
    return false;
  }

  if (host.length() == 0) {
    Serial.println("[TIME] syncTimeFromHttp: host bos");
    return false;
  }

  String url = "http://" + host + path;
  Serial.print("[TIME] HTTP time sync URL: ");
  Serial.println(url);

  HTTPClient http;
  WiFiClient client;

  if (!http.begin(client, url)) {
    Serial.println("[TIME] http.begin FAIL");
    return false;
  }

  const char* keys[] = { "X-Server-Epoch", "Date" };
  http.collectHeaders(keys, 2);

  int code = http.GET();
  if (code <= 0 || code >= 500) {
    Serial.printf("[TIME] HTTP code=%d\n", code);
    http.end();
    return false;
  }

  uint32_t epoch = 0;

  String hEpoch = http.header("X-Server-Epoch");
  hEpoch.trim();
  if (hEpoch.length() > 0) {
    epoch = (uint32_t)hEpoch.toInt();
    Serial.printf("[TIME] X-Server-Epoch: %s -> %u\n", hEpoch.c_str(), epoch);
  }

  if (epoch == 0) {
    String dateStr = http.header("Date");
    dateStr.trim();
    Serial.printf("[TIME] Date header: '%s'\n", dateStr.c_str());
    uint32_t e2 = 0;
    if (dateStr.length() > 0 && parseHttpDateToEpoch(dateStr, &e2)) {
      epoch = e2;
      Serial.printf("[TIME] Parsed Date -> epoch=%u\n", epoch);
    }
  }

  http.end();

  if (epoch == 0) {
    Serial.println("[TIME] epoch alinmadi");
    return false;
  }

  offlineUpdateTimeFromServerEpoch(epoch);
  Serial.println("[TIME] syncTimeFromHttp OK");
  return true;
}

// ===================================================
// Merged from offline.cpp
// ===================================================

static const char* OFFLINE_FILE = "/offline.bin";
static const char* OFFLINE_IDX  = "/offline.idx";

static const uint16_t OFFLINE_CAPACITY = 2048;

static bool     s_offlineInited = false;
static uint16_t s_head = 0;
static uint16_t s_tail = 0;

struct OfflineIndex {
  uint16_t head;
  uint16_t tail;
  uint16_t crc;
};

static uint16_t idxCrc(const OfflineIndex& idx) {
  return (uint16_t)(idx.head ^ idx.tail ^ 0xA5A5u);
}

static void offlineSaveIndexes() {
  if (!LittleFS.begin()) {
    Serial.println("[OFFLINE] LittleFS.begin FAIL (saveIdx)");
    return;
  }

  OfflineIndex idx;
  idx.head = s_head;
  idx.tail = s_tail;
  idx.crc  = idxCrc(idx);

  File f = LittleFS.open(OFFLINE_IDX, "w");
  if (!f) {
    Serial.println("[OFFLINE] offline.idx WRITE open FAIL");
    return;
  }

  if (f.write((const uint8_t*)&idx, sizeof(idx)) != sizeof(idx)) {
    Serial.println("[OFFLINE] offline.idx write FAIL");
  }
  f.close();
}

static void offlineLoadIndexes() {
  if (!LittleFS.begin()) {
    Serial.println("[OFFLINE] LittleFS.begin FAIL (loadIdx)");
    s_head = s_tail = 0;
    return;
  }

  if (!LittleFS.exists(OFFLINE_IDX)) {
    s_head = s_tail = 0;
    offlineSaveIndexes();
    return;
  }

  File f = LittleFS.open(OFFLINE_IDX, "r");
  if (!f) {
    Serial.println("[OFFLINE] offline.idx READ open FAIL");
    s_head = s_tail = 0;
    return;
  }

  OfflineIndex idx;
  if (f.read((uint8_t*)&idx, sizeof(idx)) != sizeof(idx)) {
    Serial.println("[OFFLINE] offline.idx size FAIL, reset indexes");
    s_head = s_tail = 0;
    f.close();
    offlineSaveIndexes();
    return;
  }
  f.close();

  if (idx.crc != idxCrc(idx)) {
    Serial.println("[OFFLINE] offline.idx CRC FAIL, reset indexes");
    s_head = s_tail = 0;
    offlineSaveIndexes();
    return;
  }

  s_head = idx.head % OFFLINE_CAPACITY;
  s_tail = idx.tail % OFFLINE_CAPACITY;

  Serial.printf("[OFFLINE] Index loaded. head=%u tail=%u\n", s_head, s_tail);
}

static bool offlineIsEmpty() {
  return (s_head == s_tail);
}

static bool offlineIsFull() {
  uint16_t nextHead = (uint16_t)((s_head + 1) % OFFLINE_CAPACITY);
  return (nextHead == s_tail);
}

static bool offlineWriteAt(uint16_t idx, const OfflineRecord& rec) {
  if (!LittleFS.begin()) {
    Serial.println("[OFFLINE] LittleFS.begin FAIL (writeAt)");
    return false;
  }

  File f = LittleFS.open(OFFLINE_FILE, "r+");
  if (!f) {
    Serial.println("[OFFLINE] offline.bin open FAIL (writeAt)");
    return false;
  }

  size_t offset = (size_t)idx * sizeof(OfflineRecord);
  if (!f.seek(offset, SeekSet)) {
    Serial.println("[OFFLINE] seek FAIL (writeAt)");
    f.close();
    return false;
  }

  if (f.write((const uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) {
    Serial.println("[OFFLINE] write FAIL (writeAt)");
    f.close();
    return false;
  }

  f.close();
  return true;
}

static bool offlineReadAt(uint16_t idx, OfflineRecord& rec) {
  if (!LittleFS.begin()) {
    Serial.println("[OFFLINE] LittleFS.begin FAIL (readAt)");
    return false;
  }

  File f = LittleFS.open(OFFLINE_FILE, "r");
  if (!f) {
    Serial.println("[OFFLINE] offline.bin open FAIL (readAt)");
    return false;
  }

  size_t offset = (size_t)idx * sizeof(OfflineRecord);
  if (!f.seek(offset, SeekSet)) {
    Serial.println("[OFFLINE] seek FAIL (readAt)");
    f.close();
    return false;
  }

  if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) {
    Serial.println("[OFFLINE] read FAIL (readAt)");
    f.close();
    return false;
  }

  f.close();
  return true;
}

void offlineInit() {
  if (s_offlineInited) return;

  if (!LittleFS.begin()) {
    Serial.println("[OFFLINE] LittleFS.begin FAIL (init)");
    return;
  }

  offlineLoadIndexes();

  if (!LittleFS.exists(OFFLINE_FILE)) {
    File f = LittleFS.open(OFFLINE_FILE, "w");
    if (f) {
      OfflineRecord zero;
      memset(&zero, 0, sizeof(zero));
      for (uint16_t i = 0; i < OFFLINE_CAPACITY; i++) {
        f.write((const uint8_t*)&zero, sizeof(zero));
      }
      f.close();
      Serial.println("[OFFLINE] offline.bin created & pre-allocated");
    } else {
      Serial.println("[OFFLINE] offline.bin create FAIL");
    }
  }

  s_offlineInited = true;
  Serial.printf("[OFFLINE] Init OK. head=%u tail=%u\n", s_head, s_tail);
}

bool offlineEnqueue(uint32_t ts, uint32_t cardId, uint8_t punchType) {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return false;

  OfflineRecord rec;
  rec.ts        = ts;
  rec.cardId    = cardId;
  rec.punchType = punchType;
  rec.reserved[0] = rec.reserved[1] = rec.reserved[2] = 0;

  uint16_t nextHead = (uint16_t)((s_head + 1) % OFFLINE_CAPACITY);

  if (offlineIsFull()) {
    s_tail = (uint16_t)((s_tail + 1) % OFFLINE_CAPACITY);
    Serial.println("[OFFLINE] Buffer FULL, oldest record overwritten");
  }

  if (!offlineWriteAt(s_head, rec)) {
    Serial.println("[OFFLINE] Enqueue write FAIL");
    return false;
  }

  s_head = nextHead;
  offlineSaveIndexes();
  Serial.printf("[OFFLINE] Enqueue OK. head=%u tail=%u\n", s_head, s_tail);
  return true;
}

bool offlineHasPending() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return false;
  return !offlineIsEmpty();
}

bool offlinePeek(OfflineRecord &rec) {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return false;
  if (offlineIsEmpty()) return false;
  return offlineReadAt(s_tail, rec);
}

bool offlinePop() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return false;
  if (offlineIsEmpty()) return false;

  s_tail = (uint16_t)((s_tail + 1) % OFFLINE_CAPACITY);
  offlineSaveIndexes();
  Serial.printf("[OFFLINE] Pop OK. head=%u tail=%u\n", s_head, s_tail);
  return true;
}

uint16_t offlinePendingCount() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return 0;

  int32_t diff = (int32_t)s_head - (int32_t)s_tail;
  if (diff < 0) diff += OFFLINE_CAPACITY;
  return (uint16_t)diff;
}

void offlineClear() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return;

  s_head = 0;
  s_tail = 0;
  offlineSaveIndexes();
  Serial.println("[OFFLINE] Queue cleared");
}

// ===================================================
// Merged from PunchSender.cpp
// ===================================================

extern bool netCanSendNow();

static uint32_t cardHexToUint(const String &cardHex)
{
    char buf[16];
    size_t len = cardHex.length();
    if (len > 8) len = 8;

    for (size_t i = 0; i < len; ++i) {
        buf[i] = cardHex[i];
    }
    buf[len] = '\0';

    char *endp = nullptr;
    uint32_t val = strtoul(buf, &endp, 16);
    if (endp == buf) {
        return 0;
    }
    return val;
}

bool sendPunchTCP(const Config &cfg, const String &cardHex, uint8_t punchType, uint32_t ts)
{
    IPAddress serverIP;
    if (!serverIP.fromString(cfg.pdks.server_host)) {
        Serial.println("[SEND] server_ip gecersiz!");
        return false;
    }

    uint16_t port = cfg.pdks.server_port;
    if (port == 0) port = 80;

    WiFiClient client;
    client.setTimeout(1500);

    Serial.printf("[SEND] Connect to %s:%u ...\n",
                  cfg.pdks.server_host.c_str(), port);

    if (!client.connect(serverIP, port)) {
        Serial.println("[SEND] TCP CONNECT FAIL");
        return false;
    }

    char payload[256];
    int n = snprintf(payload, sizeof(payload),
        "{\"device_no\":\"%s\",\"card_code\":\"%s\",\"company_id\":%u,\"type\":%u,\"created_time\":%u}",
        cfg.dev.device_no.c_str(),
        cardHex.c_str(),
        (unsigned)cfg.dev.company_id,
        (unsigned)punchType,
        (unsigned)ts
    );

    client.write((const uint8_t*)payload, n);
    client.write("\n", 1);
    client.flush();
    delay(10);
    client.stop();

    Serial.print("[SEND] sent: ");
    Serial.println(payload);

    return true;
}

void punchNowOrQueue(const Config &cfg, const String &cardHex, uint8_t punchType)
{
    uint32_t ts = nowTs();
    if (ts == 0) {
        Serial.println("[PUNCH] WARNING: Zaman senkronize degil (ts=0)");
    }

    if (!netCanSendNow()) {
        Serial.println("[PUNCH] Net health -> SEND DISABLED, offline enqueue");
        uint32_t cardNum = cardHexToUint(cardHex);
        offlineEnqueue(ts, cardNum, punchType);
        return;
    }

    if (!sendPunchTCP(cfg, cardHex, punchType, ts)) {
        Serial.println("[PUNCH] SEND FAIL -> offline enqueue");
        uint32_t cardNum = cardHexToUint(cardHex);
        offlineEnqueue(ts, cardNum, punchType);
        return;
    }

    Serial.println("[PUNCH] SEND OK");
}

void flushOfflinePunches(const Config &cfg)
{
    if (!netCanSendNow()) {
        Serial.println("[FLUSH] netCanSendNow=FALSE, flush skip");
        return;
    }

    OfflineRecord rec;
    uint8_t limit = 10;

    while (limit-- && offlinePeek(rec)) {
        char cardBuf[16];
        snprintf(cardBuf, sizeof(cardBuf), "%08X", (unsigned)rec.cardId);
        String cardHex(cardBuf);

        Serial.printf("[FLUSH] TRY ts=%u card=%s type=%u\n",
                      rec.ts, cardHex.c_str(), rec.punchType);

        if (sendPunchTCP(cfg, cardHex, rec.punchType, rec.ts)) {
            Serial.println("[FLUSH] OK -> pop");
            offlinePop();
        } else {
            Serial.println("[FLUSH] FAIL -> durdur");
            break;
        }
    }
}

// ===================================================
// Merged from Rdm.h
// ===================================================

static char rdmBuf[20];
static uint8_t rdmIdx = 0;

static bool     rdmHasCard = false;
static String   rdmLastCard = "";
static uint32_t rdmLastRead = 0;
static const uint32_t RDM_REPEAT_BLOCK_MS = 1500;

void initRDM6300() {
  rdmIdx = 0;
  rdmHasCard = false;
  Serial.println("[RDM] initRDM6300 (hardware serial, no library)");
}

void rdmLoop() {
  while (Serial.available()) {
    uint8_t b = Serial.read();

    if (b == 0x02) {
      rdmIdx = 0;
      continue;
    }

    if (b == 0x03) {
      rdmBuf[rdmIdx] = 0;

      if (rdmIdx >= 12) {
        String raw = String(rdmBuf);
        String idHex = raw.substring(0, 10);

        uint32_t now = millis();

        if (idHex != rdmLastCard || (now - rdmLastRead) > RDM_REPEAT_BLOCK_MS) {
          rdmLastCard = idHex;
          rdmLastRead = now;
          rdmHasCard  = true;

          Serial.printf("[RDM] Card HEX: %s\n", idHex.c_str());
        }
      }

      rdmIdx = 0;
      continue;
    }

    if (rdmIdx < sizeof(rdmBuf) - 1) {
      rdmBuf[rdmIdx++] = (char)b;
    }
  }
}

bool rdmCardAvailable() {
  return rdmHasCard;
}

String rdmGetCard() {
  rdmHasCard = false;
  return rdmLastCard;
}


