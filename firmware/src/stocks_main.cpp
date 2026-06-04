/*
  Portfolio — ESP32 CYD (ILI9341 2.8" 320×240)
  Stores cost basis (vol + avg) in the backend; polls /api/stocks which fetches
  the live market price (Yahoo Finance) and computes %U.PL = (mkt-avg)/avg*100.
  Draws a Maybank-style table: Symbol | Avg | Mkt | %U.PL  + a Total line.
  Shares hardware config + scaffolding with main.cpp (the token dashboard).
*/

#include <Arduino.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include "config.h"

TFT_eSPI    tft = TFT_eSPI();
WiFiManager wm;
Preferences prefs;

SPIClass vspi(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
bool g_touchReady = false;

char g_url[120];
char g_backupUrl[120];

// ── Colors ──────────────────────────────────────────────────────────────────
#define C_BG     0x0841
#define C_WHITE  0xFFFF
#define C_GRAY   0x7BEF
#define C_DGRAY  0x3186
#define C_GREEN  0x27E6
#define C_RED    0xF800
#define C_YELLOW 0xFFE0
#define C_HDR    0x0014   // dark blue header bar
#define C_DIV    0x2945

// ── Layout ────────────────────────────────────────────────────────────────────
#define HDR_H     24
#define COLHDR_Y  HDR_H
#define COLHDR_H  16
#define LIST_Y    (COLHDR_Y + COLHDR_H)        // 40
#define ROW_H     16
#define TOTAL_Y   210
#define STA_Y     226
#define ROWS_PER_PAGE ((TOTAL_Y - LIST_Y) / ROW_H)  // 10

// Column right edges (rotation 1 → 320 wide); Symbol is left-aligned.
#define X_SYM   4
#define X_VOL   148
#define X_AVG   200
#define X_MKT   255
#define X_UPL   318

// ── State ─────────────────────────────────────────────────────────────────────
#define MAX_HOLD 40
struct Holding {
  char  sym[14];
  long  vol;
  float avg, mkt, upl;
  bool  hasAvg, hasMkt;
};
Holding g_h[MAX_HOLD];
int   g_count = 0;
bool  g_dataValid = false;
int   g_page = 0;
String g_srcLabel = "";

bool  g_hasTotal = false;
float g_totalPct = 0, g_totalUpl = 0;

unsigned long g_lastFetch = 0, g_lastPage = 0;

int totalPages() { int p = (g_count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE; return p < 1 ? 1 : p; }
uint16_t plColor(float v) { return v > 0 ? C_GREEN : (v < 0 ? C_RED : C_GRAY); }

// Full integer with thousands separators, e.g. 122500 -> "122,500".
String withCommas(long n) {
  bool neg = n < 0;
  char tmp[16]; snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)(neg ? -n : n));
  String d = tmp, out = "";
  int len = d.length();
  for (int i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0) out += ',';
    out += d[i];
  }
  return neg ? "-" + out : out;
}

// ── Drawing ───────────────────────────────────────────────────────────────────
void drawTopBar(bool wifiOk) {
  tft.fillRect(0, 0, 320, HDR_H, C_HDR);
  tft.setTextFont(2); tft.setTextColor(C_WHITE, C_HDR); tft.setTextDatum(ML_DATUM);
  tft.drawString("Timecapital", 8, HDR_H / 2);

  time_t now = time(nullptr); struct tm* t = localtime(&now);
  char b[8]; snprintf(b, sizeof(b), "%02d:%02d", t->tm_hour, t->tm_min);
  tft.setTextDatum(MR_DATUM); tft.setTextColor(C_WHITE, C_HDR);
  tft.drawString(b, 290, HDR_H / 2);
  tft.fillCircle(309, HDR_H / 2, 4, wifiOk ? C_GREEN : C_RED);
}

