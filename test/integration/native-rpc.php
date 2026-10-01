<?php
// Standalone test client: exercise daemon persistence without application APIs.
$socket = stream_socket_client('unix:///run/rtorrent/rtorrent.sock', $errno, $error, 3);
if ($socket === false) {
    fwrite(STDERR, "$error\n");
    exit(1);
}
stream_set_timeout($socket, 5);
$xml = '<?xml version="1.0"?><methodCall><methodName>' . htmlspecialchars($argv[1], ENT_XML1) . '</methodName><params>';
foreach (array_slice($argv, 2) as $arg) {
    $xml .= '<param><value><string>' . htmlspecialchars($arg, ENT_XML1) . '</string></value></param>';
}
$xml .= '</params></methodCall>';
$headers = "CONTENT_LENGTH\0" . strlen($xml) . "\0SCGI\0" . "1\0REQUEST_METHOD\0POST\0REQUEST_URI\0/RPC2\0";
$request = strlen($headers) . ':' . $headers . ',' . $xml;
while ($request !== '') {
    $size = fwrite($socket, $request);
    if ($size === false || $size === 0) {
        exit(1);
    }
    $request = substr($request, $size);
}
$response = stream_get_contents($socket, 65536);
fclose($socket);
$start = strpos($response, '<?xml');
if ($start === false) {
    exit(1);
}
$result = simplexml_load_string(substr($response, $start), SimpleXMLElement::class, LIBXML_NONET);
if ($result === false || isset($result->fault)) {
    fwrite(STDERR, $response . "\n");
    exit(1);
}
$value = $result->params->param->value;
echo (string) ($value->string ?? $value->i8 ?? $value->int ?? $value->i4 ?? '') . "\n";
