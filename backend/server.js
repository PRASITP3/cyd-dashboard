'use strict';
require('dotenv').config();

const express = require('express');
const { DatabaseSync } = require('node:sqlite');
const os   = require('os');
const path = require('path');
const fs   = require('fs');
const { readClaudeCodeUsage } = require('./lib/cc-usage');
const { getCcFromSupabase, getPortfolio } = require('./lib/supabase');
const { fetchQuotes }         = require('./lib/yahoo');

// Claude Code usage: read local ~/.claude logs when available (local server),
// otherwise fall back to the snapshot in Supabase (Vercel / any cloud host).
async function getClaudeCode() {
  return readClaudeCodeUsage() || await getCcFromSupabase();
}

const app = express();
app.use(express.json());

// ── Database (optional — skipped on Vercel/cloud) ─────────────────────────────
let db = null;
try {
  const { DatabaseSync } = require('node:sqlite');
  db = new DatabaseSync(path.join(__dirname, 'usage.db'));
  db.exec(`
    CREATE TABLE IF NOT EXISTS api_calls (
      id            INTEGER  PRIMARY KEY AUTOINCREMENT,
      model         TEXT     NOT NULL DEFAULT 'unknown',
      input_tokens  INTEGER  NOT NULL DEFAULT 0,
      output_tokens INTEGER  NOT NULL DEFAULT 0,
      created_at    TEXT     DEFAULT (strftime('%Y-%m-%dT%H:%M:%S', 'now', 'localtime'))
    );
  `);
} catch (_) { /* running in cloud — no local DB */ }

// ── Cache ─────────────────────────────────────────────────────────────────────
let _cache   = null;
let _cacheAt = 0;
const CACHE_TTL = 5 * 60 * 1000;

// ── Anthropic helpers ─────────────────────────────────────────────────────────

function sumBuckets(data, fromMs) {
  let input = 0, output = 0;
  for (const bucket of (data || [])) {
    if (new Date(bucket.starting_at).getTime() < fromMs) continue;
    for (const r of (bucket.results || [])) {
      input  += (r.uncached_input_tokens                     || 0)
              + (r.cache_read_input_tokens                   || 0)
              + (r.cache_creation?.ephemeral_1h_input_tokens || 0)
              + (r.cache_creation?.ephemeral_5m_input_tokens || 0);
      output += r.output_tokens || 0;
    }
  }
  return { inputTokens: input, outputTokens: output, totalTokens: input + output };
}

async function fetchAllBuckets(startingAt, endingAt, adminKey) {
  const allBuckets = [];
  let nextPage = null;
  do {
    const params = new URLSearchParams({
      starting_at:  startingAt,
      ending_at:    endingAt,
      bucket_width: '1d',
      limit:        31,
      ...(nextPage ? { page: nextPage } : {}),
    });
    const resp = await fetch(
      `https://api.anthropic.com/v1/organizations/usage_report/messages?${params}`,
      { headers: { 'x-api-key': adminKey, 'anthropic-version': '2023-06-01' } }
    );
    if (!resp.ok) return null;
    const json = await resp.json();
    allBuckets.push(...(json.data || []));
    nextPage = json.has_more ? json.next_page : null;
  } while (nextPage);
  return allBuckets;
}

async function fetchDashboardData() {
  const adminKey = process.env.ANTHROPIC_ADMIN_KEY;
  if (!adminKey) return null;

  const cacheKey = new Date().toDateString();
  if (_cache?.key === cacheKey && Date.now() - _cacheAt < CACHE_TTL) return _cache.data;

  const now        = new Date();
  const monthStart = new Date(now.getFullYear(), now.getMonth(), 1).toISOString();
  const weekStart  = new Date(now - 7 * 24 * 3600 * 1000).toISOString();
  const todayStart = new Date(now); todayStart.setHours(0, 0, 0, 0);

  const buckets = await fetchAllBuckets(monthStart, now.toISOString(), adminKey);
  if (!buckets) return null;

  const weekMs  = new Date(weekStart).getTime();
  const monthMs = new Date(monthStart).getTime();
  let allBuckets = buckets;
  if (weekMs < monthMs) {
    const extra = await fetchAllBuckets(weekStart, monthStart, adminKey);
    if (extra) allBuckets = [...extra, ...buckets];
  }

  const month = sumBuckets(buckets,    monthMs);
  const week  = sumBuckets(allBuckets, weekMs);
  const day   = sumBuckets(buckets,    todayStart.getTime());

  const data = { month, week, day };
  _cache = { key: cacheKey, data };
  _cacheAt = Date.now();
  return data;
}