void drawColHeader() {
  tft.fillRect(0, COLHDR_Y, 320, COLHDR_H, C_BG);
  tft.setTextFont(1); tft.setTextColor(C_GRAY, C_BG);
  int y = COLHDR_Y + COLHDR_H / 2;
  tft.setTextDatum(ML_DATUM); tft.drawString("Symbol", X_SYM, y);
  tft.setTextDatum(MR_DATUM);
  tft.drawString("Vol",   X_VOL, y);
  tft.drawString("Avg",   X_AVG, y);
  tft.drawString("Mkt",   X_MKT, y);
  tft.drawString("%U.PL", X_UPL, y);
  tft.drawLine(0, COLHDR_Y + COLHDR_H, 320, COLHDR_Y + COLHDR_H, C_DIV);
}

void drawList() {
  tft.fillRect(0, LIST_Y, 320, TOTAL_Y - LIST_Y, C_BG);
  int start = g_page * ROWS_PER_PAGE;

  for (int i = 0; i < ROWS_PER_PAGE; i++) {
    int idx = start + i;
    if (idx >= g_count) break;
    Holding& h = g_h[idx];
    int y = LIST_Y + i * ROW_H + ROW_H / 2;
    char b[16];

    tft.setTextFont(2);
    tft.setTextColor(C_WHITE, C_BG); tft.setTextDatum(ML_DATUM);
    tft.drawString(h.sym, X_SYM, y);

    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_GRAY, C_BG);
    tft.drawString(withCommas(h.vol), X_VOL, y);

    tft.setTextColor(C_WHITE, C_BG);
    if (h.hasAvg) dtostrf(h.avg, 0, 2, b); else strcpy(b, "-");
    tft.drawString(b, X_AVG, y);

    if (h.hasMkt) { dtostrf(h.mkt, 0, 2, b); tft.setTextColor(C_WHITE, C_BG); }
    else          { strcpy(b, "N/A");        tft.setTextColor(C_GRAY,  C_BG); }
    tft.drawString(b, X_MKT, y);

    if (h.hasMkt && h.hasAvg) {
      if (h.upl == 0) strcpy(b, "0.00");
      else            snprintf(b, sizeof(b), "%+.2f", h.upl);
      tft.setTextColor(plColor(h.upl), C_BG);
    } else { strcpy(b, "N/A"); tft.setTextColor(C_YELLOW, C_BG); }
    tft.drawString(b, X_UPL, y);
  }
}

void drawTotal() {
  tft.fillRect(0, TOTAL_Y, 320, STA_Y - TOTAL_Y, C_BG);
  tft.drawLine(0, TOTAL_Y, 320, TOTAL_Y, C_DIV);
  int y = TOTAL_Y + (STA_Y - TOTAL_Y) / 2;
  tft.setTextFont(2);
  tft.setTextColor(C_GRAY, C_BG); tft.setTextDatum(ML_DATUM);
  tft.drawString("Total", X_SYM, y);
  if (!g_hasTotal) return;

  // unrealized P/L value (full integer baht) at the Mkt column
  long tv = (long)(g_totalUpl >= 0 ? g_totalUpl + 0.5f : g_totalUpl - 0.5f);
  String tval = (tv >= 0 ? "+" : "") + withCommas(tv);
  tft.setTextColor(plColor(g_totalUpl), C_BG); tft.setTextDatum(MR_DATUM);
  tft.drawString(tval, X_MKT, y);
  // total %U.PL at the %U.PL column
  char b[16];
  snprintf(b, sizeof(b), "%+.2f%%", g_totalPct);
  tft.setTextColor(plColor(g_totalPct), C_BG);
  tft.drawString(b, X_UPL, y);
}

