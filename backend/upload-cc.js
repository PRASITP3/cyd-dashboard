'use strict';
// Local uploader — run this on the PC where Claude Code actually runs.
// It reads ~/.claude usage and pushes a snapshot to Supabase so the Vercel
// dashboard (and the ESP32) can read it from anywhere.
//
//   one-shot:   node upload-cc.js
//   keep alive: node upload-cc.js --watch        (re-uploads every INTERVAL)
//
// Requires env (see .env): SUPABASE_URL, SUPABASE_SERVICE_KEY
require('dotenv').config();

const { readClaudeCodeUsage } = require('./lib/cc-usage');
const { putCcToSupabase }     = require('./lib/supabase');

const INTERVAL = Number(process.env.UPLOAD_INTERVAL_MS) || 5 * 60 * 1000; // 5 min
const WATCH    = process.argv.includes('--watch');

async function uploadOnce() {
  const cc = readClaudeCodeUsage({ useCache: false });
  if (!cc) {
    console.error('[upload] no ~/.claude data found on this machine — nothing to upload');
    return false;
  }
  await putCcToSupabase(cc);
  const ts = new Date().toISOString();
  console.log(`[upload] ${ts}  day=${cc.day.totalTokens}  week=${cc.week.totalTokens}  month=${cc.month.totalTokens}`);
  return true;
}

(async () => {
  try {
    await uploadOnce();
  } catch (err) {
    console.error('[upload] failed:', err.message);
    if (!WATCH) process.exit(1);
  }

  if (WATCH) {
    console.log(`[upload] watching — re-uploading every ${Math.round(INTERVAL / 1000)}s`);
    setInterval(() => {
      uploadOnce().catch(err => console.error('[upload] failed:', err.message));
    }, INTERVAL);
  }
})();
