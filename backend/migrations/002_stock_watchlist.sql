-- Run this in the time-capital Supabase project (SQL Editor / Studio).
-- Ordered watchlist for the ESP32 stock ticker. Edit rows here to change what
-- the screen shows — no redeploy needed (the backend reads this on each refresh,
-- cached ~5 min). Falls back to the STOCK_SYMBOLS env var when empty.

create table if not exists public.stock_watchlist (
  position int  primary key,   -- display order (lower first)
  symbol   text not null,      -- Yahoo symbol, e.g. GOOG80.BK  (Thai SET/DR use .BK)
  display  text,               -- label to show (null = symbol without .BK)
  enabled  boolean not null default true
);

alter table public.stock_watchlist enable row level security;

drop policy if exists "anon read watchlist" on public.stock_watchlist;
create policy "anon read watchlist"
  on public.stock_watchlist
  for select
  to anon
  using (true);

-- Seed (the PORT from the screenshot). Replace with your own list, then:
--   insert ... on conflict (position) do update set symbol=excluded.symbol, ...
insert into public.stock_watchlist (position, symbol) values
  (1,'GOOG80.BK'), (2,'AMZN80.BK'), (3,'LVMH01.BK'), (4,'ICHI.BK'),
  (5,'SMIC23.BK'), (6,'TENCENT80.BK'), (7,'MICRON80.BK'), (8,'CPN.BK'),
  (9,'AP.BK'), (10,'LLY80.BK'), (11,'CATL01.BK'), (12,'SISB.BK'), (13,'AU.BK')
on conflict (position) do nothing;
