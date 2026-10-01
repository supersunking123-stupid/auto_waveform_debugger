# Graph DB snapshots

The reader supports v1-v6 and maps the DB as an immutable snapshot. POD records
are copied with memcpy because section offsets may be unaligned. Version 6
coordinates and declared axes use mapped views with the same eager validation:
sorted unique signal IDs, contiguous axes, known flags, bounded counts and exact
footer EOF. Versions 1-4 retain their small record conversions. The compiler
keeps owned vectors and writes exactly the visible stack's v6 bytes, including
member declaration coverage (header feature bits 3).

Writers use an exclusive lock on the resolved DB path plus .lock. They prepare
private staging files in the destination directory before parsing/elaboration.
This probes actual group/mode permission before graph building. Stage names
include the effective uid and a bounded destination-path hash. The writer never
changes umask. Replacements preserve mode/group, and read-only destinations fail
before building. An unprivileged replacement cannot preserve another uid's owner.

After checked DB close, writers restore attributes, write and close metadata,
remove old metadata, rename the DB, then rename metadata last. All publication
steps stay under the exclusive sidecar lock. There are no DB, metadata or
directory fsync calls: this is a regenerable cache, without crash durability.
Two names cannot be atomically replaced together. A process interruption can
leave old/new complete DB data with missing metadata, which forces a rebuild.

The metadata keeps the compile fingerprint plus a bounded cache envelope with
schema 1, DB byte size and all fixed header bytes. Compile cache checks hold the
shared sidecar lock, open the DB, fstat it and read its fixed header. Wrong size,
magic/version/header, unreadable data or malformed/old metadata causes a rebuild.
This cheap check does not detect same-sized interior corruption or every
power-loss tear. It is deliberately not a full DB checksum.

A DB without .lock gets one forced incremental rebuild under the exclusive
writer lock. Old fingerprint-only metadata also rebuilds once. Keep the sidecar
in place: deleting it can split the compiler mutex across two inodes. Readers
neither create nor open the sidecar. An unreadable sidecar or a suspended compiler
must not block a readable DB query. Readers hold a shared lock on the DB inode
itself to cooperate with legacy truncate writers; the opened FD is the mapped FD.
Atomic writers rename a new inode without waiting for the old serve snapshot.
Reload opens and validates the replacement before changing sessions.

The cleanup guardian retains the exclusive writer lock and watches parent EOF.
It ignores SIGINT, SIGTERM, SIGHUP and SIGQUIT; these are blocked around fork so
there is no child startup race. Compiler-only SIGKILL and process-group SIGINT
clean the current user's owned, regular, singly-linked staging files. Whole-group
SIGKILL or power loss can leave stages. The next actual writer recovers those
stages under the lock. Guardian creation failure warns and continues with local
cleanup; guardian failure after a successful publish warns without failing it.

NFS behavior depends on server/filesystem lock and rename support. flock may map
to POSIX server locks. Remote replacement can invalidate an existing mapped
snapshot and cause ESTALE or SIGBUS. There is no guarantee for such invalidation.

String access checks stored bounds; serve checks mapped inode size at command
boundaries. These do not guarantee protection against a concurrent in-place
truncate during a query. Mixed legacy and atomic compile writers are unsupported:
a legacy writer can lock an old inode and then reopen/truncate a new path inode.
An unchanged inode's cooperating legacy writer waits for the reader lease.
