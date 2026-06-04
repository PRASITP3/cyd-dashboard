'use strict';
// Thin REST wrapper around the Supabase cc_usage table (no SDK dependency).
//   - getCcFromSupabase(): used by the Vercel dashboard to READ the snapshot
//                          (anon key, SELECT allowed by RLS).
//   - putCcToSupabase():   used by the local uploader to WRITE the snapshot
//                          (service_role key, bypasses RLS).

const SUPABASE_URL = process.env.SUPABASE_URL || '';
const ANON_KEY     = process.env.SUPABASE_ANON_KEY || '';
const SERVICE_KEY  = process.env.SUPABASE_SERVICE_KEY || '';

const TABLE = 'cc_usage';

// Short read cache so a burst of dashboard hits doesn't hammer Supabase.
let _cache = null, _cacheAt = 0;
const READ_TTL = 60 * 1000;

async function getCcFromSupabase({ useCache = true } = {}) {
  if (!SUPABASE_URL || !ANON_KEY) return null;
  if (useCache && _cache && Date.now() - _cacheAt < READ_TTL) return _cache;

  try {
    const url = `${SUPABASE_URL}/rest/v1/${TABLE}?id=eq.1&select=data,updated_at`;
    const resp = await fetch(url, {
      headers: { apikey: ANON_KEY, Authorization: `Bearer ${ANON_KEY}` },
    });
    if (!resp.ok) { console.error('[supabase] read', resp.status); return null; }
    const rows = await resp.json();
    const data = rows?.[0]?.data ?? null;
    if (data) { _cache = data; _cacheAt = Date.now(); }
    return data;
  } catch (err) {
    console.error('[supabase] read error', err.message);
    return null;
  }
}

async function putCcToSupabase(data) {
  if (!SUPABASE_URL || !SERVICE_KEY) {
    throw new Error('SUPABASE_URL and SUPABASE_SERVICE_KEY must be set to upload');
  }
  const url = `${SUPABASE_URL}/rest/v1/${TABLE}?on_conflict=id`;
  const resp = await fetch(url, {
    method: 'POST',
    headers: {
      apikey: SERVICE_KEY,
      Authorization: `Bearer ${SERVICE_KEY}`,
      'Content-Type': 'application/json',
      Prefer: 'resolution=merge-duplicates,return=minimal',
    },
    body: JSON.stringify({ id: 1, data, updated_at: new Date().toISOString() }),
  });
  if (!resp.ok) {
    const txt = await resp.text().catch(() => '');
    throw new Error(`Supabase upsert failed ${resp.status}: ${txt}`);
  }
}

// ── Stock watchlist ────────────────────────────────────────────────────────
// Returns an ordered array of { symbol, display } from the stock_watchlist
// table. Falls back to the STOCK_SYMBOLS env var (comma-separated) when the
// table is empty/unreachable, so the endpoint still works without Supabase.
let _wlCache = null, _wlCacheAt = 0;
const WL_TTL = 5 * 60 * 1000;

function watchlistFromEnv() {
  return (process.env.STOCK_SYMBOLS || '')
    .split(',').map(s => s.trim()).filter(Boolean)
    .map(symbol => ({ symbol, display: symbol.replace(/\.BK$/i, '') }));
}

async function getWatchlist({ useCache = true } = {}) {
  if (useCache && _wlCache && Date.now() - _wlCacheAt < WL_TTL) return _wlCache;

  let rows = null;
  if (SUPABASE_URL && ANON_KEY) {
    try {
      const url = `${SUPABASE_URL}/rest/v1/stock_watchlist`
        + `?enabled=eq.true&select=symbol,display&order=position.asc`;
      const resp = await fetch(url, {
        headers: { apikey: ANON_KEY, Authorization: `Bearer ${ANON_KEY}` },
      });
      if (resp.ok) rows = await resp.json();
      else console.error('[supabase] watchlist', resp.status);
    } catch (err) {
      console.error('[supabase] watchlist error', err.message);
    }
  }

  const result = (rows && rows.length) ? rows : watchlistFromEnv();
  _wlCache = result; _wlCacheAt = Date.now();
  return result;
}

// ── Portfolio (holdings with cost basis) ─────────────────────────────────────
// Reads holdings from the `portfolio` table; falls back to the built-in default
// (the screenshot holdings) so /api/stocks works even before the table exists.
// Returns [{ symbol, display, vol, avg }] ordered for display.
const DEFAULT_PORTFOLIO = [
  { symbol: 'AMZN80.BK',    vol: 122500, avg: 2.06 },
  { symbol: 'GOOG80.BK',    vol: 58299,  avg: 3.92 },
  { symbol: 'ICHI.BK',      vol: 19200,  avg: 13.09 },
  { symbol: 'LVMH01.BK',    vol: 7100,   avg: 14.22 },
  { symbol: 'SHOP03.BK',    vol: 40000,  avg: 2.52 },
  { symbol: 'SMIC23.BK',    vol: 69400,  avg: 3.61 },
  { symbol: 'TENCENT80.BK', vol: 13000,  avg: 19.31 },
];

let _pfCache = null, _pfCacheAt = 0;
const PF_TTL = 5 * 60 * 1000;

async function getPortfolio({ useCache = true } = {}) {
  if (useCache && _pfCache && Date.now() - _pfCacheAt < PF_TTL) return _pfCache;

  let rows = null;
  if (SUPABASE_URL && ANON_KEY) {
    try {
      const url = `${SUPABASE_URL}/rest/v1/portfolio`
        + `?enabled=eq.true&select=symbol,display,vol,avg_cost&order=position.asc`;
      const resp = await fetch(url, {
        headers: { apikey: ANON_KEY, Authorization: `Bearer ${ANON_KEY}` },
      });
      if (resp.ok) rows = await resp.json();
      else console.error('[supabase] portfolio', resp.status);
    } catch (err) {
      console.error('[supabase] portfolio error', err.message);
    }
  }

  const result = (rows && rows.length)
    ? rows.map(r => ({ symbol: r.symbol, display: r.display, vol: Number(r.vol) || 0, avg: r.avg_cost == null ? null : Number(r.avg_cost) }))
    : DEFAULT_PORTFOLIO.map(h => ({ ...h, display: null }));

  _pfCache = result; _pfCacheAt = Date.now();
  return result;
}

module.exports = { getCcFromSupabase, putCcToSupabase, getWatchlist, getPortfolio };
