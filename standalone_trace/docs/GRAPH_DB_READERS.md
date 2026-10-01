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

Writers lock a stable `<resolved-db-path>.lock` sidecar. DB symlink aliases use
the canonical target for the DB, `.meta`, and `.lock` paths. Writers stage both
files in the destination directory. They restore the mode and group, then
`fsync` each file. Under the same lock, they remove the old metadata and sync
the directory before renaming the DB. They sync the directory after the DB
rename and again after the metadata rename.

Two file names cannot be replaced as one atomic operation. An interruption can
leave a complete old or new DB with missing metadata. Incremental compilation
then rebuilds. It cannot treat an old fingerprint as proof for a new DB.
Readers and incremental cache checks hold a shared sidecar lock while loading
the published state. A legacy DB without a sidecar cannot give an incremental
cache hit; its first updated compile rebuilds under an exclusive lock. The
lock file must remain in place: deleting it could
split the writer mutex across two inodes.

A small cleanup child holds the writer lock and watches the compiler's lifetime.
Compiler-only termination, including `SIGKILL`, removes that writer's staging
files before releasing the lock. Normal completion disarms and reaps the child.
Killing the entire process group or losing power can leave staging files. The
next actual writer removes owned, regular, singly linked staging files under
the same exclusive lock. A cache hit or read-only query does not do recovery.
Foreign-owned files, symlinks, and hard links in the reserved staging namespace
are rejected rather than removed.

New files use mode `0666` filtered by the caller's umask. Replacements preserve
the existing mode and group or fail before publication. Read-only DB or metadata
files cannot be silently replaced. An unprivileged writer can change the owner
when replacing another user's writable file; it cannot preserve that other uid.
The writer does not change the process umask. An existing read-only sidecar can
be opened read-only for advisory locking. All retained file descriptors use
`O_CLOEXEC`.

Readers open and map the same file descriptor. They never create a sidecar and
can query DBs in read-only directories. A shared inode lock stays held until
the mapping is released. The shared sidecar lock is released after loading.
Atomic writers can replace the DB while a `serve` session retains its previous
snapshot. `reload` opens the new snapshot before replacing the session.

Each string access checks the offset-table bounds, offset order, blob length,
and mapping length before constructing a view. These checks use stored sizes;
they do not call `fstat` per string. `serve` also checks the inode length at each
command boundary and drops a session whose file length has changed.

All concurrent compile writers must use the updated atomic writer. Concurrent
legacy compile writers are unsupported: the old writer can lock an old inode,
then reopen the path after an atomic rename and truncate the new inode. A reader's
shared inode lease does not prevent that race. Reading old DB formats remains
supported. On an unchanged inode, a cooperating legacy writer waits for the
reader's shared lease. Bounds and length checks do not prevent `SIGBUS` if an
uncooperative writer truncates a mapped inode during a query. They also cannot
detect every same-size rewrite. Concurrent mixed legacy and atomic writers are
outside the supported snapshot contract.
