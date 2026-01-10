#pragma once

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ENC28J60lwIP.h>
#include "ConfigSchema.h"

extern ENC28J60lwIP eth;

static inline String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// -------------------------------------------------------------------
//  AP SSID / PASS helper'ları
//  - ap_ssid boşsa device_no kullan
//  - ap_pass boşsa default şifre kullan ("0102030003" gibi)
//  NOT: DevConfig'e bu iki alanı eklediğini varsayıyorum:
//    struct DevConfig { String device_no; uint32_t company_id; uint8_t direction;
//                       String ap_ssid; String ap_pass; };
// -------------------------------------------------------------------

// --- AP SSID belirleme ---
// AP SSID = DEVICE_ID (ör: PDKS_0003)
static inline String getApSsid(const Config& cfg) {
  return String(DEVICE_ID);
}

// --- AP Password belirleme ---
// AP şifresi = 010203XXXX (DEVICE_ID son 4)
static inline String getApPassword(const Config& cfg) {
  return getDisablePassword();   // ör: 0102030003
}

// -------------------------------------------------------------------
// Küçük yardımcılar
// -------------------------------------------------------------------

static inline bool parseBoolField(const String& v, bool def) {
  if (v.length() == 0) return def;
  if (v == "1" || v == "true"  || v == "on"  || v == "ON")  return true;
  if (v == "0" || v == "false" || v == "off" || v == "OFF") return false;
  return def;
}

// -------------------------------------------------------------------
// /config + /config/save + /ping + G0/G1/G2 route'ları
// -------------------------------------------------------------------

