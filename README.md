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
`renameat2(RENAME_NOREPLACE)` and requires kernel support. Journal and claim opens refuse symlinks;
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
parent-death SIGKILL, and direct argv without a shell. No PID timeout or process
exit retires an unknown captured migration obligation. Pending Q intent and
exact completion receipts are durable across daemon restart. Original release
receipts attest completed Q only after an explicit application finish; cold
recovery and rollback retire ownership while leaving unprocessed Q pending.
Trusted `d.retrackers.cancel_original(H,T,I,Q)` requires exact current preclaim
T/I/Q/U/L and marker/ACK, no capsule/release/file lease, no completed marker
and no active insert hook. It durably preserves the live source and pending Q
while retiring only that observation and its pending hook key. Unknown work
stays held; cancellation never acknowledges tracker processing.

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
`RR_MIG_TEST_CUT`; production builds ignore it. Run `make check` for unit tests.
Run the current-master daemon gates with an existing builder image that supplies
its matching libtorrent ABI and C++ compiler:

```
python3 test/integration/native_regressions.py BUILDER_IMAGE BUILD_TREE SCRATCH
```

`BUILD_TREE/src/rtorrent` must contain the exact candidate binary. The runner
uses disposable network-isolated containers, a synthetic local metadata peer,
and a separate Unix UID for asynchronous path refusal. For a dedicated test-cut
build, also select `owner-cut-before owner-cut-after owner-cut-retry` as trailing
arguments. These cuts are excluded from the default production run.

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
admits one separate dispatcher. Duplicate launches reuse their exact identity;
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
