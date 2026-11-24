begin;

create table public.account_mod_profiles (
    user_id uuid not null references auth.users (id) on delete cascade,
    game_id text not null check (game_id in ('mp9', 'wiiparty')),
    profile_name text not null check (profile_name ~ '^[A-Za-z0-9_-]{1,40}$' and profile_name <> 'session'),
    version bigint not null check (version > 0),
    catalog jsonb not null check (
        jsonb_typeof(catalog) = 'object'
        and catalog ?& array['schema','version','game_id','base_content_id','loader','profile','mods']
        and catalog->>'schema' = 'wii-launcher-mod-catalog'
        and catalog->'version' = '1'::jsonb
        and catalog->>'game_id' = game_id and catalog->>'profile' = profile_name
        and jsonb_typeof(catalog->'mods') = 'array'
        and jsonb_array_length(catalog->'mods') <= 1000
        and octet_length(catalog::text) <= 393216
        and (catalog - array['schema','version','game_id','base_content_id','loader','profile','mods']) = '{}'::jsonb
    ),
    sha256 text not null check (sha256 ~ '^[0-9a-f]{64}$'),
    updated_at timestamptz not null default now(),
    primary key (user_id, game_id, profile_name)
);

alter table public.account_mod_profiles enable row level security;
revoke all on public.account_mod_profiles from public, anon, authenticated;
grant select on public.account_mod_profiles to authenticated;
create policy account_mod_own_profiles on public.account_mod_profiles
    for select to authenticated using ((select auth.uid()) = user_id);

create function public.account_commit_mod_catalog(
    p_game text, p_profile text, p_version bigint, p_catalog jsonb, p_hash text
) returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
    uid uuid := auth.uid();
    current_version bigint;
begin
    if uid is null then
        raise exception 'authentication required' using errcode = '42501';
    end if;
    if p_version is null or p_version < 0 or p_version >= 2147483647 then
        raise exception 'invalid catalog revision' using errcode = '22023';
    end if;
    if p_version = 0 then
        insert into public.account_mod_profiles (user_id, game_id, profile_name, version, catalog, sha256)
        values (uid, p_game, p_profile, 1, p_catalog, p_hash)
        on conflict (user_id, game_id, profile_name) do nothing
        returning version into current_version;
    else
        update public.account_mod_profiles as p
          set catalog = p_catalog, sha256 = p_hash, updated_at = now(), version = p.version + 1
          where p.user_id = uid and p.game_id = p_game and p.profile_name = p_profile and p.version = p_version
          returning p.version into current_version;
    end if;
    if current_version is not null then
        return jsonb_build_object('status', 'committed', 'version', current_version);
    end if;
    select p.version into current_version from public.account_mod_profiles as p
      where p.user_id = uid and p.game_id = p_game and p.profile_name = p_profile;
    return jsonb_build_object('status', 'conflict', 'version', coalesce(current_version, 0));
end;
$$;
revoke all on function public.account_commit_mod_catalog(text, text, bigint, jsonb, text) from public, anon, authenticated;
grant execute on function public.account_commit_mod_catalog(text, text, bigint, jsonb, text) to authenticated;

commit;
