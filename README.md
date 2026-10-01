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
rename, `fsync`, and `fdatasync`. The daemon must own the session directory and
its journals. Group/other write access is refused; setgid session directories
are supported. Stop intent directories are private 0700 (setgid is allowed),
and records are single-link files with mode 0600. Filesystems that discard
these permissions, and mounts with incompatible ownership mapping, require
operator correction rather than an automatic fallback. Refusals include the
path and expected ownership/mode.

Stop generations are published as one atomic 17-byte record. A new same-hash
load starts at the current generation; erasure retires its record unless an
unresolved retrackers migration still needs it. Startup removes safe orphaned
records and unpublished temporary files after migration recovery. User Stop
commands remain synchronous so a later crash cannot silently revoke a Stop.

Destructive migration failpoints are disabled by default. Only dedicated test
builds configured with `--enable-rr-migration-test-cuts` honor
`RR_MIG_TEST_CUT`; production builds ignore it. Run `make check` for unit tests.
The integration fixture in `test/integration/native_state.py` uses an isolated
container and a synthetic local metadata peer, without public tracker traffic.
Run `python3 test/integration/meta_callback.py` for callback lifetime and Stop
transfer regressions against the actual function body using non-deleting test
doubles; this focused check requires a host C++ compiler.

Image pins use full commit SHAs. Keep every shipped pin reachable from an
immutable published release tag; create and publish that tag only as part of
the owner-approved release workflow. A moving branch tip alone is not a
long-term source archive.