void drawStatus(int secondsLeft) {
  tft.fillRect(0, STA_Y, 320, 240 - STA_Y, C_BG);
  tft.setTextFont(1); tft.setTextDatum(ML_DATUM);
  char b[48];
  if (g_dataValid) {
    tft.setTextColor(C_DGRAY, C_BG);
    snprintf(b, sizeof(b), "%d/%d  %s", g_page + 1, totalPages(), g_srcLabel.c_str());
  } else { tft.setTextColor(C_RED, C_BG); strcpy(b, "Server unreachable"); }
  tft.drawString(b, 4, STA_Y + 4);

  snprintf(b, sizeof(b), "Next %ds", secondsLeft);
  tft.setTextColor(C_DGRAY, C_BG); tft.setTextDatum(MR_DATUM);
  tft.drawString(b, 316, STA_Y + 4);
}

void renderAll(bool wifiOk) {
  drawTopBar(wifiOk);
  drawColHeader();
  drawList();
  drawTotal();
}

// ── Network ───────────────────────────────────────────────────────────────────
bool fetchFromUrl(const char* url, const char* label) {
  if (strlen(url) < 8) return false;
  HTTPClient http;
  bool isHttps = strncmp(url, "https", 5) == 0;
  static WiFiClientSecure sc;
  if (isHttps) { sc.setInsecure(); sc.setHandshakeTimeout(20); http.begin(sc, url); }
  else         { http.begin(url); }
  http.setReuse(false);
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray arr = doc["holdings"].as<JsonArray>();
      if (!arr.isNull()) {
        g_count = 0;
        for (JsonObject s : arr) {
          if (g_count >= MAX_HOLD) break;
          Holding& h = g_h[g_count];
          strncpy(h.sym, s["sym"] | "", sizeof(h.sym) - 1);
          h.sym[sizeof(h.sym) - 1] = '\0';
          h.vol    = s["vol"] | 0L;
          h.hasAvg = !s["avg"].isNull(); h.avg = s["avg"] | 0.0f;
          h.hasMkt = !s["mkt"].isNull(); h.mkt = s["mkt"] | 0.0f;
          h.upl    = s["upl"] | 0.0f;
          g_count++;
        }
        JsonVariant tot = doc["total"];
        g_hasTotal = !tot.isNull() && !tot["pct"].isNull();
        g_totalPct = tot["pct"] | 0.0f;
        g_totalUpl = tot["upl"] | 0.0f;

        g_dataValid = g_count > 0;
        g_srcLabel  = label;
        if (g_page >= totalPages()) g_page = 0;
        http.end();
        return true;
      }
    }
  }
  http.end();
  return false;
}

void fetchData() {
  if (WiFi.status() != WL_CONNECTED) { g_dataValid = false; return; }
  // ESP32 TLS handshakes sometimes reset on the first try — retry the primary.
  for (int attempt = 0; attempt < 3; attempt++) {
    if (fetchFromUrl(g_url, "Yahoo")) return;
    delay(600);
  }
  if (fetchFromUrl(g_backupUrl, "Local")) return;
  g_dataValid = false;
}

// ── Preferences ───────────────────────────────────────────────────────────────
void loadUrls() {
  prefs.begin("stocks", true);
  String p = prefs.getString("url",       STOCKS_API_URL);
  String b = prefs.getString("backupUrl", STOCKS_BACKUP_URL);
  prefs.end();
  strncpy(g_url,       p.c_str(), sizeof(g_url) - 1);
  strncpy(g_backupUrl, b.c_str(), sizeof(g_backupUrl) - 1);
}

void saveUrls(const char* p, const char* b) {
  prefs.begin("stocks", false);
  prefs.putString("url", p);
  prefs.putString("backupUrl", b);
  prefs.end();
}