// ── Routes ────────────────────────────────────────────────────────────────────

app.get('/api/dashboard', async (req, res) => {
  const dailyLimit  = Number(process.env.DAILY_TOKEN_LIMIT)  || 5_000_000;
  const weeklyLimit = Number(process.env.WEEKLY_TOKEN_LIMIT) || 25_000_000;

  const d = await fetchDashboardData();

  const cc = await getClaudeCode();
  console.log('[/api/dashboard] cc:', !!cc, cc ? Object.keys(cc) : 'null');

  if (d) {
    const dayPct  = Math.min(100, Math.round(d.day.totalTokens  / dailyLimit  * 100));
    const weekPct = Math.min(100, Math.round(d.week.totalTokens / weeklyLimit * 100));
    return res.json({
      month:      d.month,
      week:       { ...d.week,  pct: weekPct,  limit: weeklyLimit },
      day:        { ...d.day,   pct: dayPct,   limit: dailyLimit  },
      claudeCode: cc,
      source:     'anthropic',
      updatedAt:  new Date().toISOString(),
    });
  }

  // Fallback: local DB
  const ld = db.prepare(`SELECT COALESCE(SUM(input_tokens),0) AS i, COALESCE(SUM(output_tokens),0) AS o FROM api_calls WHERE strftime('%Y-%m',created_at)=strftime('%Y-%m',datetime('now','localtime'))`).get();
  const lw = db.prepare(`SELECT COALESCE(SUM(input_tokens+output_tokens),0) AS t FROM api_calls WHERE created_at>=datetime('now','-7 days','localtime')`).get().t;
  const ly = db.prepare(`SELECT COALESCE(SUM(input_tokens+output_tokens),0) AS t FROM api_calls WHERE date(created_at)=date('now','localtime')`).get().t;
  res.json({
    month:     { inputTokens: ld.i, outputTokens: ld.o, totalTokens: ld.i + ld.o },
    week:      { totalTokens: lw, pct: Math.min(100, Math.round(lw / weeklyLimit * 100)), limit: weeklyLimit },
    day:       { totalTokens: ly, pct: Math.min(100, Math.round(ly / dailyLimit  * 100)), limit: dailyLimit  },
    claudeCode: cc,
    source:    'local',
    updatedAt: new Date().toISOString(),
  });
});

/** GET /api/claudecode — Claude Code local session usage */
app.get('/api/claudecode', async (req, res) => {
  const cc = await getClaudeCode();
  if (!cc) return res.status(503).json({ error: 'Claude Code data not found' });
  res.json({ ...cc, source: 'claude-code', updatedAt: new Date().toISOString() });
});

/** GET /api/stocks — portfolio with live market price + auto %U.PL (Yahoo) */
app.get('/api/stocks', async (req, res) => {
  const pf = await getPortfolio();
  const quotes = await fetchQuotes(pf.map(p => p.symbol));
  const mktBy = {};
  for (const q of quotes) mktBy[q.sym] = q.last;   // q.sym is already stripped of .BK
  const r2 = (n) => n == null ? null : Math.round(n * 100) / 100;

  let cost = 0, value = 0;
  const holdings = pf.map(p => {
    const sym = (p.display || p.symbol).replace(/\.BK$/i, '');
    const mkt = mktBy[sym] ?? null;
    const avg = p.avg ?? null;
    const upl = (mkt != null && avg) ? Math.round(((mkt - avg) / avg) * 10000) / 100 : null;
    if (mkt != null && avg != null) { cost += p.vol * avg; value += p.vol * mkt; }
    return { sym, vol: p.vol, avg: r2(avg), mkt: r2(mkt), upl };
  });

  // Always sort by %U.PL high → low; holdings with no market price (N/A) last.
  holdings.sort((a, b) => (b.upl ?? -Infinity) - (a.upl ?? -Infinity));

  const totalUpl = value - cost;
  const total = {
    cost:  r2(cost),
    value: r2(value),
    upl:   r2(totalUpl),
    pct:   cost ? Math.round((totalUpl / cost) * 10000) / 100 : null,
  };

  res.json({ holdings, total, count: holdings.length, source: 'yahoo', updatedAt: new Date().toISOString() });
});

