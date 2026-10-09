RTorrent BitTorrent Client
========

Introduction
------------

A ncurses-based command line torrent client for high performance. 

To learn how to use rTorrent visit the [Wiki](https://github.com/rakshasa/rtorrent/wiki).

Download the [latest stable release](https://github.com/rakshasa/rtorrent/releases/latest)

Related Projects
----------------

* [https://github.com/rakshasa/rbedit](https://github.com/rakshasa/rbedit): A dependency-free bencode editor.

Donate to rTorrent development
------------------------------

* [Paypal](https://paypal.me/jarisundellno)
* [Patreon](https://www.patreon.com/rtorrent)
* [SubscribeStar](https://www.subscribestar.com/rtorrent)
* Bitcoin: 1MpmXm5AHtdBoDaLZstJw8nupJJaeKu8V8
* Ethereum: 0x9AB1e3C3d8a875e870f161b3e9287Db0E6DAfF78
* Litecoin: LdyaVR67LBnTf6mAT4QJnjSG2Zk67qxmfQ
* Cardano: addr1qytaslmqmk6dspltw06sp0zf83dh09u79j49ceh5y26zdcccgq4ph7nmx6kgmzeldauj43254ey97f3x4xw49d86aguqwfhlte


Help keep rTorrent development going by donating to its creator.


BUILDING
--------

Jump into the github cloned directory

```
cd rtorrent
```

## Install build dependencies

Install [libtorrent](https://github.com/rakshasa/libtorrent) with the same version rTorrent.

Generate configure scripts:

```
autoreconf -ivf
```

Optionally, generate man pages:

```
docbook2man rtorrent.1.xml
```

Man pages output to "doc/rtorrent.1".

RTorrent follows the development of [libtorrent](https://github.com/rakshasa/libtorrent) closely, and thus the versions must be in sync.

## USAGE

Refer to User Guide: https://github.com/rakshasa/rtorrent/wiki/User-Guide

## LICENSE

GNU GPL, see COPYING. "libtorrent/src/utils/sha_fast.{cc,h}" is
originally from the Mozilla NSS and is under a triple license; MPL,
LGPL and GPL. An exception to non-NSS code has been added for linking to OpenSSL as requested by Debian, though the author considers that library to be part of the Operative System and thus linking is allowed according to the GPL.

Use whatever fits your purpose, the code required to compile with
Mozilla's NSS implementation of SHA1 has been retained and can be
compiled if the user wishes to avoid using OpenSSL.

## DEPENDENCIES

* libcurl >= 7.12.0
* libtorrent = (same version)
* ncurses

## BUILD DEPENDENCIES

* libtoolize
* aclocal
* autoconf
* autoheader
* automake

## Native fork runtime requirements

The `rutorrent-native` branch targets current upstream `master`. Native session
and retrackers patches are not carried onto the legacy 0.9.8 build.

Use a POSIX filesystem for the session directory: it must retain numeric
ownership and private permissions and support `openat` with `O_NOFOLLOW`, atomic
rename, `fsync`, and `fdatasync`. Every ancestor of the session directory must
be owned by root or the daemon's effective UID. Group/other write access on an
ancestor is accepted only when it has the sticky bit, as with `/tmp` 01777.
The session directory itself must be owned by the daemon and must have no
group/other write access, even when sticky. Setgid session directories are
supported. No path component may be a symlink, including with a trailing slash
or in a relative path; `..` does not bypass the ancestor checks.

The `.rutorrent-stop-intents` and `.retrackers-migration` directories must be
owned by the daemon with permissions 0700; setgid is allowed. Stop generation
records must be regular, single-link files with mode 0600. Stop claim files
must be regular, single-link files with mode 0600. A historical interrupted
link publication is recovered only when its complete 128-byte receipt and
exact nonce-named staging entry identify the same owned inode with precisely
two links; all other aliases remain refused. Linux publication now uses
`renameat2(RENAME_NOREPLACE)`. When the filesystem or kernel rejects that
operation with EINVAL, ENOSYS or EOPNOTSUPP, the daemon main thread checks the
final entry with `AT_SYMLINK_NOFOLLOW` and uses plain `renameat` only if absent.
No lock is taken: the main thread is the only writer of these private
directories, and only trusted same-UID processes could race the check.
Existing files and dangling symlinks are refused. The first fallback of a
process is reported once, by errno class only, to the `warn` log and stderr.
ruTorrent's `plugins/erasedata/rename-noreplace.c` applies the same fallback
rule to payload renames; change both together. Private directory admission
excludes other UIDs; same-UID processes remain trusted. File and directory
synchronization is unchanged. Journal and claim opens refuse symlinks;
multiply linked stop generation records are refused. Filesystems that discard
these permissions, and mounts with incompatible ownership mapping, require
operator correction rather than an automatic fallback. Path safety refusals
name the offending path, observed UID and octal mode when available, and the
required ownership/mode. Corrupt stop records and claims name their file path
and torrent hash.

Stop generations are published as one atomic 17-byte record. A new same-hash
load starts at the current generation; erasure retires its record unless an
unresolved retrackers migration still needs it. Startup removes safe orphaned
records and unpublished temporary files after migration recovery. User Stop
commands remain synchronous so a later crash cannot silently revoke a Stop.

The bounded retrackers queue requires Linux parent-death support. The daemon
captures `RUTORRENT_NATIVE_PHP` (default `/usr/bin/php85`) and
`RUTORRENT_NATIVE_APP_ROOT` (default `/rutorrent/app`) once before accepting RPC.
The only actor scripts are `plugins/retrackers/update.php` and
`plugins/retrackers/dispatch.php` below that root. RPC callers cannot select
executables, scripts or environments. Actor paths require trusted root/daemon
ownership through every ancestor, safe permissions, no symlinks, regular
single-link leaves, and a PHP executable without setuid, setgid or file
capabilities. Unsafe or missing configured paths visibly refuse admission.

Eight worker reservations and one independent nonoverlapping dispatcher bound
this mechanism to nine direct children, including launches before their first
RPC and exited children until reap. Children receive an exact T/I job identity,
parent-death SIGKILL, and direct argv without a shell. No PID timeout or absent
process retires a captured migration obligation. Only the reaped exit of the
exact native-owned worker H/T/I proves that worker gone, and no other process
can continue its captured transaction (`worker_start` answers `held` for it).
A watcher that runs while workers live then finishes it as startup would: a
committed candidate is released; otherwise an idle uncommitted candidate is
erased, the original is restored from its claim and released, and Q stays
owed. That rollback spends one counted attempt (`capture-refused`, due
2^count seconds later, the sixth terminal) in the same released state, so a
worker that always dies after capture ends terminal after six cycles instead
of looping; a commit is not counted. Each outcome is one error-log line,
`RR worker exit recovery: H: finished` or `rolled back; attempt N of 6`, or
`RR worker exit recovery held: H: <reason>`.
Before capture nothing was taken, so Q stays owed and the dispatcher relaunches
the job. Pending Q intent and
exact completion receipts are durable across daemon restart. Original release
receipts attest completed Q only after an explicit application finish; cold
recovery and rollback retire ownership while leaving unprocessed Q pending.
Trusted `d.retrackers.cancel_original(H,T,I,Q)` takes exactly these three
arguments after the hash. It requires exact current preclaim
T/I/Q/U/L and marker/ACK, no capsule/release/file lease, no completed marker
and no active insert hook. It durably preserves the live source and pending Q
while retiring only that observation and its pending hook key. Unknown work
stays held; cancellation never acknowledges tracker processing.
`d.retrackers.defer_original(H,T,I,Q,expected,retry)` takes exactly five and
adds an exact compare-and-set
of `retrackers-retry` to that same durable cancellation. Its receipt is
`r1:Q:count:due:reason`: counts 1 through 5 have a positive Unix due time, count 6
has due 0 and a visible exhaustion message. `d.message` projects that terminal
status from the exact durable Q whenever its ordinary message is empty, including
after tracker success clears the message or after a cold restart. Reasons are `capture-refused`,
`transport-unavailable`, `source-not-ready` or `preflight-refused`. Each update
retains Q unfinished and either advances exactly one attempt or, as a wait for
`source-not-ready` or `transport-unavailable`, keeps the current count with a
nonzero due. Counted reasons always advance, and a wait is never terminal.
The first receipt has count 1 whatever its reason, so a first wait also spends
attempt 1, as the application computes it. `system.retrackers.retry_waits`
returns 1 on daemons that accept waits. Stale
values are refused before mutation. One parser, `src/core/retrackers_queue_receipts.h`, holds this format,
the six-attempt limit and the reasons; ruTorrent's
`plugins/retrackers/update.php` `RetrackersQueueRetry` must accept the same
receipts. A failure before the release is durable, such as a storage error in
the later save, restores the expected retry value and status, so the same
compare-and-set succeeds once storage is repaired. Once the release receipt, or
after final cleanup the finalized receipt of the same T/I, exists, the released
snapshot carries the new retry and a later failure keeps it. Native enqueue restores the
application `wp:L` pending receipt while
preserving hook receipts. A new Q clears only its inherited retry state and the
exact native exhaustion message; dequeue clears the job's retry receipt and
that message too. When the durable save of a new Q fails with a storage error
(EACCES, ENOSPC), enqueue rolls Q back and refuses with `RR enqueue refused:
session-storage-failed`, as an error-log line and the torrent's `d.message`:
the insert hook logs the refusal instead of ending the daemon, and a later
insert or recovery can enqueue again. A paused original (open, stopped) that
capture closed is reopened after its release unless a user Stop followed the
owned quiesce; open is runtime state, so after a daemon restart it comes back
stopped like any other paused torrent. Direct user erase may cancel an exact queued
uncaptured claim synchronously; otherwise the erase is refused with
`RR original claim blocks direct erase` and the cancellation refusal. Any
capsule file, including partial publication, keeps the source held:
`system.retrackers.original_claim` then answers `held`, and capture, begin,
enqueue, finish, cancel, defer and erase all refuse. Startup removes the
unpublished `.HASH...tmp.PID.N` temporaries that a killed writer can leave in
`.retrackers-migration` and the session directory, and reports other entries
with such names instead of touching them. A few seconds after an erase, one
batch for all recent erases durably removes the ordinary triples' names and
then retires those owners' tombstones and finalized receipts, unless another
journal entry still names a hash: three directory syncs per batch rather than
per erase. A clean shutdown runs a pending batch before it exits. Startup does
the same for tombstones that older daemons or a crash left and for ordinary
owners whose triple is gone, so erased hashes do not accumulate in the journal.
Finalize and owner repair at startup each read the journal in one scan rather
than one per receipt.

Normal and quick shutdown save and flush session data before tracker stop
requests can consume the service timeout. The docker-rutorrent rc hook
`event.system.shutdown` -> `session.save=` stays for 0.9.8 and unpatched
daemons, which lack this native step. On this daemon both run: the hook only
queues resume builds, and the native save finds every download already pending,
so each triple is built and written once before the flush. Saves of one
download never overlap, because both would share its `.new` staging names: a
later request waits in the queue for the running one. A save that a migration
claim redirects to its journal (`session.save`, `d.save_resume`, the shutdown
save) never throws from an asynchronous save request: a refusal or storage
error there is logged like a failed session write for that download, and the
save goes on with the others; durable saves still throw. Startup holds and
refusals of migration recovery reach the error log as well as the UI log. A
restored original whose release completes when it loads gets its ordinary
session event, so a started original is started again instead of staying
closed with `d.state=1`.

Trusted `d.retrackers.preflight_stage` reads only canonical procfd capabilities
held by the exact live registered H/T/I worker. It verifies anonymous regular
0600 files owned by the daemon, exact device/inode/size, a 64 MiB limit, SHA256
and nonce; current T/I/Q and actor liveness are checked again before success.
It creates no helper PHP/cat processes. Five-second elapsed checkpoints bound
userspace work; one regular filesystem read cannot be preempted, so services
must preserve ambiguity while a synchronous operation is busy. Legacy 0.9.8
keeps its separate existing helper behavior and has no native queue ABI.

Destructive migration failpoints are disabled by default. Only dedicated test
builds configured with `--enable-rr-migration-test-cuts` honor
`RR_MIG_TEST_CUT`, read only through `src/core/migration_test_cut.h`;
production builds ignore it. Run `make check` for unit tests; the rename
fallback unit tests that inject errors are registered only in test-cut builds.
Run the current-master daemon gates with an existing builder image that supplies
its matching libtorrent ABI and C++ compiler:

```
python3 test/integration/native_regressions.py BUILDER_IMAGE BUILD_TREE SCRATCH
```

`BUILD_TREE/src/rtorrent` must contain the exact candidate binary. The runner
uses disposable network-isolated containers, a synthetic local metadata peer,
a local tracker that holds stopped announces, and a separate Unix UID for
asynchronous path refusal. On a dedicated test-cut build, add `--cuts` to run
every case, including those listed in the runner's `CUTS` table; the default
run excludes them. `--ninep` repeats the run with a seccomp profile that fails
flagged `renameat2` with EINVAL, as WSL drvfs over 9p does. Every daemon
scenario explicitly retains session mode 02700 and the unsafe-path actor uses
a different Unix UID.

The older integration fixture in `test/integration/native_state.py` also uses
an isolated container and synthetic local peer. Supply a runtime image with the
matching libtorrent ABI when mounting a candidate binary; an older image cannot
load a current-master binary simply because both are named rtorrent.
Run `python3 test/integration/meta_callback.py` for callback lifetime and Stop
transfer regressions against the actual function body using non-deleting test
doubles; this focused check requires a host C++ compiler.

Image pins use full commit SHAs. Keep every shipped pin reachable from an
immutable published release tag; create and publish that tag only as part of
the owner-approved release workflow. A moving branch tip alone is not a
long-term source archive.

The optional native retrackers queue keeps its `q1:U:Q` job identity in the
ordinary session custom fields and binds Q to its durable original preclaim.
A finished Q is acknowledged only with matching finalized T/I authority.
`system.retrackers.worker_start(H,T,I,user)` admits eight direct worker children
from launch reservation through reap. `system.retrackers.dispatcher_start(user)`
admits one separate dispatcher. It answers `idle` without launching when no
queue of that user has work: a finalized claim to dequeue, or, while a worker
slot is free, a none or original claim whose retry is empty or due, that has no
live worker and, for none, no file-operation stop lease. Waiting and terminal
jobs without a completion marker are skipped before any journal read. A
malformed queue marker, an invalid retry, recovery markers on a none claim or
an unreadable claim start a pass at most once a minute, so the dispatcher can
report them. Duplicate launches reuse their exact identity;
full or held admission preserves the durable queue. Child death never releases
transaction authority. On Linux, parent death kills the directly executed
workers with `PR_SET_PDEATHSIG`; privilege-changing executable files are refused.

These execution methods are trusted-only and are not marked RPC-safe. They take
no executable, script, or environment arguments. The executable and application
root are captured once during daemon startup from `RUTORRENT_NATIVE_PHP` and
`RUTORRENT_NATIVE_APP_ROOT`, defaulting to `/usr/bin/php85` and `/rutorrent/app`.
Only the fixed `plugins/retrackers/update.php` and `plugins/retrackers/dispatch.php`
entrypoints are launched. Every path ancestor and the final file must retain
trusted ownership and permissions; symlinks, nonregular leaves, hardlinks,
setuid/setgid executables and file capabilities are refused. Non-Linux worker
launch is unsupported rather than silently weakening the parent-death contract.

`d.erase_preserving_tied` checks the expected tied path, Stop token and
state/open/active tuple before erasure. Its scoped tied-file deletion guard
covers retie hooks and nested guarded erasures while unrelated erase hooks run.
Scheduled command errors also emit a bounded classified key/reason to stderr
and the error log; raw commands and remote error text are not added there.

### Native AutoTools Move jobs

`system.autotools.move_queue` advertises the trusted Move queue ABI. Finished
hooks call `d.autotools.move_enqueue(user)` without spawning a process or
saving the session. A private, single-link 0600
`.rutorrent-stop-intents/H.move` record stores
`m1:sha256(user):Q:local_id:enqueue_time:retry` followed by a newline. Its Q and
retry survive internal retrackers erase/load and cold restart. Malformed or
unsafe records are retained with a visible classified refusal naming the file.

`system.autotools.move_dispatch(user)` launches the fixed
`plugins/autotools/move_worker.php H Q user I` actor. Move shares the native
pool's eight worker reservations and separate dispatcher slot; at most two
Move workers run. Reservations remain occupied through reap. Actor paths
still come only from daemon startup policy. These commands are trusted SCGI
commands and must not be exposed through application HTTP proxy allowlists.

`move_begin(Q,I)` checks the current incarnation and exact queued Q, then
reuses the exclusive stop lease. Repeating it completes interrupted quiescence
without replacing its original run-state receipt. Retrackers capture and file
operation leases exclude each other; a pending Move record alone does not
block durable retrackers enqueue. Multi-file directory publication must use
`d.directory.base.set_if_stop_close_claim` with the actual root, avoiding an
extra torrent-name component. Late user Stop remains authoritative.

The `d.autotools.move_defer(Q,expected,next)` CAS accepts Move retry reasons
`preflight-refused`, `storage-failed`, and `worker-exit`, with six counted
attempts and delays of 60, 120, 240, 480, and 960 seconds. Noncounted
`source-not-ready` and `transport-unavailable` waits may start at count zero;
existing retrackers r1 receipts retain their original count-one grammar.
An unexpected worker exit spends an attempt, while exit zero without an
outcome waits before relaunch. A terminal leased job keeps its lease and
payload recovery journal. `system.autotools.move_reset(H)` clears only the
terminal retry, allowing the same Q and lease to resume recovery.
`system.autotools.move_owner(H)` returns its user digest for trusted
operator tooling, including held records whose state has no owner field.

`move_finish(Q)` and `move_cancel(Q,reason)` require the exact record and no
lease. User erase retires a pending record; internal capture preserves it.
A datadir operation publishes its own exclusive lease before retiring a
queued Move. If interrupted between those writes, dispatch sees that exact
foreign lease and finishes superseding the old record. Unleased, nonterminal
jobs expire after 24 hours with a visible outcome; terminal records remain
until explicit reset or retirement.

An enqueue storage refusal is projected from retained custom metadata even
when tracker activity clears the volatile message. For retrackers, repair
session storage and use `tools/retrackers-apply-existing.php`; successful
native enqueue clears the owned refusal. No metadata-erasing re-add is needed.
