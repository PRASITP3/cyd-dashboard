-- Run this in the time-capital Supabase project (SQL Editor / Studio).
-- Supersedes 002_stock_watchlist.sql — this is a real PORTFOLIO: it stores the
-- cost basis (vol + avg) so the backend can fetch the live market price and
-- compute %U.PL = (market - avg) / avg * 100 automatically.
--
-- Edit rows here to change holdings — no redeploy needed (backend reads this,
-- cached ~5 min). If this table is empty/absent, the backend falls back to a
-- built-in default (the same seed below).

create table if not exists public.portfolio (
  position int     primary key,        -- display order
  symbol   text    not null,           -- Yahoo symbol, e.g. GOOG80.BK (.BK = SET/DR)
  display  text,                       -- label to show (null = symbol without .BK)
  vol      bigint  not null default 0, -- shares held (Avail Vol)
  avg_cost numeric not null,           -- average cost (Avg)
  enabled  boolean not null default true
);

alter table public.portfolio enable row level security;
drop policy if exists "anon read portfolio" on public.portfolio;
create policy "anon read portfolio"
  on public.portfolio for select to anon using (true);

-- Seed = holdings from the Maybank Portfolio screenshot (SSI excluded — delisted).
insert into public.portfolio (position, symbol, vol, avg_cost) values
  (1,'AMZN80.BK',    122500, 2.06),
  (2,'GOOG80.BK',     58299, 3.92),
  (3,'ICHI.BK',       19200, 13.09),
  (4,'LVMH01.BK',      7100, 14.22),
  (5,'MICRON80.BK',       0, 30.80),
  (6,'SHOP03.BK',     40000, 2.52),
  (7,'SMIC23.BK',     69400, 3.61),
  (8,'TENCENT80.BK',  13000, 19.31)
on conflict (position) do update
  set symbol = excluded.symbol, vol = excluded.vol, avg_cost = excluded.avg_cost;
