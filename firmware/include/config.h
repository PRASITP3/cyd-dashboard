#pragma once

// ── Default URLs (ตั้งค่าจริงผ่าน WiFiManager portal) ──────────────────────────
#define PRIMARY_API_URL "https://cyd-dashboard.vercel.app/api/dashboard"
#define BACKUP_API_URL  "http://192.168.0.102:3001/api/dashboard"

// ── Refresh interval ──────────────────────────────────────────────────────────
#define UPDATE_INTERVAL 60000UL

// ── Stock watchlist firmware (env:stocks) ─────────────────────────────────────
#define STOCKS_API_URL         "https://cyd-dashboard.vercel.app/api/stocks"
#define STOCKS_BACKUP_URL      "http://192.168.0.102:3001/api/stocks"
#define STOCKS_UPDATE_INTERVAL 120000UL   // 2 min
#define STOCKS_PAGE_MS         8000UL     // auto-flip page every 8s

// ── NTP Server ────────────────────────────────────────────────────────────────
#define NTP_SERVER "pool.ntp.org"
#define TZ_OFFSET (7 * 3600)  // Thailand UTC+7

// ── Hardware ──────────────────────────────────────────────────────────────────
#define TFT_BL_PIN  21
#define BOOT_PIN     0

// ── WiFiManager AP name ───────────────────────────────────────────────────────
#define AP_NAME "CYD-Dashboard"

// Touch Screen (XPT2046) — VSPI Bus (separate from TFT HSPI)
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define TOUCH_SCLK 25
#define TOUCH_CS   33
#define TOUCH_IRQ  36
