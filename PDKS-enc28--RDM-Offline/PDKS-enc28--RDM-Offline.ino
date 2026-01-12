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

extern "C" {
#include "user_interface.h"
}

#include "ConfigSchema.h"
#include "Web.h"        // ESP32’deki gibi: /config UI + AP portal burada
#include "Offline.h"

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
void ledNotifyNetChange(const ::Config& cfg, bool online);

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

static const uint32_t NET_HEALTH_CHECK_MS = 10UL * 60UL * 1000UL; // 10 dk
static const uint32_t NET_HEALTH_RESET_MS = 10UL * 60UL * 1000UL; // 10 dk

bool netCanSendNow() {
  return s_canSendNow;
}


void netHealthLoop(const ::Config& cfg) {
  uint32_t now = millis();

  // İlk çalışmada hemen kontrol yapsın
  if (s_lastHealthMs == 0) {
    s_lastHealthMs = now;
  } else {
    // Check aralığı dolmadıysa sağlık kontrolü yapma
    if (now - s_lastHealthMs < NET_HEALTH_CHECK_MS) {
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
      ledNotifyNetChange(cfg, true);
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
      ledNotifyNetChange(cfg, false);
    }
    s_canSendNow = false;

    // Offline süresi 10 dk'yı geçtiyse ENC reset + re-init
    if (s_lastNetOkMs != 0 && (now - s_lastNetOkMs) >= NET_HEALTH_RESET_MS) {
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
unsigned long INTERVAL0  = 4000,  timer0;   bool timedOut0  = true;
unsigned long INTERVAL3  = 10000, timer3;   bool timedOut3  = true;
unsigned long INTERVAL5  = 3000,  timer5;   bool timedOut5  = false;
bool          oku         = true;
unsigned long INTERVAL12 = 1000,  timer12;  bool timedOut12 = true;
unsigned long INTERVAL   = 1000,  timer1;   bool timedOut1  = true;
unsigned long INTERVAL10 = 300,   timer10;  bool timedOut10 = true;
unsigned long previousMillis = 0;

bool          busyPunch = false;
bool          pendingPunch = false;
String        pendingCard  = "";
uint8_t       pendingType  = 0;
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

  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  WiFi.mode(WIFI_STA);
  delay(100);

  uint8_t macw[6];
  buildWifiMac(cfg, macw);
  wifi_set_macaddr(STATION_IF, macw);
  WiFi.hostname(DEVICE_ID);
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

void ledNotifyNetChange(const ::Config& cfg, bool online) {
  (void)cfg;
  (void)online;
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
        s_ledPhase = LED_PHASE_IDLE;
        ledApplyBaseFromDirection(cfg);
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
  if (oku && !busyPunch) {
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
  if (incomingByte.length() && !busyPunch) {
    kart = incomingByte;

    if (kart != okart) {
      // KAPI PULSE
      ledStartDoorSequence(cfg);   // Kapı 1.5sn + kırmızı 3x blink

      Serial.print("[PUNCH] Kart: ");
      Serial.println(kart);

      // Non-blocking: kapı/LED bitince gönder
      pendingCard  = kart;
      pendingType  = (uint8_t)cfg.dev.direction;
      pendingPunch = true;
      busyPunch    = true;

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

  if (pendingPunch && s_ledPhase != LED_PHASE_DOOR) {
    punchNowOrQueue(cfg, pendingCard, pendingType);
    pendingPunch = false;
    busyPunch = false;
  }

netHealthLoop(cfg); 
  delay(3);
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
