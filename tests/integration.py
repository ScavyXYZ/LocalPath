"""Black-box filesystem and streaming regressions; run with the executable path."""
import http.client
import io
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from urllib.parse import quote, urlencode


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.uploads = cls.root / "uploads"
        cls.uploads.mkdir()
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            cls.port = s.getsockname()[1]
        cls.console = (cls.root / "console.log").open("w+b", buffering=0)
        cls.proc = subprocess.Popen([sys.argv[1], str(cls.port)], cwd=cls.root,
                                    stdout=cls.console, stderr=cls.console)
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", cls.port), 0.1):
                    break
            except OSError:
                if cls.proc.poll() is not None:
                    raise RuntimeError("Server exited during startup")
                time.sleep(0.05)
        else:
            raise RuntimeError("Server did not start")
        for _ in range(100):
            output = (cls.root / "console.log").read_text(encoding="utf-8", errors="replace")
            match = re.search(r"Local:\s+http://127\.0\.0\.1:\d+/\?token=([a-f0-9]{32})", output)
            if match:
                cls.token = match.group(1)
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Server did not display a tokenized link")
        connection = http.client.HTTPConnection("127.0.0.1", cls.port)
        connection.request("GET", "/?token=" + cls.token)
        response = connection.getresponse()
        assert response.status == 303 and response.getheader("Location") == "/"
        cls.cookie = response.getheader("Set-Cookie").split(";", 1)[0]
        assert "HttpOnly" in response.getheader("Set-Cookie")
        assert "SameSite=Lax" in response.getheader("Set-Cookie")
        response.read()
        connection.close()

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.proc.kill()
            cls.proc.wait()
        cls.console.close()
        cls.tmp.cleanup()

    def request(self, method, path, body=None, headers=None, authenticated=True):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        headers = dict(headers or {})
        if authenticated:
            headers["Cookie"] = self.cookie
        connection.request(method, path, body, headers)
        response = connection.getresponse()
        status, data = response.status, response.read()
        connection.close()
        return status, data

    def test_access_requires_token(self):
        protected = self.uploads / "protected.txt"
        protected.write_text("keep", encoding="utf-8")
        for path in ("/", "/index.html", "/api/files?path=", "/download?file=empty.txt", "/archive?path="):
            self.assertEqual(self.request("GET", path, authenticated=False)[0], 401)
        self.assertEqual(self.request("GET", "/?token=wrong", authenticated=False)[0], 401)
        self.assertEqual(self.request("GET", "/api/files?path=&token=" + self.token, authenticated=False)[0], 401)
        self.assertEqual(self.request("GET", "/", headers={"Cookie": "LocalPathSession=wrong"}, authenticated=False)[0], 401)
        self.assertEqual(self.request("POST", "/delete", "file=protected.txt", authenticated=False)[0], 401)
        self.assertEqual(self.request("POST", "/upload?path=", b"data", authenticated=False)[0], 401)
        self.assertEqual(protected.read_text(encoding="utf-8"), "keep")
        self.assertEqual(self.request("GET", "/", authenticated=True)[0], 200)

    def test_console_qr_decodes_to_network_link(self):
        try:
            import cv2
            import numpy as np
        except ImportError:
            self.skipTest("OpenCV is not installed (optional QR scan check)")
        output = (self.root / "console.log").read_text(encoding="utf-8", errors="replace")
        qr_lines = output.split("Scan to open on your network\n", 1)[1].split("\n  Network:", 1)[0].splitlines()
        self.assertTrue(qr_lines)
        width = (len(qr_lines[0]) - 2) // 2
        self.assertEqual(len(qr_lines), width)
        pixels = np.array([[0 if line[2 + x * 2:4 + x * 2] == "##" else 255
                            for x in range(width)] for line in qr_lines], dtype=np.uint8)
        network = re.search(r"Network:\s+(http://[^\s]+)", output).group(1)
        decoded = ""
        for scale in (8, 10, 12, 16):
            image = cv2.resize(pixels, (width * scale, width * scale), interpolation=cv2.INTER_NEAREST)
            decoded, _, _ = cv2.QRCodeDetector().detectAndDecode(image)
            if decoded: break
        self.assertEqual(decoded, network)

    def upload(self, name, content, folder=""):
        boundary = "localpath-test-boundary"
        payload = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
                   f'filename="{name}"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
        payload += content + f'\r\n--{boundary}--\r\n'.encode()
        return self.request("POST", "/upload?path=" + quote(folder), payload,
                            {"Content-Type": "multipart/form-data; boundary=" + boundary})

    def test_large_upload_download_and_archive(self):
        content = (b"0123456789abcdef" * 262144) + b"end"
        self.assertEqual(self.upload("large.bin", content, "nested")[0], 200)
        self.assertEqual(self.request("GET", "/download?file=nested%2Flarge.bin"), (200, content))
        status, data = self.request("GET", "/archive?path=nested")
        self.assertEqual(status, 200)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            self.assertEqual(archive.extractfile("large.bin").read(), content)

    def test_empty_file_and_json_escaping(self):
        self.assertEqual(self.upload("empty.txt", b"")[0], 200)
        self.assertEqual(self.request("GET", "/download?file=empty.txt"), (200, b""))
        (self.uploads / "apostrophe's.txt").write_text("safe", encoding="utf-8")
        if os.name != "nt":
            (self.uploads / "line\nbreak.txt").write_text("safe", encoding="utf-8")
        status, data = self.request("GET", "/api/files?path=")
        self.assertEqual(status, 200)
        names = {entry["name"] for entry in json.loads(data)}
        self.assertIn("apostrophe's.txt", names)
        if os.name != "nt":
            self.assertIn("line\nbreak.txt", names)

    def test_traversal_and_root_delete(self):
        outside = self.root / "outside.txt"
        outside.write_text("keep", encoding="utf-8")
        sibling = self.root / "uploads-other"
        sibling.mkdir()
        (sibling / "private.txt").write_text("keep", encoding="utf-8")
        self.assertNotEqual(self.request("GET", "/download?file=..%2Foutside.txt")[0], 200)
        self.assertNotEqual(self.request("GET", "/download?file=..%2Fuploads-other%2Fprivate.txt")[0], 200)
        self.assertNotEqual(self.request("GET", "/archive?path=..")[0], 200)
        self.assertNotEqual(self.upload("evil.txt", b"evil", "..")[0], 200)
        self.assertNotEqual(self.upload("../outside.txt", b"evil")[0], 200)
        self.request("POST", "/delete", urlencode({"file": "../outside.txt"}),
                     {"Content-Type": "application/x-www-form-urlencoded"})
        self.request("POST", "/delete", urlencode({"file": "."}),
                     {"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(outside.read_text(encoding="utf-8"), "keep")
        self.assertEqual((sibling / "private.txt").read_text(encoding="utf-8"), "keep")
        self.assertTrue(self.uploads.is_dir())

    def test_empty_archive(self):
        (self.uploads / "empty-folder").mkdir(exist_ok=True)
        status, data = self.request("GET", "/archive?path=empty-folder")
        self.assertEqual(status, 200)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            self.assertEqual(archive.getnames(), [])

    def test_failed_upload_does_not_report_success(self):
        (self.uploads / "blocked").write_bytes(b"existing file")
        self.assertNotEqual(self.upload("new.txt", b"data", "blocked")[0], 200)
        self.assertEqual((self.uploads / "blocked").read_bytes(), b"existing file")
        self.assertNotEqual(self.request("POST", "/upload?path=", b"broken",
                         {"Content-Type": "multipart/form-data; boundary=broken"})[0], 200)

    def test_symlink_escape(self):
        outside = self.root / "private"
        outside.mkdir()
        (outside / "secret.txt").write_text("secret", encoding="utf-8")
        try:
            (self.uploads / "link").symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("Symlinks unavailable")
        self.assertNotEqual(self.request("GET", "/download?file=link%2Fsecret.txt")[0], 200)
        self.assertNotEqual(self.upload("leak.txt", b"no", "link")[0], 200)
        self.request("POST", "/delete", urlencode({"file": "link"}),
                     {"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual((outside / "secret.txt").read_text(encoding="utf-8"), "secret")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
