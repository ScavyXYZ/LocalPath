"""Optional browser smoke test: python tests/ui_smoke.py <executable> [screenshot-dir]."""
import shutil
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from playwright.sync_api import sync_playwright


html = Path(__file__).resolve().parents[1] / "LocalPath" / "index.html"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    shutil.copy2(html, root / "index.html")
    uploads = root / "uploads"
    uploads.mkdir()
    (uploads / "Photos").mkdir()
    (uploads / "Photos" / "sunrise.jpg").write_bytes(b"image")
    (uploads / "Project notes.txt").write_text("A shared document", encoding="utf-8")
    (uploads / "quarterly-report.pdf").write_bytes(b"pdf")
    (uploads / "design-assets.zip").write_bytes(b"zip")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    console = (root / "console.log").open("w+b", buffering=0)
    server = subprocess.Popen([str(Path(sys.argv[1]).resolve()), str(port)], cwd=root,
                              stdout=console, stderr=console)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", port), 0.1):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("LocalPath did not start")
        for _ in range(100):
            output = (root / "console.log").read_text(encoding="utf-8", errors="replace")
            match = re.search(r"Local:\s+(http://127\.0\.0\.1:\d+/\?token=[a-f0-9]{32})", output)
            if match:
                access_url = match.group(1)
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Tokenized URL was not printed")
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch()
            page = browser.new_page(viewport={"width": 1440, "height": 960}, device_scale_factor=1)
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(access_url)
            assert page.url == f"http://127.0.0.1:{port}/"
            page.get_by_role("link", name="Project notes.txt").wait_for()
            assert page.get_by_text("4 items in this folder").count() == 1
            page.get_by_role("button", name="New file").click()
            assert page.get_by_role("dialog", name="Create new file").is_visible()
            page.keyboard.press("Escape")
            assert not page.get_by_role("dialog", name="Create new file").is_visible()
            assert page.get_by_role("button", name="New file").evaluate("el => el === document.activeElement")
            if len(sys.argv) > 2:
                output = Path(sys.argv[2]); output.mkdir(parents=True, exist_ok=True)
                page.screenshot(path=str(output / "ui-desktop.png"), full_page=True)
            page.get_by_role("button", name="Photos", exact=True).click()
            page.get_by_role("button", name="All files").click()
            page.get_by_role("link", name="Project notes.txt").wait_for()
            page.get_by_role("searchbox", name="Search files in this folder").fill("Project")
            assert page.get_by_role("link", name="Project notes.txt").is_visible()
            assert not page.get_by_role("link", name="quarterly-report.pdf").is_visible()
            page.get_by_role("searchbox", name="Search files in this folder").fill("")
            page.get_by_role("button", name="New file").click()
            page.get_by_label("File name").fill("Created in browser.txt")
            page.get_by_role("button", name="Create file").click()
            page.get_by_role("link", name="Created in browser.txt").wait_for()
            mobile = browser.new_page(viewport={"width": 375, "height": 812}, device_scale_factor=1)
            mobile.on("pageerror", lambda error: errors.append(str(error)))
            mobile.goto(access_url)
            mobile.locator("#mobileCards .card-name").get_by_text("Project notes.txt").wait_for()
            assert not mobile.evaluate("document.documentElement.scrollWidth > window.innerWidth")
            mobile.get_by_role("combobox", name="Sort files").select_option("name-asc")
            assert mobile.locator("#mobileCards .card-name").first.inner_text().strip() == "Photos"
            if len(sys.argv) > 2:
                mobile.screenshot(path=str(output / "ui-mobile.png"), full_page=True)
            mobile.set_viewport_size({"width": 320, "height": 720})
            assert not mobile.evaluate("document.documentElement.scrollWidth > window.innerWidth")
            assert not errors, errors
            browser.close()
        print("UI smoke test passed (desktop, 375px/320px mobile, navigation, search, sort, creation, modal keyboard focus)")
    finally:
        server.terminate()
        server.wait(timeout=5)
        console.close()
