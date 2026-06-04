'use strict';
// Fetches quotes from Yahoo Finance's public chart endpoint (no auth needed).
// Thai SET stocks and DRs use the ".BK" suffix, e.g. GOOG80.BK, AP.BK.
// We use meta.regularMarketPrice and meta.chartPreviousClose to compute change
// (meta.previousClose is often null for these symbols).

const CHART = 'https://query1.finance.yahoo.com/v8/finance/chart';
// Yahoo blocks Node's default UA; a browser-ish UA is required.
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36';

let _cache = null, _cacheAt = 0, _cacheKey = '';
const CACHE_TTL = 30 * 1000;

function displayFor(symbol) {
  return symbol.replace(/\.BK$/i, '');
}

async function fetchOne(symbol) {
  try {
    const url = `${CHART}/${encodeURIComponent(symbol)}?interval=1d&range=1d`;
    const resp = await fetch(url, { headers: { 'User-Agent': UA } });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const json = await resp.json();
    const meta = json?.chart?.result?.[0]?.meta;
    if (!meta) throw new Error('no meta');

    const last = meta.regularMarketPrice ?? null;
    const prev = meta.chartPreviousClose ?? meta.previousClose ?? null;
    const chg  = (last != null && prev != null) ? last - prev : null;
    const pct  = (chg != null && prev) ? (chg / prev) * 100 : null;
    const r2 = (n) => n == null ? null : Math.round(n * 100) / 100;

    return {
      sym:  displayFor(symbol),
      last: r2(last),
      chg:  r2(chg),
      pct:  r2(pct),
      time: meta.regularMarketTime ? meta.regularMarketTime * 1000 : null,
    };
  } catch (err) {
    console.error(`[yahoo] ${symbol}: ${err.message}`);
    return { sym: displayFor(symbol), last: null, chg: null, pct: null, time: null };
  }
}

async function fetchQuotes(symbols) {
  const list = (symbols || []).map(s => s.trim()).filter(Boolean);
  if (!list.length) return [];

  const key = list.join(',');
  if (_cache && _cacheKey === key && Date.now() - _cacheAt < CACHE_TTL) return _cache;

  const quotes = await Promise.all(list.map(fetchOne));
  _cache = quotes; _cacheAt = Date.now(); _cacheKey = key;
  return quotes;
}

module.exports = { fetchQuotes };
