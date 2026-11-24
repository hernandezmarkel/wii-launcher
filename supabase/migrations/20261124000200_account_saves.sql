begin;

create table public.account_save_revisions (
    revision_id uuid primary key,
    user_id uuid not null references auth.users (id) on delete cascade,
    game_id text not null check (game_id in ('mp9', 'wiiparty')),
    slot_id text not null check (slot_id ~ '^[0-9a-f]{8}$'),
    parent_revision_id uuid,
    sha256 text not null check (sha256 ~ '^[0-9a-f]{64}$'),
    byte_size integer not null check (byte_size between 1 and 33554432),
    compatibility text not null check (compatibility ~ '^[A-Z0-9]{6}:[A-Za-z0-9._-]{1,80}$'),
    device_id uuid not null,
    storage_key text generated always as (
        user_id::text || '/' || game_id || '/' || slot_id || '/' || revision_id::text
    ) stored,
    created_at timestamptz not null default now(),
    unique (user_id, game_id, slot_id, revision_id),
    foreign key (user_id, game_id, slot_id, parent_revision_id)
        references public.account_save_revisions (user_id, game_id, slot_id, revision_id),
    check (revision_id is distinct from parent_revision_id)
);
create unique index account_save_revisions_storage_key on public.account_save_revisions (storage_key);

create table public.account_save_heads (
    user_id uuid not null references auth.users (id) on delete cascade,
    game_id text not null,
    slot_id text not null,
    revision_id uuid not null,
    version bigint not null check (version > 0),
    primary key (user_id, game_id, slot_id),
    foreign key (user_id, game_id, slot_id, revision_id)
        references public.account_save_revisions (user_id, game_id, slot_id, revision_id)
);

alter table public.account_save_revisions enable row level security;
alter table public.account_save_heads enable row level security;
revoke all on public.account_save_revisions, public.account_save_heads from public, anon, authenticated;
grant select on public.account_save_revisions, public.account_save_heads to authenticated;
create policy account_save_own_revisions on public.account_save_revisions
    for select to authenticated using ((select auth.uid()) = user_id);
create policy account_save_own_heads on public.account_save_heads
    for select to authenticated using ((select auth.uid()) = user_id);

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('wiiports-saves', 'wiiports-saves', false, 33554432, array['application/octet-stream']);

create policy wiiports_saves_read_own on storage.objects for select to authenticated
    using (bucket_id = 'wiiports-saves' and (storage.foldername(name))[1] = (select auth.uid())::text);
create policy wiiports_saves_insert_own on storage.objects for insert to authenticated
    with check (
        bucket_id = 'wiiports-saves'
        and (storage.foldername(name))[1] = (select auth.uid())::text
        and name ~ '^[0-9a-f-]{36}/(mp9|wiiparty)/[0-9a-f]{8}/[0-9a-f-]{36}$'
    );

create function public.account_commit_save(
    expected_head uuid,
    new_revision uuid,
    save_game text,
    save_slot text,
    content_hash text,
    content_size integer,
    port_compatibility text,
    source_device uuid
) returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
    uid uuid := auth.uid();
    head uuid;
    current_version bigint;
    candidate public.account_save_revisions%rowtype;
    key text;
begin
    if uid is null then
        raise exception 'authentication required' using errcode = '42501';
    end if;
    if new_revision is null or source_device is null or save_game is null
       or save_game not in ('mp9', 'wiiparty') or save_slot is null or save_slot !~ '^[0-9a-f]{8}$'
       or content_hash is null or content_hash !~ '^[0-9a-f]{64}$'
       or content_size is null or content_size not between 1 and 33554432
       or port_compatibility is null or port_compatibility !~ '^[A-Z0-9]{6}:[A-Za-z0-9._-]{1,80}$'
       or new_revision is not distinct from expected_head then
        raise exception 'invalid revision metadata' using errcode = '22023';
    end if;

    perform pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended(uid::text, 827156));
    select h.revision_id, h.version into head, current_version
        from public.account_save_heads as h
        where h.user_id = uid and h.game_id = save_game and h.slot_id = save_slot;
    select * into candidate from public.account_save_revisions as r
        where r.revision_id = new_revision and r.user_id = uid;
    if found then
        if candidate.game_id <> save_game or candidate.slot_id <> save_slot
           or candidate.parent_revision_id is distinct from expected_head or candidate.sha256 <> content_hash
           or candidate.byte_size <> content_size or candidate.compatibility <> port_compatibility
           or candidate.device_id <> source_device then
            raise exception 'revision already exists with different metadata' using errcode = '22023';
        end if;
        return jsonb_build_object('status', case when head = new_revision then 'committed' else 'conflict' end,
                                  'head', head, 'version', coalesce(current_version, 0));
    end if;

    if (select coalesce(sum(r.byte_size), 0) from public.account_save_revisions as r where r.user_id = uid)
        + content_size > 268435456 then
        raise exception 'account save quota exceeded' using errcode = '54000';
    end if;
    key := uid::text || '/' || save_game || '/' || save_slot || '/' || new_revision::text;
    perform 1 from storage.objects as o where o.bucket_id = 'wiiports-saves' and o.name = key
        and o.metadata->>'size' = content_size::text;
    if not found then
        raise exception 'save object missing or wrong size' using errcode = '22023';
    end if;

    insert into public.account_save_revisions
        (revision_id, user_id, game_id, slot_id, parent_revision_id, sha256, byte_size, compatibility, device_id)
        values (new_revision, uid, save_game, save_slot, expected_head, content_hash, content_size, port_compatibility, source_device);
    if head is distinct from expected_head then
        return jsonb_build_object('status', 'conflict', 'head', head, 'version', coalesce(current_version, 0));
    end if;
    insert into public.account_save_heads (user_id, game_id, slot_id, revision_id, version)
    values (uid, save_game, save_slot, new_revision, 1)
    on conflict (user_id, game_id, slot_id) do update
        set revision_id = excluded.revision_id, version = public.account_save_heads.version + 1
    returning version into current_version;
    return jsonb_build_object('status', 'committed', 'head', new_revision, 'version', current_version);
end;
$$;
revoke all on function public.account_commit_save(uuid, uuid, text, text, text, integer, text, uuid)
    from public, anon, authenticated;
grant execute on function public.account_commit_save(uuid, uuid, text, text, text, integer, text, uuid)
    to authenticated;

commit;
