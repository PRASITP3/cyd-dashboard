'use strict';
// Reads Claude Code session usage from the local ~/.claude/projects/*.jsonl logs.
// This ONLY works on a machine that actually runs Claude Code — on Vercel the
// directory does not exist and this returns null (the dashboard then falls back
// to the snapshot stored in Supabase).

const os   = require('os');
const path = require('path');
const fs   = require('fs');

const CACHE_TTL = 5 * 60 * 1000;
let _cache = null, _cacheAt = 0;

// Claude Sonnet pricing ($/M tokens) — used for API-equivalent cost
const PRICE = { input: 3.00, cacheRead: 0.30, cacheWrite: 3.75, output: 15.00 };

function calcCost(p) {
  return (p.input * PRICE.input + p.cacheRead * PRICE.cacheRead +
          p.cacheWrite * PRICE.cacheWrite + p.output * PRICE.output) / 1_000_000;
}

function readClaudeCodeUsage({ useCache = true } = {}) {
  if (useCache && _cache && Date.now() - _cacheAt < CACHE_TTL) return _cache;

  const claudeDir = path.join(os.homedir(), '.claude', 'projects');
  if (!fs.existsSync(claudeDir)) return null;

  const now     = new Date();
  const monthMs = new Date(now.getFullYear(), now.getMonth(), 1).getTime();
  const weekMs  = now.getTime() - 7 * 24 * 3600 * 1000;
  const todayMs = new Date(now).setHours(0, 0, 0, 0);

  const empty = () => ({ input:0, cacheRead:0, cacheWrite:0, output:0, sessions: new Set(), messages:0 });
  let month = empty(), week = empty(), day = empty();

  try {
    const projectDirs = fs.readdirSync(claudeDir, { withFileTypes: true })
      .filter(d => d.isDirectory())
      .map(d => path.join(claudeDir, d.name));

    for (const dir of projectDirs) {
      for (const file of fs.readdirSync(dir).filter(f => f.endsWith('.jsonl'))) {
        const lines = fs.readFileSync(path.join(dir, file), 'utf8').split('\n');
        for (const line of lines) {
          if (!line.trim()) continue;
          try {
            const obj = JSON.parse(line);
            if (obj.type !== 'assistant') continue;
            const usage = obj.message?.usage;
            if (!usage) continue;

            const ts  = new Date(obj.timestamp).getTime();
            const sid = obj.sessionId || '';
            const inp = usage.input_tokens || 0;
            const cr  = usage.cache_read_input_tokens || 0;
            const cw  = (usage.cache_creation_input_tokens || 0)
                      + (usage.cache_creation?.ephemeral_1h_input_tokens || 0)
                      + (usage.cache_creation?.ephemeral_5m_input_tokens || 0);
            const out = usage.output_tokens || 0;

            const add = (p) => {
              p.input += inp; p.cacheRead += cr; p.cacheWrite += cw;
              p.output += out; p.messages++; if (sid) p.sessions.add(sid);
            };
            if (ts >= monthMs) add(month);
            if (ts >= weekMs)  add(week);
            if (ts >= todayMs) add(day);
          } catch (_) {}
        }
      }
    }
  } catch (err) { console.error('[Claude Code]', err.message); return null; }

  const fmt = (p) => ({
    inputTokens:  p.input,
    cacheRead:    p.cacheRead,
    cacheWrite:   p.cacheWrite,
    outputTokens: p.output,
    totalTokens:  p.input + p.cacheRead + p.cacheWrite + p.output,
    costUsd:      Math.round(calcCost(p) * 100) / 100,
    sessions:     p.sessions.size,
    messages:     p.messages,
  });

  const result = { month: fmt(month), week: fmt(week), day: fmt(day) };
  _cache = result; _cacheAt = Date.now();
  return result;
}

module.exports = { readClaudeCodeUsage };
