#!/bin/sh
set -eu
mkdir -p /run/rtorrent /data/.session
chown -R 991:991 /run/rtorrent /data
chmod "${TEST_SESSION_MODE:-2755}" /data/.session
cat > /tmp/native-test.rc <<'EOF'
system.daemon.set = true
session.path.set = /data/.session
directory.default.set = /data
network.scgi.open_local = /run/rtorrent/rtorrent.sock
schedule = chmod_scgi_socket,0,0,"execute.nothrow=chmod,660,/run/rtorrent/rtorrent.sock"
network.listen.port.range.set = 45551-45551
network.block.ipv6.set = no
dht.mode.set = disable
protocol.pex.set = no
log.open_file.flush = "native", /tmp/native.log
log.add_output = "error", "native"
EOF
native_binary=/native-bin
[ -x "$native_binary" ] || native_binary=/usr/local/bin/rtorrent
exec su-exec 991:991 "$native_binary" -n -o import=/tmp/native-test.rc