static void attachConfigRoutes(ESP8266WebServer& server,
                               ConfigStore&       store,
                               Config&            cfg)
{
  // root → /config
  server.on("/", HTTP_GET, [&]() {
    server.sendHeader("Location", "/config", true);
    server.send(302, "text/plain", "");
  });

  // Basit ping
  server.on("/ping", HTTP_GET, [&]() {
    IPAddress ip = WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP();
    String js = String("{\"ok\":true,\"device_no\":\"") + cfg.dev.device_no +
                "\",\"ip\":\"" + ip.toString() + "\"}";
    server.send(200, "application/json", js);
  });

  // ---- CONFIG HTML FORM (/config) ----
  server.on("/config", HTTP_GET, [&]() {

    String apSsid = getApSsid(cfg);
    String apPass = getApPassword(cfg);
    IPAddress wifiIp = WiFi.isConnected() ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
    IPAddress ethIp  = (eth.status() == WL_CONNECTED) ? eth.localIP() : IPAddress(0, 0, 0, 0);
    IPAddress ip = (eth.status() == WL_CONNECTED)
                   ? eth.localIP()
                   : (WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP());
    String ipStr = ip.toString();

    String wifiMac = WiFi.macAddress();
    uint8_t ethMacBytes[6];
    buildEthMac(cfg, ethMacBytes);
    String ethMac = macToString(ethMacBytes);

    String html;
    html.reserve(5000);

    html  = "<!DOCTYPE HTML><html lang='tr'><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>IoT PDKS Ayarlari</title>";

    // Basit ama şık CSS (ESP32 tarafına benzer)
    html += "<style>";
    html += "body{margin:0;padding:0;font-family:Arial,Helvetica,sans-serif;background:#e5e7eb;color:#111;}";
    html += ".page{max-width:900px;margin:20px auto;padding:20px;border-radius:14px;background:#ffffff;box-shadow:0 10px 25px rgba(15,23,42,0.18);}";
    html += ".header{text-align:center;margin-bottom:20px;}";
    html += ".header h1{font-size:22px;margin:0 0 6px;color:#111827;}";
    html += ".header h2{font-size:13px;margin:0 0 10px;color:#6b7280;}";
    html += ".badge{display:inline-block;margin:4px 6px;padding:5px 12px;border-radius:999px;font-size:12px;background:#e0f2fe;color:#0f172a;border:1px solid #bae6fd;}";
    html += "fieldset{border:1px solid #d1d5db;border-radius:10px;padding:12px;margin-top:12px;background:#f9fafb;}";
    html += "legend{padding:4px 12px;border-radius:999px;background:#e0f2fe;border:1px solid #bfdbfe;color:#0f172a;font-size:13px;}";
    html += "table{width:100%;border-collapse:collapse;}";
    html += "th,td{text-align:left;padding:6px 4px;font-size:13px;vertical-align:middle;}";
    html += "label{font-weight:bold;color:#374151;}";
    html += "input[type='text'],input[type='password'],input[type='number']{width:100%;max-width:260px;padding:7px 9px;border-radius:8px;border:1px solid #cbd5f5;background:#f9fafb;color:#111827;box-sizing:border-box;font-size:13px;transition:border-color .15s,box-shadow .15s,background-color .15s;}";
    html += "input:focus{outline:none;border-color:#0ea5e9;box-shadow:0 0 0 1px #0ea5e9;background:#eff6ff;}";
    html += ".hint{font-size:11px;color:#6b7280;margin-bottom:8px;}";
    html += ".btn-primary{margin-top:15px;display:inline-block;padding:10px 26px;border-radius:999px;border:none;cursor:pointer;background:linear-gradient(135deg,#0ea5e9,#6366f1);color:#ffffff;font-weight:bold;font-size:13px;letter-spacing:0.3px;box-shadow:0 8px 18px rgba(37,99,235,0.35);}";
    html += ".btn-primary:hover{filter:brightness(1.05);box-shadow:0 10px 22px rgba(37,99,235,0.45);}";
    html += ".footer{margin-top:15px;text-align:center;font-size:11px;color:#6b7280;}";
    html += "</style></head><body>";

    html += "<div class='page'><div class='header'>";
    html += "<h1>IoT PDKS Konfigürasyon</h1>";
    html += "<h2>ESP8266 + ENC28J60 - Offline PDKS</h2>";
    html += "<div>";
    html += "<span class='badge'>Device: " + cfg.dev.device_no + "</span>";
    html += "<span class='badge'>IP: "     + ipStr           + "</span>";
    html += "<span class='badge'>WiFi IP: " + wifiIp.toString() + "</span>";
    html += "<span class='badge'>ETH IP: "  + ethIp.toString()  + "</span>";
    html += "<span class='badge'>WiFi MAC: " + wifiMac + "</span>";
    html += "<span class='badge'>ETH MAC: "  + ethMac  + "</span>";
    html += "</div></div>";

    html += "<form method='post' action='/config/save'>";

    // ---- Cihaz & PDKS ----
    html += "<fieldset><legend>Cihaz &amp; PDKS</legend><table>";

    html += "<tr><th><label for='company_id'>Sirket ID</label></th><td>";
    html += "<input id='company_id' name='company_id' type='number' value='" + String(cfg.dev.company_id) + "'>";
    html += "</td></tr>";

        html += "<tr><th>Yön</th><td>";

    // 0 = Giriş
    html += "<label style='margin-right:12px;'>";
    html += "<input type='radio' name='direction' value='0'";
    if (cfg.dev.direction == 0) {
      html += " checked";
    }
    html += "> Çıkış</label>";

    // 1 = Çıkış
    html += "<label>";
    html += "<input type='radio' name='direction' value='1'";
    if (cfg.dev.direction == 1) {
      html += " checked";
    }
    html += "> Giriş</label>";

    html += "</td></tr>";


    html += "<tr><th><label for='server_host'>Server Host/IP</label></th><td>";
    html += "<input id='server_host' name='server_host' type='text' value='" + cfg.pdks.server_host + "'>";
    html += "</td></tr>";

    html += "<tr><th><label for='server_port'>Server Port</label></th><td>";
    html += "<input id='server_port' name='server_port' type='number' value='" + String(cfg.pdks.server_port) + "'>";
    html += "</td></tr>";

    html += "</table></fieldset>";
/*
    // ---- AP Ayarları (SSID/PASS) ----
    html += "<fieldset><legend>AP (Konfig) Ayarlari</legend><table>";

    html += "<tr><th><label for='ap_ssid'>AP SSID</label></th><td>";
    html += "<input id='ap_ssid' name='ap_ssid' type='text' value='" + apSsid + "'>";
    html += "</td></tr>";

    html += "<tr><th><label for='ap_pass'>AP Sifre</label></th><td>";
    html += "<input id='ap_pass' name='ap_pass' type='password' value='" + apPass + "'>";
    html += "</td></tr>";

    html += "</table></fieldset>";
*/
    // ---- Net Mode ----
    html += "<fieldset><legend>Ag Modu</legend><table>";
    html += "<tr><th>Mod</th><td>";
    html += "<label style='margin-right:12px;'>";
    html += "<input type='radio' name='net_mode' value='wifi'";
    if (cfg.net.wifi_enabled) {
      html += " checked";
    }
    html += "> WiFi</label>";
    html += "<label>";
    html += "<input type='radio' name='net_mode' value='eth'";
    if (!cfg.net.wifi_enabled) {
      html += " checked";
    }
    html += "> Ethernet</label>";
    html += "</td></tr>";
    html += "</table></fieldset>";

    // ---- WiFi ----
    html += "<fieldset><legend>WiFi Ayarlari</legend>";
    html += "<table>";

    html += "<tr><th><label for='wifi_ssid'>SSID</label></th><td>";
    html += "<input id='wifi_ssid' name='wifi_ssid' type='text' value='" + cfg.net.wifi_ssid + "'>";
    html += "</td></tr>";

    html += "<tr><th><label for='wifi_pass'>Sifre</label></th><td>";
    html += "<input id='wifi_pass' name='wifi_pass' type='password' value='" + cfg.net.wifi_pass + "'>";
    html += "</td></tr>";

    html += "</table></fieldset>";

    // ---- Ethernet / Statik IP ----
    html += "<fieldset><legend>Ethernet &amp; IP</legend>";
    html += "<div class='hint'>IP bos ise DHCP, dolu ise statik.</div>";
    html += "<table>";

    html += "<tr><th><label for='ip'>IP Adresi</label></th><td>";
    html += "<input id='ip' name='ip' type='text' value='" + cfg.net.ip + "'>";
    html += "</td></tr>";

    html += "<tr><th><label for='subnet'>Subnet Mask</label></th><td>";
    html += "<input id='subnet' name='subnet' type='text' value='" + cfg.net.subnet + "'>";
    html += "</td></tr>";

    html += "<tr><th><label for='gateway'>Gateway</label></th><td>";
    html += "<input id='gateway' name='gateway' type='text' value='" + cfg.net.gateway + "'>";
    html += "</td></tr>";

    html += "<tr><th><label for='dns'>DNS</label></th><td>";
    html += "<input id='dns' name='dns' type='text' value='" + cfg.net.dns + "'>";
    html += "</td></tr>";

    html += "</table></fieldset>";

    html += "<button type='submit' class='btn-primary'>AYARLARI KAYDET</button>";
    html += "<div class='footer'>Kayit sonrasi cihaz otomatik yeniden baslatilabilir.</div>";

    html += "</form></div></body></html>";

    server.send(200, "text/html; charset=utf-8", html);
  });

  // ---- CONFIG SAVE (POST /config/save) ----
  server.on("/config/save", HTTP_POST, [&]() {
    auto arg = [&](const String& name) -> String {
      return server.hasArg(name) ? server.arg(name) : String("");
    };
    String apSsid = getApSsid(cfg);
    String apPass = getApPassword(cfg);
    // Dev
    cfg.dev.company_id = arg("company_id").toInt();
    cfg.dev.direction  = (uint8_t)arg("direction").toInt();
    apSsid    = arg("ap_ssid");
    apPass    = arg("ap_pass");

    // Pdks
    cfg.pdks.server_host = arg("server_host");
    cfg.pdks.server_port = (uint16_t)arg("server_port").toInt();

    // Net
    String netMode = arg("net_mode");
    if (netMode == "wifi") {
      cfg.net.wifi_enabled = true;
      cfg.net.eth_enabled  = false;
    } else if (netMode == "eth") {
      cfg.net.wifi_enabled = false;
      cfg.net.eth_enabled  = true;
    }
    cfg.net.wifi_ssid     = arg("wifi_ssid");
    cfg.net.wifi_pass     = arg("wifi_pass");

    cfg.net.ip            = arg("ip");
    cfg.net.subnet        = arg("subnet");
    cfg.net.gateway       = arg("gateway");
    cfg.net.dns           = arg("dns");

    bool ok = store.save(cfg);

    if (ok) {
      String js = "{\"ok\":true,\"msg\":\"Config kaydedildi, cihaz yeniden baslatiliyor\"}";
      server.send(200, "application/json", js);
      Serial.println("[CFG] save OK, restarting...");
      delay(500);
      ESP.restart();
    } else {
      String js = "{\"ok\":false,\"msg\":\"Config kaydedilemedi\"}";
      server.send(500, "application/json", js);
    }
  });


  // 404
  server.onNotFound([&]() {
    String js = "{\"ok\":false,\"error\":\"not_found\"}";
    server.send(404, "application/json", js);
  });
}

// -------------------------------------------------------------------
// AP Config Portal'ı başlatan helper
// -------------------------------------------------------------------
static void startApConfigPortal(ESP8266WebServer& server,
                                ConfigStore&      store,
                                Config&           cfg)
{
  WiFi.mode(WIFI_AP);
  
  String ssid = getApSsid(cfg);
  String pass = getApPassword(cfg);

  WiFi.softAP(ssid.c_str(), pass.c_str(), 6);

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("[AP] Config portal SSID: " + ssid);
  Serial.print("[AP] IP: ");
  Serial.println(apIP);

  attachConfigRoutes(server, store, cfg);
  server.begin();
  Serial.println("[HTTP] Config portal started");
}

