"""Verify the QR/link panel stays pinned above logs on a Unix terminal."""
import fcntl
import http.client
import os
import pty
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time


def read_until(fd, marker, timeout=10):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while marker not in data and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
        if ready:
            data.extend(os.read(fd, 16384))
    assert marker in data, "Terminal output is missing the expected marker"
    return bytes(data)


with tempfile.TemporaryDirectory() as directory:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 45, 120, 0, 0))
    server = subprocess.Popen([sys.argv[1], str(port)], cwd=directory,
                              stdout=slave, stderr=slave, stdin=subprocess.DEVNULL)
    os.close(slave)
    try:
        output = read_until(master, b";1H", 15)
        assert re.search(rb"\x1b\[\d+;45r", output), "The log scroll region was not pinned"
        match = re.search(rb"Local:\s+http://127\.0\.0\.1:\d+/\?token=([a-f0-9]{32})", output)
        assert match, "Missing access link"
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        connection.request("GET", "/?token=" + match.group(1).decode())
        response = connection.getresponse()
        assert response.status == 303
        cookie = response.getheader("Set-Cookie").split(";", 1)[0]
        response.read(); connection.close()
        boundary = "pin-test"
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
                'filename="test.txt"\r\n\r\nhello\r\n--pin-test--\r\n').encode()
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        connection.request("POST", "/upload?path=", body,
                           {"Cookie": cookie, "Content-Type": "multipart/form-data; boundary=" + boundary})
        response = connection.getresponse()
        assert response.status == 200
        response.read(); connection.close()
        log = read_until(master, b"[upload]", 5)
        assert b"Network:" not in log, "Pinned panel was printed again instead of staying in place"
    finally:
        server.terminate()
        server.wait(timeout=5)
        os.close(master)
print("Terminal QR/link panel remains pinned above logs")
