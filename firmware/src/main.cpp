/*
  Claude Token Dashboard — ESP32 CYD (ILI9341 2.8" 320×240)
  WiFiManager + Dual URL Fallback + Touch Tabs (TODAY/WEEK/MONTH)
  Using TFT_eSPI built-in XPT2046 touch support
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

// XPT2046 on VSPI bus (separate from TFT's HSPI)
SPIClass vspi(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
bool g_touchReady = false;

char g_primaryUrl[120];
char g_backupUrl[120];

// ── Colors ────────────────────────────────────────────────────────────────────
#define C_BG     0x0841
#define C_WHITE  0xFFFF
#define C_GRAY   0x7BEF
#define C_DGRAY  0x3186
#define C_CYAN   0x07FF
#define C_YELLOW 0xFFE0
#define C_GREEN  0x27E6
#define C_RED    0xF800
#define C_BLUE   0x041F
#define C_HDR    0x0C52
#define C_DIV    0x2945

// ── Sections ──────────────────────────────────────────────────────────────────
enum Section { HOME = 0, TODAY = 1, WEEK = 2, MONTH = 3 };
Section g_currentSection = HOME;

// ── Layout ────────────────────────────────────────────────────────────────────
#define HDR_H   40
#define TAB_Y   (HDR_H + 2)
#define TAB_H   24
#define CONTENT_Y (TAB_Y + TAB_H + 2)
#define STA_Y   (240 - 13)

// Tab boundaries (4 equal tabs)
#define TAB_W   (320 / 4)

// ── State ─────────────────────────────────────────────────────────────────────
struct CCPeriod {
  int64_t inputTokens  = 0;
  int64_t cacheRead    = 0;
  int64_t cacheWrite   = 0;
  int64_t outputTokens = 0;
  int64_t totalTokens  = 0;
  int32_t sessions     = 0;
  int32_t messages     = 0;
};

struct DashData {
  CCPeriod day, week, month;
  bool     valid    = false;
  bool     hasCC    = false;
  String   srcLabel = "";
};

DashData      g_data;
unsigned long g_lastFetch = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────
String fmtTokens(int64_t n) {
  char buf[16];
  if (n >= 1000000000LL) { dtostrf(n/1e9, 5, 2, buf); return String(buf)+"B"; }
  if (n >= 1000000LL)    { dtostrf(n/1e6, 5, 2, buf); return String(buf)+"M"; }
  if (n >= 1000LL)       { snprintf(buf,16,"%lldK",(long long)(n/1000)); return String(buf); }
  snprintf(buf,16,"%lld",(long long)n); return String(buf);
}

// ── Drawing ───────────────────────────────────────────────────────────────────
void drawHeader(bool wifiOk) {
  tft.fillRect(0, 0, 320, HDR_H, C_HDR);
  tft.drawLine(0, HDR_H-1, 320, HDR_H-1, C_BLUE);
  tft.setTextFont(4); tft.setTextColor(C_WHITE, C_HDR);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("CLAUDE CODE", 10, HDR_H/2);
  if (g_data.day.sessions > 0) {
    char sb[20]; snprintf(sb, 20, "%d active", g_data.day.sessions);
    tft.setTextFont(2); tft.setTextColor(C_GREEN, C_HDR);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(sb, 295, HDR_H/2);
  }
  tft.fillCircle(312, HDR_H/2, 5, wifiOk ? C_GREEN : C_RED);
}

void drawTabs() {
  tft.fillRect(0, TAB_Y, 320, TAB_H, C_BG);
  const char* tabs[] = {"HOME", "TODAY", "WEEK", "MONTH"};
  uint16_t colors[] = {C_BLUE, C_CYAN, C_GREEN, C_YELLOW};

  for (int i = 0; i < 4; i++) {
    int x = i * TAB_W;
    uint16_t bgCol = (i == g_currentSection) ? colors[i] : C_DGRAY;
    uint16_t textCol = (i == g_currentSection) ? C_BG : C_GRAY;

    tft.fillRect(x+1, TAB_Y+1, TAB_W-2, TAB_H-2, bgCol);
    tft.setTextFont(2); tft.setTextColor(textCol, bgCol);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(tabs[i], x + TAB_W/2, TAB_Y + TAB_H/2);
  }
  tft.drawLine(0, TAB_Y + TAB_H, 320, TAB_Y + TAB_H, C_DIV);
}

void drawLabelValue(int y, const char* label, int64_t value) {
  tft.setTextFont(1); tft.setTextColor(C_DGRAY, C_BG);
  tft.setTextDatum(ML_DATUM); tft.drawString(label, 8, y+5);
  tft.setTextFont(2); tft.setTextColor(C_WHITE, C_BG);
  tft.setTextDatum(MR_DATUM); tft.drawString(fmtTokens(value), 312, y+5);
}

void drawHomeScreen() {
  tft.fillRect(0, CONTENT_Y, 320, STA_Y - CONTENT_Y, C_BG);

  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  // Digital clock (HH:MM, large)
  tft.setTextFont(7);        // Large font
  tft.setTextSize(2);        // 2x scale
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextDatum(MC_DATUM);
  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d",
           timeinfo->tm_hour, timeinfo->tm_min);
  tft.drawString(timeStr, 160, CONTENT_Y + 50);
  tft.setTextSize(1);  // Reset

  // Day of week
  tft.setTextFont(2); tft.setTextSize(2);
  tft.setTextColor(C_WHITE, C_BG);
  const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  tft.drawString(days[timeinfo->tm_wday], 160, CONTENT_Y + 105);
  tft.setTextSize(1);

  // Date (D/M/YYYY)
  tft.setTextFont(2); tft.setTextColor(C_GRAY, C_BG);
  char dateStr[20];
  snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
           timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
  tft.drawString(dateStr, 160, CONTENT_Y + 135);

  // Status (source shown at bottom of screen)
}

void drawSection(int y, const CCPeriod& period) {
  tft.fillRect(0, CONTENT_Y, 320, STA_Y - CONTENT_Y, C_BG);

  drawLabelValue(y, "Input",    period.inputTokens);  y += 14;
  drawLabelValue(y, "Output",   period.outputTokens); y += 14;
  drawLabelValue(y, "Cache R",  period.cacheRead);    y += 14;
  drawLabelValue(y, "Cache W",  period.cacheWrite);   y += 14;
  drawLabelValue(y, "TOTAL",    period.totalTokens);  y += 14;

  tft.setTextFont(1); tft.setTextColor(C_DGRAY, C_BG);
  tft.setTextDatum(ML_DATUM);
  char sb[32]; snprintf(sb, 32, "Sessions %d | Messages %d", period.sessions, period.messages);
  tft.drawString(sb, 8, y);
}

void drawStatus(int secondsLeft) {
  tft.fillRect(0, STA_Y, 320, 240-STA_Y, C_BG);
  tft.setTextFont(1); tft.setTextDatum(ML_DATUM);
  if (g_data.valid) {
    tft.setTextColor(C_DGRAY, C_BG);
    char buf[64];
    snprintf(buf, 64, "%s | %s", WiFi.SSID().c_str(), g_data.srcLabel.c_str());
    tft.drawString(buf, 4, STA_Y+6);
  } else {
    tft.setTextColor(C_RED, C_BG);
    tft.drawString("Server unreachable", 4, STA_Y+6);
  }
  char cb[16]; snprintf(cb, 16, "Next %ds", secondsLeft);
  tft.setTextColor(C_DGRAY, C_BG); tft.setTextDatum(MR_DATUM);
  tft.drawString(cb, 316, STA_Y+6);
}

void renderAll() {
  if (g_currentSection == HOME) {
    drawHomeScreen();
    return;
  }

  if (!g_data.valid) {
    tft.fillRect(0, CONTENT_Y, 320, STA_Y - CONTENT_Y, C_BG);
    tft.setTextFont(4); tft.setTextColor(C_RED, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Server unreachable", 160, (CONTENT_Y + STA_Y) / 2);
    return;
  }

  CCPeriod* periods[] = {&g_data.day, &g_data.week, &g_data.month};
  drawSection(CONTENT_Y + 6, *periods[g_currentSection - 1]);
}

// ── Touch Debug ───────────────────────────────────────────────────────────────
void touchDebug() {
  tft.fillScreen(C_BG);
  tft.setTextFont(2); tft.setTextColor(C_CYAN, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch Debug (15s)", 160, 20);
  tft.setTextFont(1); tft.setTextColor(C_WHITE, C_BG);
  tft.setTextDatum(ML_DATUM);

  unsigned long start = millis();

  while (millis() - start < 15000) {
    tft.fillRect(0, 40, 320, 180, C_BG);

    // Try ts.touched()
    bool touched = ts.touched();
    tft.setTextDatum(ML_DATUM);
    tft.drawString("ts.touched(): " + String(touched ? "YES" : "NO"), 10, 50);

    if (touched) {
      TS_Point p = ts.getPoint();
      tft.drawString("X=" + String(p.x), 10, 65);
      tft.drawString("Y=" + String(p.y), 10, 80);
      tft.drawString("Z=" + String(p.z), 10, 95);
      Serial.print("Point: X="); Serial.print(p.x);
      Serial.print(" Y="); Serial.print(p.y);
      Serial.print(" Z="); Serial.println(p.z);
    }

    delay(200);
  }

  tft.fillScreen(C_BG);
}

// ── Touch ─────────────────────────────────────────────────────────────────────
void checkTouch() {
  if (!g_touchReady) return;

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    Serial.print("Touch: X="); Serial.print(p.x);
    Serial.print(" Y="); Serial.print(p.y);
    Serial.print(" Z="); Serial.println(p.z);

    // Map to display coordinates (may need calibration)
    int x = map(p.x, 200, 3800, 0, 320);
    int y = map(p.y, 200, 3800, 0, 240);

    x = constrain(x, 0, 319);
    y = constrain(y, 0, 239);

    Serial.print("Mapped: X="); Serial.print(x);
    Serial.print(" Y="); Serial.println(y);

    // Check if tap is on tabs
    if (y >= TAB_Y && y < (TAB_Y + TAB_H)) {
      Serial.println(">>> TAP ON TAB AREA!");
      if (x < TAB_W)         { g_currentSection = HOME; }
      else if (x < TAB_W*2)  { g_currentSection = TODAY; }
      else if (x < TAB_W*3)  { g_currentSection = WEEK; }
      else                   { g_currentSection = MONTH; }
      drawTabs();
      renderAll();
    }
    delay(200);  // debounce
  }
}

// ── Network ───────────────────────────────────────────────────────────────────
bool fetchFromUrl(const char* url, const char* label) {
  if (strlen(url) < 8) return false;
  HTTPClient http;
  bool isHttps = strncmp(url, "https", 5) == 0;
  if (isHttps) {
    static WiFiClientSecure sc; sc.setInsecure();
    http.begin(sc, url);
  } else { http.begin(url); }
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, http.getString())) {
      JsonVariant cc = doc["claudeCode"];
      if (!cc.isNull()) {
        g_data.day.inputTokens  = cc["day"]["inputTokens"]  | (int64_t)0;
        g_data.day.cacheRead    = cc["day"]["cacheRead"]    | (int64_t)0;
        g_data.day.cacheWrite   = cc["day"]["cacheWrite"]   | (int64_t)0;
        g_data.day.outputTokens = cc["day"]["outputTokens"] | (int64_t)0;
        g_data.day.totalTokens  = cc["day"]["totalTokens"]  | (int64_t)0;
        g_data.day.sessions     = cc["day"]["sessions"]     | 0;
        g_data.day.messages     = cc["day"]["messages"]     | 0;

        g_data.week.inputTokens  = cc["week"]["inputTokens"]  | (int64_t)0;
        g_data.week.cacheRead    = cc["week"]["cacheRead"]    | (int64_t)0;
        g_data.week.cacheWrite   = cc["week"]["cacheWrite"]   | (int64_t)0;
        g_data.week.outputTokens = cc["week"]["outputTokens"] | (int64_t)0;
        g_data.week.totalTokens  = cc["week"]["totalTokens"]  | (int64_t)0;
        g_data.week.sessions     = cc["week"]["sessions"]     | 0;
        g_data.week.messages     = cc["week"]["messages"]     | 0;

        g_data.month.inputTokens  = cc["month"]["inputTokens"]  | (int64_t)0;
        g_data.month.outputTokens = cc["month"]["outputTokens"] | (int64_t)0;
        g_data.month.totalTokens  = cc["month"]["totalTokens"]  | (int64_t)0;
        g_data.month.sessions     = cc["month"]["sessions"]     | 0;
        g_data.month.messages     = cc["month"]["messages"]     | 0;

        g_data.hasCC    = g_data.month.totalTokens > 0;
      }
      g_data.valid    = true;
      g_data.srcLabel = label;
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

void fetchData() {
  if (WiFi.status() != WL_CONNECTED) { g_data.valid = false; return; }
  if (!fetchFromUrl(g_primaryUrl, "Vercel"))
    if (!fetchFromUrl(g_backupUrl, "Local"))
      g_data.valid = false;
}

// ── Preferences ───────────────────────────────────────────────────────────────
void loadUrls() {
  prefs.begin("dashboard", true);
  String p = prefs.getString("primaryUrl", PRIMARY_API_URL);
  String b = prefs.getString("backupUrl",  BACKUP_API_URL);
  prefs.end();
  strncpy(g_primaryUrl, p.c_str(), sizeof(g_primaryUrl)-1);
  strncpy(g_backupUrl,  b.c_str(), sizeof(g_backupUrl)-1);
}

void saveUrls(const char* p, const char* b) {
  prefs.begin("dashboard", false);
  prefs.putString("primaryUrl", p);
  prefs.putString("backupUrl",  b);
  prefs.end();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL_PIN, OUTPUT); digitalWrite(TFT_BL_PIN, HIGH);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  tft.init(); tft.setRotation(1); tft.fillScreen(C_BG);

  // Init XPT2046 on VSPI
  vspi.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(vspi);
  ts.setRotation(1);
  g_touchReady = true;
  Serial.println("Touch init OK");

  drawHeader(false);
  loadUrls();

  WiFiManagerParameter pPrimary("primary", "Primary URL", g_primaryUrl, 119);
  WiFiManagerParameter pBackup ("backup",  "Backup URL",  g_backupUrl, 119);
  wm.addParameter(&pPrimary);
  wm.addParameter(&pBackup);

  tft.setTextFont(2); tft.setTextColor(C_GRAY, C_BG);
  tft.setTextDatum(MC_DATUM); tft.drawString("Connecting...", 160, 130);

  wm.setAPCallback([](WiFiManager*){});
  wm.setSaveParamsCallback([&](){
    saveUrls(pPrimary.getValue(), pBackup.getValue());
    strncpy(g_primaryUrl, pPrimary.getValue(), sizeof(g_primaryUrl)-1);
    strncpy(g_backupUrl,  pBackup.getValue(),  sizeof(g_backupUrl)-1);
  });
  wm.setConfigPortalTimeout(180);

  bool ok = wm.autoConnect("CYD-Dashboard");
  if (!ok) { delay(3000); ESP.restart(); }

  tft.fillRect(0, HDR_H, 320, 60, C_BG);
  tft.setTextFont(2); tft.setTextColor(C_GREEN, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connected: "+WiFi.SSID(), 160, 130);

  // Sync time via NTP (UTC+7 for Thailand)
  tft.setTextFont(1); tft.setTextColor(C_GRAY, C_BG);
  tft.drawString("Syncing time...", 160, 155);

  Serial.println("\n=== NTP Time Sync Debug ===");
  Serial.printf("WiFi SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
  Serial.printf("Local IP: %s\n", WiFi.localIP().toString().c_str());

  // Set timezone BEFORE configTime (UTC+7)
  // POSIX TZ: UTC-7 means 7 hours ahead (counterintuitive!)
  setenv("TZ", "UTC-7", 1);
  tzset();
  Serial.println("TZ set to: UTC-7 (UTC+7)");

  // Configure NTP with multiple servers + SNTP
  Serial.println("Configuring NTP servers...");

  // Try 3 NTP servers
  configTime(7 * 3600, 0, "time.nist.gov", "time.google.com", "pool.ntp.org");

  Serial.print("Syncing: ");
  time_t now = time(nullptr);
  int retries = 40;  // 8 seconds max

  while (now < 1609459200 && retries-- > 0) {
    delay(200);
    now = time(nullptr);
    Serial.print(".");
  }
  Serial.println();

  if (now >= 1609459200) {
    struct tm* tm = localtime(&now);
    Serial.printf("✓ SUCCESS: %04d-%02d-%02d %02d:%02d:%02d (UTC+7)\n",
                  tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
                  tm->tm_hour, tm->tm_min, tm->tm_sec);
    tft.setTextColor(C_GREEN, C_BG);
    tft.drawString("Time synced!", 160, 155);
  } else {
    Serial.println("✗ FAILED: NTP unavailable");
    Serial.println("Fallback: Using manual time 2026-06-01 12:00:00");

    // Manual fallback time (2026-06-01 12:00:00)
    struct tm timeinfo = {0, 0, 12, 1, 5, 126, 0, 0, 0};  // sec,min,hour,mday,mon(0-11),year-1900
    time_t t = mktime(&timeinfo);
    struct timeval tv = {t, 0};
    settimeofday(&tv, nullptr);

    tft.setTextColor(C_YELLOW, C_BG);
    tft.drawString("Using fallback time", 160, 155);
  }

  delay(1000);

  drawHeader(true);
  drawTabs();
  fetchData();
  renderAll();
  drawStatus(UPDATE_INTERVAL/1000);
  g_lastFetch = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static unsigned long bootHoldStart = 0;

void loop() {
  checkTouch();

  // BOOT button: 1s=calibrate touch, 3s=reset WiFi
  if (digitalRead(BOOT_PIN) == LOW) {
    if (bootHoldStart == 0) bootHoldStart = millis();
    unsigned long held = millis() - bootHoldStart;

    if (held >= 3000) {
      tft.fillScreen(C_BG); drawHeader(false);
      tft.setTextFont(4); tft.setTextColor(C_RED, C_BG);
      tft.setTextDatum(MC_DATUM); tft.drawString("Resetting...", 160, 120);
      wm.resetSettings();
      prefs.begin("dashboard",false); prefs.clear(); prefs.end();
      delay(2000); ESP.restart();
    }
  } else {
    unsigned long held = (bootHoldStart > 0) ? (millis() - bootHoldStart) : 0;
    if (held > 800 && held < 2000) {
      touchDebug();
      drawHeader(true); drawTabs(); renderAll();
    }
    bootHoldStart = 0;
  }

  // Fetch every 60s
  unsigned long elapsed = millis() - g_lastFetch;
  int secondsLeft = (elapsed >= UPDATE_INTERVAL) ? 0
                    : (int)((UPDATE_INTERVAL-elapsed)/1000UL);

  if (elapsed >= UPDATE_INTERVAL) {
    if (WiFi.status() != WL_CONNECTED) wm.autoConnect("CYD-Dashboard");
    fetchData();
    drawTabs();
    renderAll();
    drawHeader(WiFi.status() == WL_CONNECTED);
    g_lastFetch = millis();
    secondsLeft = UPDATE_INTERVAL/1000;
  }

  // Update HOME tab only when minute changes (reduce flicker), status every 10s
  static int prevMin = -1;
  static int prevStatusSec = -1;

  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  int currentMin = timeinfo->tm_min;

  if (g_currentSection == HOME && currentMin != prevMin) {
    renderAll();
    prevMin = currentMin;
  }

  if (secondsLeft != prevStatusSec) {
    drawStatus(secondsLeft);
    prevStatusSec = secondsLeft;
  }

  delay(200);
}
