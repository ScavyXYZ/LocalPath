# LocalPath –  effortless local network file sharing  

**LocalPath** is a lightweight, self‑contained file server for your local network.  
Drop files, browse folders, preview media, and download entire directories as `.tar` – all through a clean, responsive web interface.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![C++](https://img.shields.io/badge/C++-17-blue)
![Boost](https://img.shields.io/badge/Boost-1.90-green)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)

---

## Features

- **Zero config** – run the executable, scan the terminal QR code or open the tokenized link
- **Link-based access** – a fresh random token is generated on each launch; requests without it are denied
- **Upload files & folders** via drag‑and‑drop or file picker
- **Nested folder support** – create and navigate directory trees
- **Inline preview** for images, videos, audio, PDFs, and text files
- **Download single files** or **entire folders as .tar**
- **Delete** files / folders (with confirmation)
- **Create text files** directly from the browser
- **Search & sort** by name, size, date
- **Mobile‑friendly** adaptive layout
- **Auto‑refresh** – changes appear automatically

---

## Requirements

- **C++17** compiler and CMake 3.16+; Boost 1.90 headers are bundled in `LocalPath/boost_min/`.
---

## Build & Run

### Linux

```bash
git clone https://github.com/ScavyXYZ/LocalPath.git
cd LocalPath

cmake -S . -B build
cmake --build build

# Run from the build directory (contains index.html)
cd build
./LocalPath

# or specify a custom port
./LocalPath 3000
```

### Windows (MSVC or MinGW)

```powershell
git clone https://github.com/ScavyXYZ/LocalPath.git
cd LocalPath

cmake -S . -B build
cmake --build build --config Release

# Run from build/ (contains index.html).
cd build
.\Release\LocalPath.exe

#or specify a custom port
.\Release\LocalPath.exe 3000
```
With single-configuration generators (e.g. MinGW Makefiles), the executable and copied `index.html` are directly in `build/` instead of `build/Release/`.

### Tests

```bash
ctest --test-dir build -C Release --output-on-failure
```

Tests require Python 3. CI builds and tests on Linux and Windows.

Optional browser UI smoke test (requires Playwright and its Chromium browser):

```bash
python tests/ui_smoke.py build/LocalPath
```

On Windows, use the path to your built `LocalPath.exe` instead.

> **Security:** the server binds to `0.0.0.0`. Anyone with the QR code, tokenized link, or an existing session cookie can read, upload, and delete files. The token is regenerated on restart, but the connection uses **unencrypted HTTP**, so someone who can observe your local network traffic could capture it. Use only on a trusted network; do not expose the port to the Internet. There is no per-user access control.
---

## 🚀 Usage

1. **Start the server** from its executable directory – it creates an `uploads/` folder in the current working directory.
2. **Scan the QR code** in the console or open its full `http://.../?token=...` link. The local link works on the server machine; the network link is for other devices. The URL remains visible above logs on a compatible terminal (otherwise it is reprinted after logs).
3. **Share the link only with trusted people.** The first visit exchanges the token for an HttpOnly session cookie and removes the token from the address bar. Opening the bare IP/port without a cookie returns HTTP 401.
4. **Upload** – drag files/folders onto the page or click the upload zone.
5. **Navigate** – click on folder names to enter, use breadcrumbs to go back.
6. **Preview, download, or delete** files with the buttons beside them.
7. **Create a text file** with the “New file” button.

> All uploaded files are stored inside the `uploads/` directory, preserving folder structures.

---

## API Endpoints (for developers)

| Endpoint                | Method | Description                                      |
|-------------------------|--------|--------------------------------------------------|
| `/` or `/index.html`    | GET    | Serves the web interface (custom `index.html`)  |
| `/upload?path=<rel>`    | POST   | Accepts `multipart/form-data` file upload       |
| `/api/files?path=<rel>` | GET    | Returns JSON listing of files/folders           |
| `/download?file=<rel>`  | GET    | Downloads a single file (inline or attachment)  |
| `/archive?path=<rel>`   | GET    | Downloads a `.tar` archive of the folder        |
| `/delete`               | POST   | Deletes a file or folder (body: `file=<rel>`)   |

- `<rel>` – path relative to the `uploads/` directory, URL‑encoded.
- Every endpoint requires the `LocalPathSession` cookie; open the tokenized URL first to obtain it. A token in an API query string is not accepted.

---

## Configuration

- **Port** – first command line argument (default `8080`).
- **Upload directory** – fixed to `uploads/` (created automatically).
- **Custom `index.html`** – place your own `index.html` in the current working directory to override the built‑in fallback.
- **Transfer memory** – file uploads, downloads and TAR archives are processed in chunks; TAR currently skips paths longer than 99 bytes and does not include empty directories.

---

## File Structure

```
.
├── localpath            # compiled executable
├── index.html           # UI
└── uploads/             # all shared files go here
    ├── myfile.txt
    ├── photos/          # subfolders are supported
    │   └── vacation.jpg
    └── ...
```

---

## License

Distributed under the **MIT License**. See `LICENSE` for more information. The bundled [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki is also MIT-licensed (license notice in `LocalPath/third_party/qrcodegen/`).

---

**Made for your local network.**
*Share files without cloud, without hassle.*