// ── Touch: top half = prev page, bottom half = next page ──────────────────────
void checkTouch() {
  if (!g_touchReady || !ts.touched()) return;
  TS_Point p = ts.getPoint();
  int y = constrain(map(p.y, 200, 3800, 0, 240), 0, 239);

  if (g_count > ROWS_PER_PAGE) {
    if (y < 120) g_page = (g_page - 1 + totalPages()) % totalPages();
    else         g_page = (g_page + 1) % totalPages();
    drawList();
    g_lastPage = millis();
  }
  delay(200);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL_PIN, OUTPUT); digitalWrite(TFT_BL_PIN, HIGH);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  tft.init(); tft.setRotation(1); tft.fillScreen(C_BG);

  vspi.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(vspi); ts.setRotation(1);
  g_touchReady = true;

  drawTopBar(false);
  loadUrls();

  WiFiManagerParameter pUrl   ("url",    "Portfolio URL", g_url,       119);
  WiFiManagerParameter pBackup("backup", "Backup URL",    g_backupUrl, 119);
  wm.addParameter(&pUrl);
  wm.addParameter(&pBackup);

  tft.setTextFont(2); tft.setTextColor(C_GRAY, C_BG);
  tft.setTextDatum(MC_DATUM); tft.drawString("Connecting...", 160, 130);

  wm.setSaveParamsCallback([&]() {
    saveUrls(pUrl.getValue(), pBackup.getValue());
    strncpy(g_url,       pUrl.getValue(),    sizeof(g_url) - 1);
    strncpy(g_backupUrl, pBackup.getValue(), sizeof(g_backupUrl) - 1);
  });
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("CYD-Stocks")) { delay(3000); ESP.restart(); }

  // Some networks hijack/block DNS (e.g. redirecting *.vercel.app). Keep the
  // DHCP-assigned address but force public DNS resolvers to bypass that.
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
              IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));

  setenv("TZ", "UTC-7", 1); tzset();
  configTime(7 * 3600, 0, "time.nist.gov", "time.google.com", "pool.ntp.org");
  time_t now = time(nullptr); int retries = 40;
  while (now < 1609459200 && retries-- > 0) { delay(200); now = time(nullptr); }

  tft.fillScreen(C_BG);
  fetchData();
  renderAll(WiFi.status() == WL_CONNECTED);
  drawStatus(STOCKS_UPDATE_INTERVAL / 1000);
  g_lastFetch = millis();
  g_lastPage  = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static unsigned long bootHoldStart = 0;
static int prevMin = -1, prevStatusSec = -1;

void loop() {
  checkTouch();

  if (digitalRead(BOOT_PIN) == LOW) {
    if (bootHoldStart == 0) bootHoldStart = millis();
    if (millis() - bootHoldStart >= 3000) {
      tft.fillScreen(C_BG); drawTopBar(false);
      tft.setTextFont(4); tft.setTextColor(C_RED, C_BG); tft.setTextDatum(MC_DATUM);
      tft.drawString("Resetting...", 160, 120);
      wm.resetSettings();
      prefs.begin("stocks", false); prefs.clear(); prefs.end();
      delay(2000); ESP.restart();
    }
  } else bootHoldStart = 0;

  unsigned long elapsed = millis() - g_lastFetch;
  int secondsLeft = (elapsed >= STOCKS_UPDATE_INTERVAL) ? 0
                    : (int)((STOCKS_UPDATE_INTERVAL - elapsed) / 1000UL);

  if (elapsed >= STOCKS_UPDATE_INTERVAL) {
    if (WiFi.status() != WL_CONNECTED) wm.autoConnect("CYD-Stocks");
    fetchData();
    renderAll(WiFi.status() == WL_CONNECTED);
    g_lastFetch = millis();
    secondsLeft = STOCKS_UPDATE_INTERVAL / 1000;
  }

  if (g_dataValid && g_count > ROWS_PER_PAGE && millis() - g_lastPage >= STOCKS_PAGE_MS) {
    g_page = (g_page + 1) % totalPages();
    drawList();
    g_lastPage = millis();
    prevStatusSec = -1;
  }

  time_t now = time(nullptr); struct tm* t = localtime(&now);
  if (t->tm_min != prevMin) { drawTopBar(WiFi.status() == WL_CONNECTED); prevMin = t->tm_min; }

  if (secondsLeft != prevStatusSec) { drawStatus(secondsLeft); prevStatusSec = secondsLeft; }

  delay(150);
}
