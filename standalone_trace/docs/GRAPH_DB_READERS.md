# Graph DB snapshots

The v1-v5 reader maps the DB file as an immutable snapshot. Strings use views into
its offset table and blob. POD sections use checked, read-only views. Each record
is copied with `memcpy` because the v5 section offsets can be unaligned. The
compiler keeps its existing owned vectors and writes the same v5 bytes.

All prior content validation remains eager. Header sizes and section counts are
checked against the mapped file length before access or allocation. Invalid
unused sections still reject the DB. There are no newly accepted malformed DB
cases. Eager validation still faults in sections during cold queries.

Runtime maps are built for each command. `find` needs only the name views.
`hier` and `whereis-instance` build only hierarchy data. `trace` builds names,
reverse references, and global nets. `serve` keeps these maps and the hierarchy
ready so its startup and status responses remain unchanged.

Writers lock a stable `<resolved-db-path>.lock` sidecar. They write a unique
file in the destination directory, close it, and atomically rename it over the
DB. Failed writes leave the existing DB intact and remove temporary output.
Existing DB modes and symlink targets are preserved. The lock file must remain
in place, because deleting it could split the writer mutex across two inodes.

Readers open and map the same file descriptor. They never create a sidecar and
can query DBs in read-only directories. A shared inode lock stays held until
the mapping is released. Atomic writers use only their stable writer mutex, so compilation can
replace the DB while a `serve` session retains its previous snapshot. `reload`
opens the new snapshot before replacing the session.

All concurrent compile writers must use the updated atomic writer. Concurrent
legacy compile writers are unsupported: the old writer can lock an old inode,
then reopen the path after an atomic rename and truncate the new inode. A reader's
shared inode lease does not prevent that race. Reading old DB formats remains
supported.

The compile fingerprint sidecar (`.meta`) is still published by `Compiler.cc`
after the DB write. The DB and its fingerprint are not an atomic pair. This
pre-existing limitation remains for concurrent compiles with different
arguments; the stable writer lock guarantees DB-file publication only.
