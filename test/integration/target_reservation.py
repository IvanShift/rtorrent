#!/usr/bin/env python3
"""Real-daemon regression for invalid retrackers successor reservations.

Run with a Docker image built from this checkout (UID 991, /data/.watch,
/tmp/rtorrent.log). The disposable container enables synchronous error logging:
    python3 test/integration/target_reservation.py IMAGE
"""

import hashlib
import pathlib
import subprocess
import sys
import tempfile
import time
import uuid


def docker(*args, check=True):
    return subprocess.run(
        ["docker", *args], text=True, capture_output=True, check=check
    )


def encode(value):
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    return b"d" + b"".join(
        encode(key) + encode(item) for key, item in sorted(value.items())
    ) + b"e"


def torrent_file(directory, name):
    info = {
        b"length": 1,
        b"name": name.encode(),
        b"piece length": 16384,
        b"pieces": hashlib.sha1(b"x").digest(),
    }
    metadata = encode({b"announce": b"http://127.0.0.1:9/announce", b"info": info})
    path = directory / (name + ".torrent")
    path.write_bytes(metadata)
    return path, hashlib.sha1(encode(info)).hexdigest().upper()


def inside(container, *args, check=False):
    return docker("exec", container, *args, check=check)


def alive(container):
    return inside(container, "pidof", "rtorrent").returncode == 0


def file_text(container, path):
    result = docker("exec", container, "cat", path, check=False)
    return result.stdout if result.returncode == 0 else ""


def wait_for(container, predicate, description, timeout=45, require_alive=True):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if require_alive and not alive(container):
            raise AssertionError("rTorrent daemon stopped while waiting for " + description)
        if predicate():
            return
        time.sleep(0.25)
    raise AssertionError("Timed out waiting for " + description)


def write_marker(container, path, content, mode="600"):
    script = "printf '%s' '" + content + "' > '" + path + "'; chmod " + mode + " '" + path + "'"
    docker("exec", "--user", "991:991", container, "sh", "-c", script)


def run_case(container, directory, name, marker, mode, reason):
    torrent, infohash = torrent_file(directory, name)
    reservation = "/data/.session/.retrackers-migration/" + infohash + ".target"
    write_marker(container, reservation, marker, mode)
    docker("cp", str(torrent), container + ":/data/.watch/" + torrent.name)
    expected = "RR migration reservation load refused: " + infohash + ": " + reason
    wait_for(container, lambda: expected in file_text(container, "/tmp/rtorrent.log"), expected)
    if file_text(container, reservation) != marker:
        raise AssertionError("Invalid reservation was modified: " + infohash)
    if inside(container, "test", "-e", "/data/.session/" + infohash + ".torrent").returncode == 0:
        raise AssertionError("Torrent with invalid reservation was loaded: " + infohash)


def run_indirect_case(container, directory):
    torrent, infohash = torrent_file(directory, "indirect.bin")
    base = "/data/.session/.retrackers-migration/" + infohash
    write_marker(container, base + ".rollback.torrent", "broken")
    write_marker(container, base + ".released.receipt", "released")
    docker("cp", str(torrent), container + ":/data/.watch/" + torrent.name)
    expected = "RR migration reservation load refused: " + infohash + ": RR migration snapshot is missing or unsafe."
    wait_for(container, lambda: expected in file_text(container, "/tmp/rtorrent.log"), expected)
    if file_text(container, base + ".rollback.torrent") != "broken":
        raise AssertionError("Source rollback marker was modified")
    if inside(container, "test", "-e", "/data/.session/" + infohash + ".torrent").returncode == 0:
        raise AssertionError("Source with invalid migration journal was loaded")


def main(image, indirect_only=False):
    unique = "rr-target-" + uuid.uuid4().hex[:12]
    config, data = unique + "-config", unique + "-data"
    container = unique
    try:
        docker("volume", "create", config)
        docker("volume", "create", data)
        docker(
            "run", "-d", "--name", container, "--network", "none",
            "--mount", "type=volume,source=" + config + ",target=/config",
            "--mount", "type=volume,source=" + data + ",target=/data",
            "-e", "HTTP_AUTH=false", "-e", "ALLOW_UNAUTHENTICATED_HTTP=true",
            "--entrypoint", "sh", image, "-c",
            "sed -i 's/^log.open_file =/log.open_file.flush =/' /home/torrent/.rtorrent.rc; "
            "exec /usr/local/bin/startup /usr/bin/s6-svscan /etc/s6.d",
        )
        with tempfile.TemporaryDirectory(prefix="rr-target-") as temporary:
            directory = pathlib.Path(temporary)
            control, control_hash = torrent_file(directory, "control.bin")
            wait_for(container, lambda: alive(container), "daemon startup", require_alive=False)
            wait_for(container, lambda: inside(container, "test", "-d", "/data/.watch").returncode == 0, "watch directory")
            docker("cp", str(control), container + ":/data/.watch/" + control.name)
            wait_for(
                container,
                lambda: inside(container, "test", "-e", "/data/.session/" + control_hash + ".torrent").returncode == 0,
                "control torrent",
            )
            if not indirect_only:
                run_case(container, directory, "malformed.bin", "broken", "600", "RR successor reservation invalid.")
                run_case(container, directory, "unsafe.bin", "broken", "644", "RR migration snapshot is missing or unsafe.")
            run_indirect_case(container, directory)
            if not alive(container):
                raise AssertionError("rTorrent daemon stopped after invalid reservations")
            if inside(container, "test", "-e", "/data/.session/" + control_hash + ".torrent").returncode != 0:
                raise AssertionError("Unrelated control torrent disappeared")
            scope = "indirect" if indirect_only else "direct and indirect"
            print("PASS: invalid " + scope + " reservations held; daemon and unrelated torrent survived")
    except Exception:
        print(docker("logs", container, check=False).stdout, file=sys.stderr)
        print(file_text(container, "/tmp/rtorrent.log"), file=sys.stderr)
        raise
    finally:
        docker("rm", "-f", container, check=False)
        docker("volume", "rm", config, check=False)
        docker("volume", "rm", data, check=False)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--indirect-only"):
        raise SystemExit("usage: target_reservation.py IMAGE [--indirect-only]")
    main(sys.argv[1], len(sys.argv) == 3)
