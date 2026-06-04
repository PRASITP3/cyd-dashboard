# Claude Code usage via Supabase (use the dashboard from anywhere)

## Why
Vercel is serverless — it **cannot** read `~/.claude/projects` on your PC, so the
cloud endpoint always returned `claudeCode: null` and the ESP32 showed all `0`s.

New flow: your PC pushes a usage snapshot to Supabase; Vercel reads it back.

```
PC (upload-cc.js)              Supabase (cc_usage)            Vercel             ESP32
~/.claude/projects  ──upsert──►   { data: {...} }   ──select──► /api/dashboard ──► screen
   service_role key                                  anon key
```

## 1. Create the table (time-capital project)
Open Supabase Studio → SQL Editor → paste & run `migrations/001_cc_usage.sql`.

## 2. Get the keys
Studio → Project Settings → API:
- **Project URL** → `SUPABASE_URL`
- **anon / publishable** key → `SUPABASE_ANON_KEY` (read-only, goes to Vercel)
- **service_role** key → `SUPABASE_SERVICE_KEY` (secret, **PC only**)

## 3. Configure Vercel (the dashboard)
Vercel project → Settings → Environment Variables, add:
- `SUPABASE_URL`
- `SUPABASE_ANON_KEY`

Then redeploy (`vercel --prod`, or push, or "Redeploy" in the dashboard).
Do **NOT** add the service_role key to Vercel.

## 4. Run the uploader on your PC
```powershell
cd C:\projects\cyd-dashboard\backend
copy .env.example .env      # then fill SUPABASE_URL + SUPABASE_SERVICE_KEY
npm install
npm run upload              # one-shot test
npm run upload:watch        # keeps pushing every 5 min
```

To run it automatically at login, Task Scheduler → Create Task → Action:
`node.exe` with arguments `upload-cc.js --watch`, "Start in"
`C:\projects\cyd-dashboard\backend`.

## 5. Verify
```powershell
curl https://cyd-dashboard.vercel.app/api/dashboard      # claudeCode should be populated
curl https://cyd-dashboard.vercel.app/api/test           # source: "supabase"
```
The ESP32 picks it up on its next 60s refresh — no firmware change needed.
(`UPDATE_INTERVAL` in `firmware/include/config.h`.)
