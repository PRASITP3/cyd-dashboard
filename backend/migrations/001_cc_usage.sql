-- Run this in the time-capital Supabase project (SQL Editor / Studio).
-- Single-row table that holds the latest Claude Code usage snapshot
-- pushed from the local PC by upload-cc.js.

create table if not exists public.cc_usage (
  id         int          primary key default 1,
  data       jsonb        not null,
  updated_at timestamptz  not null default now(),
  constraint cc_usage_singleton check (id = 1)
);

-- Anyone with the anon key may READ the snapshot (the Vercel dashboard does).
-- WRITES happen only with the service_role key (which bypasses RLS) from
-- the local uploader, so no INSERT/UPDATE policy is granted to anon.
alter table public.cc_usage enable row level security;

drop policy if exists "anon read cc_usage" on public.cc_usage;
create policy "anon read cc_usage"
  on public.cc_usage
  for select
  to anon
  using (true);
