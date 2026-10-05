# Graph DB snapshots

The reader supports v1-v6 and maps the DB as an immutable snapshot. POD records
are copied with memcpy because section offsets may be unaligned. Version 6
coordinates and declared axes use mapped views with the same eager validation:
sorted unique signal IDs, contiguous axes, known flags, bounded counts and exact
footer EOF. Versions 1-4 retain their small record conversions. The compiler
keeps owned vectors. Header feature bits 1 and 2 describe declared coordinates
and member coverage. Feature bit 4 stores port query coverage. Feature bit 8 stores exact compact
port routes. The current compiler writes feature field 15 at semantics epoch 18.
Readers reject the abandoned feature field 7. Older
readers reject unknown features.

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

Port mapping keeps source bitmaps in their native coordinate frame. A constrained
reverse-hop result may clip a copied native access. Selected mapped identity
expressions may retain separate source accesses instead of a merged bitmap.
A flagged endpoint stores
its separate owner coverage in an interned `Q1;lo:hi[;lo:hi...]|native-bitmap`
envelope. The query reader removes that envelope before returning the endpoint.
Endpoint flags 0x04, 0x08 and 0x10 mean owner coverage, constant connection and
unresolved connection mapping. Constant and unresolved markers carry no fake
source assignment. A separate marker can retain the actual connection source
range for stop detail. Compact R1 ports keep their declaration location.
Unproven mapping results retain the legacy hop answer marked approximate.
Readers reject unknown flags, conflicting markers, missing
feature bits and malformed or overflowing coverage intervals. Coverage also
participates in compile deduplication and canonical VERIFY comparisons. Query
output deduplication uses native endpoint identity after coverage filtering.

Q1 stores connected owner-domain unions. It does not store a source-to-owner
bit pair. Exact routes on standalone compact ports preserve bit pairing.
Q1 remains available for source coverage and explicit terminal markers.

An exact compact Port uses flag `0x20`, coverage flag `0x04`, and header feature
field 15. Its hidden envelope is
`R1;childLo:childHi:targetPathStringId:targetLo[;...]|`.
Each piece maps equal-width child and parent intervals in physical owner order.
Original bound pieces must fit their selected packed rows. Adjacent pieces with
the same target and affine translation may merge across verified contiguous
rows after flattening. The reader checks full-root bounds for that merged range.
The parent target uses verified full-owner ordinals, including fixed unpacked
prefixes. The public endpoint keeps its native source coordinates. The query
intersects the selected child domain and follows each exact parent domain.
It does not infer bit pairing from Q1 coverage or equal native span widths.

R1 has 1 to 256 sorted pieces. They cover the complete integral formal without
overlap. Target names must identify existing root signals with verified layouts.
Readers reject bad tokens, overflow, unknown targets, invalid bounds, incomplete
coverage, conflicting flags, source references, and nonempty native payloads.
Only input-driver and output-load lists may carry these routes. Both owned and
mapped readers validate them before use. Header and endpoint POD sizes stay
unchanged. Old readers reject feature 15. Genuine feature-1/3 coordinate and
route behavior is unchanged. Rebuild caches from older experimental mapping
semantics with the current compiler.

The query identity repair includes ordered public LHS and RHS path vectors in
the endpoint key. Fields and vectors use count and length framing. Different
generated timing contexts must not collapse when their source location and
native bitmap match. Identical full records still deduplicate. Hidden Q1/R1
fields and numeric string IDs are not public query identity. This reader fix
applies to all supported formats. It changes no stored bytes or compile epoch.
Ordinary queries can expose distinct references that a refs-omitting key
suppressed. The query key does not change the compiled DB layout.

Root input queries can retain an active declaration beside a mapped writer.
For an unused plain input, the exact connection and selected owner coverage
control this compatibility record. A 32-bit ANSI packed-struct input uses
a complete checked formal map with exact signal or constant pieces. Its
constant-connected bits keep the active declaration and the constant stop.
The retained port keeps its original public native bitmap. Q1 coverage filters
the queried owner domain separately. A connected writer alone does not prove
that an active port declaration can be removed.
