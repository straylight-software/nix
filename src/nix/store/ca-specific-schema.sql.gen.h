// ca-specific-schema.sql.gen.h - Generated from ca-specific-schema.sql
// TODO[b7r6]: Generate this properly in build

R"sql(
-- Extension of the sql schema for content-addressing derivations.
-- Won't be loaded unless the experimental feature `ca-derivations`
-- is enabled

create table if not exists Realisations (
    id integer primary key autoincrement not null,
    drv_path text not null,
    output_name text not null, -- symbolic output id, usually "out"
    output_path integer not null,
    signatures text, -- space-separated list
    foreign key (output_path) references ValidPaths(id) on delete cascade
);

create index if not exists IndexRealisations on Realisations(drv_path, output_name);

-- We can end-up in a weird edge-case where a path depends on itself because
-- it’s an output of a CA derivation, that happens to be the same as one of its
-- dependencies.
-- In that case we have a dependency loop (path -> realisation1 -> realisation2
-- -> path) that we need to break by removing the dependencies between the
-- realisations
create trigger if not exists DeleteSelfRefsViaRealisations before delete on ValidPaths
  begin
    delete from RealisationsRefs where realisationReference in (
      select id from Realisations where output_path = old.id
    );
  end;

create table if not exists RealisationsRefs (
    referrer integer not null,
    realisationReference integer,
    foreign key (referrer) references Realisations(id) on delete cascade,
    foreign key (realisationReference) references Realisations(id) on delete restrict
);
-- used by deletion trigger
create index if not exists IndexRealisationsRefsRealisationReference on RealisationsRefs(realisationReference);

-- used by QueryRealisationReferences
create index if not exists IndexRealisationsRefs on RealisationsRefs(referrer);
-- used by cascade deletion when ValidPaths is deleted
create index if not exists IndexRealisationsRefsOnOutputPath on Realisations(output_path);
)sql"
