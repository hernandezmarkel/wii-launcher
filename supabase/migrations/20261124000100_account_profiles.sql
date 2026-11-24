begin;

create table public.account_profiles (
    user_id uuid primary key references auth.users (id) on delete cascade,
    display_name text collate "C" not null check (
        char_length(display_name) between 3 and 24
        and display_name ~ '^[A-Za-z0-9][A-Za-z0-9 _-]+[A-Za-z0-9]$'
        and position('  ' in display_name) = 0
    ),
    normalized_display_name text collate "C" generated always as (lower(display_name)) stored unique,
    created_at timestamptz not null default now()
);

alter table public.account_profiles enable row level security;
revoke all on public.account_profiles from public, anon, authenticated;
grant select, delete on public.account_profiles to authenticated;
grant insert (user_id, display_name), update (user_id, display_name)
    on public.account_profiles to authenticated;

create policy account_profiles_select_own on public.account_profiles
    for select to authenticated using ((select auth.uid()) = user_id);
create policy account_profiles_insert_own on public.account_profiles
    for insert to authenticated with check ((select auth.uid()) = user_id);
create policy account_profiles_update_own on public.account_profiles
    for update to authenticated using ((select auth.uid()) = user_id)
    with check ((select auth.uid()) = user_id);
create policy account_profiles_delete_own on public.account_profiles
    for delete to authenticated using ((select auth.uid()) = user_id);

commit;
