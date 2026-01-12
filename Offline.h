#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <stdlib.h>

#include "ConfigSchema.h"

// netIsOnline / netCanSendNow main .ino'da
extern bool netIsOnline();
extern bool netCanSendNow();

// ---- Offline record ----
struct OfflineRecord {
  uint32_t ts;
  uint32_t cardId;
  uint8_t  punchType;
  uint8_t  reserved[3];
};

// ===================================================
// TimeService (offline time)
// ===================================================

static const char* TIME_FILE = "/time.dat";
static bool     s_timeSynced   = false;
static uint32_t s_epochOffset  = 0;   // serverEpoch - millis()/1000

inline bool timeIsSynced() {
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

inline void timeServiceInit() {
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

inline void offlineUpdateTimeFromServerEpoch(uint32_t serverEpoch) {
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

inline uint32_t nowTs() {
  if (!s_timeSynced) {
    return 0;
  }
  uint32_t nowSec = millis() / 1000UL;
  return s_epochOffset + nowSec;
}

inline bool syncTimeFromHttp(const String& host, const String& path = "/") {
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
// Offline queue
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

inline void offlineInit() {
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

inline bool offlineEnqueue(uint32_t ts, uint32_t cardId, uint8_t punchType) {
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

inline bool offlineHasPending() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return false;
  return !offlineIsEmpty();
}

inline bool offlinePeek(OfflineRecord &rec) {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return false;
  if (offlineIsEmpty()) return false;
  return offlineReadAt(s_tail, rec);
}

inline bool offlinePop() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return false;
  if (offlineIsEmpty()) return false;

  s_tail = (uint16_t)((s_tail + 1) % OFFLINE_CAPACITY);
  offlineSaveIndexes();
  Serial.printf("[OFFLINE] Pop OK. head=%u tail=%u\n", s_head, s_tail);
  return true;
}

inline uint16_t offlinePendingCount() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return 0;

  int32_t diff = (int32_t)s_head - (int32_t)s_tail;
  if (diff < 0) diff += OFFLINE_CAPACITY;
  return (uint16_t)diff;
}

inline void offlineClear() {
  if (!s_offlineInited) offlineInit();
  if (!s_offlineInited) return;

  s_head = 0;
  s_tail = 0;
  offlineSaveIndexes();
  Serial.println("[OFFLINE] Queue cleared");
}

// ===================================================
// PunchSender
// ===================================================

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

inline bool sendPunchTCP(const ::Config &cfg, const String &cardHex, uint8_t punchType, uint32_t ts)
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

inline void punchNowOrQueue(const ::Config &cfg, const String &cardHex, uint8_t punchType)
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

inline void flushOfflinePunches(const ::Config &cfg)
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
