# LocalPath –  effortless local network file sharing  

**LocalPath** is a lightweight, self‑contained file server for your local network.  
Drop files, browse folders, preview media, and download entire directories as `.tar` – all through a clean, responsive web interface.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![C++](https://img.shields.io/badge/C++-17-blue)
![Boost](https://img.shields.io/badge/Boost-1.80+-green)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)

---

## Features

- **Zero config** – run the executable, open the displayed IP address
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

- **C++17** compiler (GCC 8+, Clang 7+, MSVC 2019+) 
---

## Build & Run

### Linux / macOS

```bash
# download repo
git clone https://github.com/ScavyXYZ/LocalPath.git
cd LocalPath/LocalPath

# Compile
g++ -Ofast -march=native -flto -Wno-ltomain main.cpp -I ./boost_min -o LocalPath

# Run (default port 8080)
./LocalPath

# or specify a custom port
./LocalPath 3000
```

### Windows (MSVC + vcpkg)

```powershell
# download repo
git clone https://github.com/ScavyXYZ/LocalPath.git
cd LocalPath\LocalPath

# Compile (Developer Command Prompt)
g++ -Ofast -march=native -flto -Wno-ltomain main.cpp -I ./boost_min -o LocalPath -lws2_32

# Run port - 8080
.\LocalPath.exe

#or specify a custom port
.\LocalPath 3000
```
---

## 🚀 Usage

1. **Start the server** – it creates an `uploads/` folder next to the executable.
2. **Open the URL** shown in the console (e.g., `http://192.168.1.100:8080`).
3. **Upload** – drag files/folders onto the page or click the upload zone.
4. **Navigate** – click on folder names to enter, use breadcrumbs to go back.
5. **Preview** – click the **👁 view** button on supported files.
6. **Download** – use `get` for single files, `tar` for entire folders, or the “download all” button for the current folder.
7. **Delete** – click the `X` button (confirmation required).
8. **Create file** – use the “+ new file” button to add a text file directly in the current folder.

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

---

## Configuration

- **Port** – first command line argument (default `8080`).
- **Upload directory** – fixed to `uploads/` (created automatically).
- **Custom `index.html`** – place your own `index.html` next to the executable to override the built‑in fallback.

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

Distributed under the **MIT License**. See `LICENSE` for more information.

---

**Made for your local network.**
*Share files without cloud, without hassle.*