app.get('/api/usage', async (req, res) => {
  const allowed = ['today', 'week', 'month', 'all'];
  const period  = allowed.includes(req.query.period) ? req.query.period : 'month';
  const d = await fetchDashboardData();
  if (d) {
    const src = period === 'today' ? d.day : period === 'week' ? d.week : d.month;
    return res.json({ ...src, period, source: 'anthropic', updatedAt: new Date().toISOString() });
  }
  res.json({ inputTokens: 0, outputTokens: 0, totalTokens: 0, period, source: 'local', updatedAt: new Date().toISOString() });
});

app.post('/api/usage/log', (req, res) => {
  const { model = 'unknown', inputTokens = 0, outputTokens = 0 } = req.body;
  if (db) db.prepare('INSERT INTO api_calls (model, input_tokens, output_tokens) VALUES (?, ?, ?)').run(String(model), Number(inputTokens), Number(outputTokens));
  res.json({ ok: true });
});

app.post('/api/claude/messages', async (req, res) => {
  const apiKey = process.env.ANTHROPIC_API_KEY;
  if (!apiKey) return res.status(500).json({ error: 'ANTHROPIC_API_KEY not set' });
  try {
    const upstream = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST',
      headers: { 'x-api-key': apiKey, 'anthropic-version': '2023-06-01', 'content-type': 'application/json' },
      body: JSON.stringify(req.body),
    });
    const data = await upstream.json();
    if (upstream.ok && data.usage)
      db.prepare('INSERT INTO api_calls (model, input_tokens, output_tokens) VALUES (?, ?, ?)')
        .run(data.model || 'unknown', data.usage.input_tokens || 0, data.usage.output_tokens || 0);
    res.status(upstream.status).json(data);
  } catch (err) { res.status(502).json({ error: err.message }); }
});

app.get('/api/history', (req, res) => {
  if (!db) return res.json([]);
  const limit = Math.min(Number(req.query.limit) || 20, 200);
  res.json(db.prepare('SELECT id, model, input_tokens, output_tokens, created_at FROM api_calls ORDER BY id DESC LIMIT ?').all(limit));
});

app.delete('/api/usage', (_req, res) => {
  if (db) db.prepare('DELETE FROM api_calls').run();
  res.json({ ok: true });
});

app.get('/health', (_req, res) => res.json({ ok: true, anthropicAdminKey: !!process.env.ANTHROPIC_ADMIN_KEY, timestamp: new Date().toISOString() }));

app.get('/api/test', async (req, res) => {
  const local = readClaudeCodeUsage();
  const cc = local || await getCcFromSupabase();
  res.json({ cc, source: local ? 'local' : (cc ? 'supabase' : 'none'),
             ccNull: cc == null, ccKeys: cc ? Object.keys(cc) : null });
});

// ── Start (local) / Export (Vercel) ──────────────────────────────────────────
if (require.main === module) {
  const PORT = Number(process.env.PORT) || 3000;
  app.listen(PORT, '0.0.0.0', () => {
    const iface   = Object.values(os.networkInterfaces()).flat().find(i => i.family === 'IPv4' && !i.internal);
    const localIp = iface ? iface.address : 'YOUR_PC_IP';
    console.log(`\nClaude Token Dashboard — http://${localIp}:${PORT}\n`);
  });
}

module.exports = app;
