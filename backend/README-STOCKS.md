# Portfolio screen (CYD env:stocks)

A second firmware for the same CYD board. It stores your holdings' cost basis
(vol + avg) and shows a Maybank-style table with the **live** market price and
**auto-computed %U.PL**:  `%U.PL = (market - avg) / avg * 100`.

```
Yahoo Finance ──/api/stocks──► Vercel ──poll every 2 min──► ESP32 (env:stocks)
   chart API     fetch mkt + compute %U.PL + totals          Symbol | Avg | Mkt | %U.PL + Total
holdings (vol, avg): Supabase `portfolio`  (fallback: built-in default)
```

## 1. Holdings source
- **Supabase table** `portfolio` (edit without redeploy): run
  `migrations/003_portfolio.sql` in the time-capital Studio, then edit rows
  (`position`, `symbol` like `GOOG80.BK`, `vol`, `avg_cost`, optional `display`).
- **Built-in default**: if the table is empty/absent, the backend uses the
  screenshot holdings baked into `lib/supabase.js` (`DEFAULT_PORTFOLIO`), so the
  endpoint works immediately. (003 supersedes the old 002 watchlist.)

## 2. Backend (deployed)
`GET /api/stocks` →
```json
{ "holdings": [ { "sym":"GOOG80", "vol":58299, "avg":3.92, "mkt":5.85, "upl":49.23 } ],
  "total": { "cost":..., "value":..., "upl":114771.07, "pct":7.99 },
  "updatedAt":"..." }
```
- Thai SET/DR symbols use the `.BK` suffix. `mkt` = Yahoo `regularMarketPrice`.
- A holding whose market is unavailable (e.g. delisted) returns `mkt:null` → shown as `N/A`.
- Test: `curl https://cyd-dashboard.vercel.app/api/stocks`

## 3. Flash the firmware (VS Code PlatformIO)
Two firmwares share `firmware/`, selected by environment:
- `env:cyd`    → Claude token dashboard (`src/main.cpp`) — unchanged
- `env:stocks` → this portfolio (`src/stocks_main.cpp`)

VS Code: PlatformIO sidebar → **esp32dev (stocks)** → **Upload**
(CLI: `pio run -e stocks -t upload`). First boot opens a `CYD-Stocks` WiFi portal.
- Touch top half = prev page, bottom half = next page; auto-flips every 8 s (only if > 10 rows).
- Hold BOOT 3 s = reset WiFi + URLs.
- Bottom **Total** line = portfolio unrealized P/L (value in K + %).

## Notes
- %U.PL uses the stored `avg`. Brokers fold trading fees into their avg, so values
  can differ by a few hundredths — store a more precise `avg_cost` to match exactly.
- `MICRON80` has vol 0 but still shows its avg-vs-market %; it contributes 0 to totals.
- Refresh is 2 min (`STOCKS_UPDATE_INTERVAL` in `firmware/include/config.h`).
- Per-stock logos from the app are intentionally omitted (too heavy for the MCU).
