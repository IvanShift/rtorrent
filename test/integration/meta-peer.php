<?php
$info = file_get_contents($argv[1] ?? '/data/native-info');
$port = (int)($argv[2] ?? '45552');
$prefix = $argv[3] ?? 'meta-peer';
$hash = sha1($info, true);
$server = stream_socket_server('tcp://127.0.0.1:' . $port, $errno, $error);
if (!$server) { fwrite(STDERR, "$error\n"); exit(1); }
file_put_contents('/data/' . $prefix . '-ready', 'ready');
$peer = stream_socket_accept($server, 20);
if (!$peer) { exit(2); }
stream_set_timeout($peer, 20);
function read_bytes($peer, $size) {
    $out = '';
    while (strlen($out) < $size) {
        $part = fread($peer, $size - strlen($out));
        if ($part === false || $part === '') { throw new RuntimeException('peer closed'); }
        $out .= $part;
    }
    return $out;
}
function message($peer, $payload) { fwrite($peer, pack('N', strlen($payload)) . $payload); }
$handshake = read_bytes($peer, 68);
fwrite($peer, chr(19) . 'BitTorrent protocol' . "\0\0\0\0\0\x10\0\0" . $hash . '-RRTEST-000000000000');
message($peer, chr(20) . chr(0) . 'd1:md11:ut_metadatai1ee13:metadata_sizei' . strlen($info) . 'e1:v11:native-test' . 'e');
$remoteId = null;
while (true) {
    $size = unpack('Nsize', read_bytes($peer, 4))['size'];
    if ($size === 0) { continue; }
    if ($size > 65536) { exit(3); }
    $payload = read_bytes($peer, $size);
    if ($payload[0] !== chr(20)) { continue; }
    if ($payload[1] === chr(0) && preg_match('/11:ut_metadatai([0-9]+)e/', substr($payload, 2), $match)) {
        $remoteId = (int)$match[1];
    }
    if ($payload[1] === chr(1) && $remoteId !== null) {
        message($peer, chr(20) . chr($remoteId) . 'd8:msg_typei1e5:piecei0e10:total_sizei' . strlen($info) . 'ee' . $info);
        file_put_contents('/data/' . $prefix . '-sent', 'sent');
        sleep(1);
        break;
    }
}
