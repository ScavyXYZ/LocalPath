#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/config.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <cstring>
#include <array>
#include <optional>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <csignal>
#include <cstdlib>
#include "third_party/qrcodegen/qrcodegen.hpp"


#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <net/if.h>

typedef int SOCKET;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace fs = std::filesystem;
using tcp = boost::asio::ip::tcp;

static const std::string UPLOAD_DIR = "uploads";
static std::mutex        g_dir_mutex;
static std::string       g_index_template;
static std::atomic<uint64_t> g_upload_id{0};
static std::string g_session_token;

static std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static bool is_usable_lan_ip(uint32_t address) {
    const uint32_t host = ntohl(address);
    if ((host >> 24) == 127 || (host & 0xffff0000U) == 0xa9fe0000U || host == 0) return false;
    return (host & 0xff000000U) == 0x0a000000U ||
           (host & 0xfff00000U) == 0xac100000U ||
           (host & 0xffff0000U) == 0xc0a80000U;
}

std::string get_local_ip() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return "127.0.0.1";
    ULONG size = 16 * 1024;
    std::vector<unsigned char> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &size);
    }
    std::string best;
    uint64_t best_score = std::numeric_limits<uint64_t>::max();
    if (result == NO_ERROR) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                adapter->IfType == IF_TYPE_TUNNEL) continue;
            bool has_gateway = false;
            for (auto* gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
                if (!gateway->Address.lpSockaddr || gateway->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* ipv4 = reinterpret_cast<sockaddr_in*>(gateway->Address.lpSockaddr);
                if (ipv4->sin_addr.s_addr != 0) {
                    has_gateway = true;
                    break;
                }
            }
            const bool physical = adapter->IfType == IF_TYPE_ETHERNET_CSMACD ||
                                  adapter->IfType == IF_TYPE_IEEE80211;
            std::wstring adapter_text;
            if (adapter->FriendlyName) adapter_text += adapter->FriendlyName;
            adapter_text += L" ";
            if (adapter->Description) adapter_text += adapter->Description;
            std::transform(adapter_text.begin(), adapter_text.end(), adapter_text.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            const bool virtual_adapter =
                adapter_text.find(L"virtual") != std::wstring::npos ||
                adapter_text.find(L"hyper-v") != std::wstring::npos ||
                adapter_text.find(L"vmware") != std::wstring::npos ||
                adapter_text.find(L"wsl") != std::wstring::npos ||
                adapter_text.find(L"vpn") != std::wstring::npos ||
                adapter_text.find(L"tunnel") != std::wstring::npos ||
                adapter_text.find(L"tailscale") != std::wstring::npos ||
                adapter_text.find(L"cloudflare") != std::wstring::npos;
            for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
                if (!address->Address.lpSockaddr || address->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* ipv4 = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
                if (!is_usable_lan_ip(ipv4->sin_addr.s_addr)) continue;
                char text[INET_ADDRSTRLEN]{};
                if (!inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text))) continue;
                // Prefer an active Ethernet/Wi-Fi adapter with a gateway, then its route metric.
                uint64_t score = static_cast<uint64_t>(adapter->Ipv4Metric);
                if (!has_gateway) score += 1ULL << 32;
                if (!physical) score += 1ULL << 33;
                if (virtual_adapter) score += 1ULL << 34;
                if (score < best_score) {
                    best_score = score;
                    best = text;
                }
            }
        }
    }
    WSACleanup();
    return best.empty() ? "127.0.0.1" : best;
#else
    std::string default_interface;
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    while (std::getline(routes, line)) {
        std::istringstream row(line);
        std::string interface_name, destination;
        if (row >> interface_name >> destination && destination == "00000000") {
            default_interface = interface_name;
            break;
        }
    }
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return "127.0.0.1";
    std::string best;
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        auto* ipv4 = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
        if (!is_usable_lan_ip(ipv4->sin_addr.s_addr)) continue;
        char text[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text))) continue;
        if (best.empty()) best = text;
        if (default_interface == item->ifa_name) {
            best = text;
            break;
        }
    }
    freeifaddrs(interfaces);
    return best.empty() ? "127.0.0.1" : best;
#endif
}

static std::string generate_token() {
    std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) throw std::runtime_error("OS random generator failed");
#else
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("OS random generator failed");
#endif
    std::string result;
    const char* digits = "0123456789abcdef";
    for (unsigned char byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

static bool token_equals(const std::string& candidate) {
    unsigned difference = static_cast<unsigned>(candidate.size() ^ g_session_token.size());
    for (size_t i = 0; i < g_session_token.size(); ++i)
        difference |= static_cast<unsigned char>(g_session_token[i]) ^
            (i < candidate.size() ? static_cast<unsigned char>(candidate[i]) : 0);
    return difference == 0;
}

static bool has_session_cookie(const std::string& cookies) {
    size_t start = 0;
    while (start < cookies.size()) {
        size_t end = cookies.find(';', start);
        if (end == std::string::npos) end = cookies.size();
        auto item = cookies.substr(start, end - start);
        auto first = item.find_first_not_of(" \t");
        if (first != std::string::npos && item.compare(first, 17, "LocalPathSession=") == 0 &&
            token_equals(item.substr(first + 17))) return true;
        start = end + 1;
    }
    return false;
}

static std::mutex g_console_mutex;
static std::string g_console_banner;
static bool g_console_pinned = false;
static int g_console_rows = 0;   // 0 when the terminal cannot host a pinned panel
static int g_panel_lines = 0;

#ifdef _WIN32
static BOOL WINAPI restore_console_on_ctrl(DWORD event) {
    if (g_console_pinned && (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT)) {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\x1b[r", 3, &written, nullptr);
    }
    return FALSE;
}
#else
static volatile std::sig_atomic_t g_console_pinned_signal = 0;
static void restore_console_on_signal(int signal) {
    if (g_console_pinned_signal) { const auto written = ::write(STDOUT_FILENO, "\x1b[r", 3); (void)written; }
    _exit(128 + signal);
}
#endif

static void restore_console_at_exit() {
    if (g_console_pinned) std::cout << "\x1b[r" << std::flush;
}

static void console_log(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_console_mutex);
    for (unsigned char c : message)
        std::cout << (c < 32 || c == 127 ? '?' : static_cast<char>(c));
    std::cout << '\n';
    if (!g_console_pinned) std::cout << g_console_banner;
    std::cout << std::flush;
}

static void show_access_panel(const std::string& network_url, const std::string& local_url) {
    bool ansi = false;
    int columns = 0, rows = 0;
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) &&
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) &&
        GetConsoleScreenBufferInfo(handle, &info)) {
        SetConsoleOutputCP(CP_UTF8);
        ansi = true;
        columns = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    struct winsize info{};
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &info) == 0) {
        ansi = true; columns = info.ws_col; rows = info.ws_row;
    }
#endif
    // Medium error correction is more reliable for phone cameras and OpenCV,
    // especially because the random token can produce visually dense patterns.
    auto qr = qrcodegen::QrCode::encodeText(network_url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int border = 4;
    const int size = qr.getSize() + border * 2;
    std::ostringstream panel;
    panel << "  LocalPath | Scan to open on your network\n";
    for (int y = 0; y < size; y += ansi ? 2 : 1) {
        panel << "  ";
        if (ansi) panel << "\x1b[47m\x1b[30m";
        for (int x = 0; x < size; ++x) {
            bool top = qr.getModule(x - border, y - border);
            bool bottom = qr.getModule(x - border, y + 1 - border);
            if (ansi) panel << (top ? (bottom ? "█" : "▀") : (bottom ? "▄" : " "));
            else panel << (top ? "##" : "  ");
        }
        if (ansi) panel << "\x1b[0m";
        panel << '\n';
    }
    panel << "  Network: " << network_url << "\n"
          << "  Local:   " << local_url << "\n"
          << "  Share this access only with people you trust.\n";
    g_console_banner = panel.str();
    const int panel_lines = static_cast<int>(std::count(g_console_banner.begin(), g_console_banner.end(), '\n'));
    g_console_pinned = ansi && rows >= panel_lines + 2 && columns >= static_cast<int>(std::max(network_url.size() + 11, local_url.size() + 11))
        && columns >= size + 2;
    g_panel_lines = panel_lines;
    g_console_rows = g_console_pinned ? rows : 0;
    if (g_console_pinned) std::cout << "\x1b[2J\x1b[H";
    std::cout << g_console_banner;
    if (g_console_pinned) {
        std::cout << "\x1b[" << (panel_lines + 1) << ';' << rows << 'r'
                  << "\x1b[" << (panel_lines + 1) << ";1H";
        std::atexit(restore_console_at_exit);
#ifdef _WIN32
        SetConsoleCtrlHandler(restore_console_on_ctrl, TRUE);
#else
        g_console_pinned_signal = 1;
        std::signal(SIGINT, restore_console_on_signal);
        std::signal(SIGTERM, restore_console_on_signal);
#endif
    }
    std::cout << std::flush;
}

static bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

static bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// Leave the pinned panel temporarily so the prompt is readable in normal scrollback.
static void restore_scrollback() {
    if (g_console_pinned) {
        g_console_pinned = false;
        std::cout << "\x1b[r" << std::flush;
    }
}

static void reframe_console() {
    if (g_console_banner.empty()) return;
    if (g_console_rows) {
        std::cout << "\x1b[2J\x1b[H" << g_console_banner
                  << "\x1b[" << (g_panel_lines + 1) << ';' << g_console_rows << 'r'
                  << "\x1b[" << (g_panel_lines + 1) << ";1H";
        g_console_pinned = true;
    } else {
        std::cout << g_console_banner;
    }
    std::cout << std::flush;
}

// Asks the operator, in the server terminal, whether an unauthenticated client
// may connect. Only interactive terminals are prompted; anything else (tests,
// CI, redirected output) is denied immediately so nothing can hang.
static bool request_manual_approval(const std::string& client_ip) {
    static std::mutex approval_mutex;
    std::lock_guard<std::mutex> approval_lock(approval_mutex);
    if (!stdin_is_tty() || !stdout_is_tty()) {
        console_log("[approval] " + (client_ip.empty() ? std::string("unknown address") : client_ip) +
                    " denied (no interactive console for manual approval)");
        return false;
    }
    std::lock_guard<std::mutex> console_lock(g_console_mutex);
    restore_scrollback();
    std::cout << "\n[approval] " << (client_ip.empty() ? "unknown address" : client_ip)
              << " wants to connect. Allow this client? [y/n]: " << std::flush;
    std::string line;
    bool ok = false;
    if (std::getline(std::cin, line)) {
        for (char& c : line) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        ok = line == "y" || line == "yes";
    }
    std::cout << "[approval] " << (client_ip.empty() ? "unknown address" : client_ip)
              << (ok ? " allowed" : " denied") << '\n';
    reframe_console();
    return ok;
}

static const char* FALLBACK_HTML = R"html(<!DOCTYPE html>
<html lang="uk">
<head><meta charset="UTF-8"><title>LocalPath Fallback</title></head>
<body><h2>LocalPath Server is Running</h2><p>Please place index.html in the working directory.</p></body>
</html>)html";

static void load_template(const std::string& path) {
    g_index_template = read_file(path);
    if (g_index_template.empty()) {
        std::cerr << "[warn] " << path << " not found, using built-in fallback.\n";
        g_index_template = FALLBACK_HTML;
    }
}

static std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = 0; std::istringstream ss(s.substr(i + 1, 2)); ss >> std::hex >> v;
            out += static_cast<char>(v); i += 2;
        }
        else if (s[i] == '+') { out += ' '; }
        else { out += s[i]; }
    }
    return out;
}

// HTTP paths and JSON use UTF-8. On Windows, constructing filesystem::path
// directly from std::string uses the active ANSI code page instead.
static fs::path path_from_utf8(const std::string& value) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return fs::u8path(value);
#endif
}

static std::string path_to_utf8(const fs::path& value) {
    const auto encoded = value.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

static std::string generic_path_to_utf8(const fs::path& value) {
    const auto encoded = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

static std::optional<fs::path> safe_path(const std::string& relative, bool allow_root = false) {
    try {
        if (relative.find('\0') != std::string::npos) return std::nullopt;
        fs::path rel = path_from_utf8(relative);
        if (rel.is_absolute() || rel.has_root_name()) return std::nullopt;
        for (const auto& component : rel) if (component == "..") return std::nullopt;
        auto base = fs::canonical(UPLOAD_DIR);
        auto cursor = base;
        for (const auto& component : rel) {
            cursor /= component;
            std::error_code ec;
            if (fs::is_symlink(cursor, ec)) return std::nullopt;
        }
        auto path = fs::weakly_canonical(base / rel);
        auto b = base.begin(), p = path.begin();
        for (; b != base.end(); ++b, ++p) if (p == path.end() || *p != *b) return std::nullopt;
        if (!allow_root && p == path.end()) return std::nullopt;
        return path;
    } catch (const fs::filesystem_error&) { return std::nullopt; }
}

static std::string json_escape(const std::string& value) {
    std::string result;
    const char* hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result;
}

static std::string content_disposition_filename(const std::string& disposition,
                                                const std::string& filename) {
    std::string fallback;
    std::string encoded;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : filename) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\') fallback += static_cast<char>(c);
        else if (c < 128) fallback += '_';
        else fallback += '_';

        const bool attr_char = std::isalnum(c) || c == '!' || c == '#' || c == '$' ||
            c == '&' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
            c == '`' || c == '|' || c == '~';
        if (attr_char) encoded += static_cast<char>(c);
        else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    if (fallback.empty()) fallback = "download";
    return disposition + "; filename=\"" + fallback + "\"; filename*=UTF-8''" + encoded;
}

static std::string guess_content_type(const std::string& ext) {
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css")                   return "text/css";
    if (ext == ".js")                    return "application/javascript";
    if (ext == ".json")                  return "application/json";
    if (ext == ".png")                   return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")                   return "image/gif";
    if (ext == ".pdf")                   return "application/pdf";
    if (ext == ".zip")                   return "application/zip";
    if (ext == ".tar")                   return "application/x-tar";
    if (ext == ".mp4")                   return "video/mp4";
    if (ext == ".mp3")                   return "audio/mpeg";
    if (ext == ".txt")                   return "text/plain";
    return "application/octet-stream";
}

static std::string human_size(uintmax_t bytes) {
    const char* units[] = { "B","KB","MB","GB","TB" };
    int u = 0; double v = static_cast<double>(bytes);
    while (v >= 1024 && u < 4) { v /= 1024; ++u; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(u ? 1 : 0) << v << " " << units[u];
    return ss.str();
}

static std::string format_time(fs::file_time_type ft) {
    auto sc = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t t = std::chrono::system_clock::to_time_t(sc);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M");
    return ss.str();
}

static std::string extract_boundary(const std::string& ct) {
    auto pos = ct.find("boundary=");
    if (pos == std::string::npos) return {};
    pos += 9;
    if (pos < ct.size() && ct[pos] == '"') {
        ++pos; auto end = ct.find('"', pos); return ct.substr(pos, end - pos);
    }
    auto end = ct.find(';', pos);
    return ct.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// One file part per request; keep a small tail so delimiters split across reads work.
class MultipartSink {
    std::string pending_, start_, finish_, folder_, filename_;
    fs::path temporary_, destination_;
    std::ofstream out_;
    uintmax_t written_ = 0;
    bool started_ = false, done_ = false;
public:
    MultipartSink(std::string boundary, std::string folder)
        : start_("--" + boundary + "\r\n"), finish_("\r\n--" + boundary + "--"), folder_(std::move(folder)) {}
    ~MultipartSink() { if (!temporary_.empty()) { out_.close(); std::error_code ec; fs::remove(temporary_, ec); } }
    bool feed(const char* data, size_t size) {
        if (done_) return size == 0;
        pending_.append(data, size);
        if (!started_) {
            auto end = pending_.find("\r\n\r\n");
            if (end == std::string::npos) return pending_.size() <= 8192;
            if (pending_.compare(0, start_.size(), start_) != 0) return false;
            auto headers = pending_.substr(start_.size(), end - start_.size());
            auto pos = headers.find("filename=\"");
            if (pos == std::string::npos) return false;
            pos += 10;
            auto quote = headers.find('"', pos);
            if (quote == std::string::npos) return false;
            filename_ = headers.substr(pos, quote - pos);
            if (filename_.empty() || filename_.find('\\') != std::string::npos) return false;
            auto relative = folder_.empty() ? filename_ : folder_ + "/" + filename_;
            auto dest = safe_path(relative);
            if (!dest) return false;
            destination_ = *dest;
            fs::create_directories(destination_.parent_path());
            if (safe_path(relative) != dest) return false;
            temporary_ = destination_.parent_path() / (".localpath-" + std::to_string(++g_upload_id) + ".tmp");
            out_.open(temporary_, std::ios::binary | std::ios::trunc);
            if (!out_) return false;
            pending_.erase(0, end + 4);
            started_ = true;
        }
        auto end = pending_.find(finish_);
        if (end != std::string::npos) {
            if (pending_.substr(end + finish_.size()) != "\r\n") return false;
            out_.write(pending_.data(), static_cast<std::streamsize>(end));
            written_ += end;
            out_.close();
            done_ = static_cast<bool>(out_);
            return done_;
        }
        if (pending_.size() > finish_.size() + 2) {
            size_t n = pending_.size() - finish_.size() - 2;
            out_.write(pending_.data(), static_cast<std::streamsize>(n));
            if (!out_) return false;
            written_ += n;
            pending_.erase(0, n);
        }
        return true;
    }
    bool complete() {
        if (!done_) return false;
        try {
            std::lock_guard<std::mutex> lock(g_dir_mutex);
            auto relative = folder_.empty() ? filename_ : folder_ + "/" + filename_;
            if (safe_path(relative) != destination_) return false;
            auto dest = destination_;
            auto stem = path_to_utf8(dest.stem()), ext = path_to_utf8(dest.extension());
            for (int n = 1; fs::exists(dest); ++n)
                dest = destination_.parent_path() /
                    path_from_utf8(stem + "_" + std::to_string(n) + ext);
            fs::rename(temporary_, dest);
            temporary_.clear();
            console_log("[upload] " +
                generic_path_to_utf8(fs::relative(dest, fs::canonical(UPLOAD_DIR))) +
                " (" + human_size(written_) + ")");
            return true;
        } catch (const fs::filesystem_error&) { return false; }
    }
};

static void write_tar_octal(char* dest, size_t size, uintmax_t value) {
    std::ostringstream ss;
    ss << std::setw(size - 1) << std::setfill('0') << std::oct << value;
    std::string str = ss.str();
    if (str.size() >= size) str = str.substr(str.size() - size + 1);
    std::copy(str.begin(), str.end(), dest);
    dest[size - 1] = '\0';
}

template<class Body>
static auto make_response(http::status status, unsigned version, const std::string& ct, std::string body)
-> http::response<http::string_body> {
    http::response<http::string_body> res{ status, version };
    res.set(http::field::server, "LocalPath/1.0"); res.set(http::field::content_type, ct);
    res.set(http::field::cache_control, "no-store");
    res.body() = std::move(body); res.prepare_payload(); return res;
}

static http::response<http::string_body> redirect(unsigned version, const std::string& location) {
    http::response<http::string_body> res{ http::status::found, version };
    res.set(http::field::location, location); res.set(http::field::server, "LocalPath/1.0");
    res.set(http::field::cache_control, "no-store");
    res.body() = ""; res.prepare_payload(); return res;
}

// Hands out the session cookie and drops the client onto the entry page,
// exactly like the tokenized link does.
static http::response<http::string_body> grant_session(unsigned version) {
    auto res = redirect(version, "/");
    res.result(http::status::see_other);
    res.set(http::field::set_cookie, "LocalPathSession=" + g_session_token + "; Path=/; HttpOnly; SameSite=Lax");
    res.set("Referrer-Policy", "no-referrer");
    res.keep_alive(false);
    return res;
}

static const char* WAITING_HTML = R"html(<!doctype html>
<html lang="uk"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LocalPath — очікування дозволу</title><style>
body{margin:0;min-height:100vh;display:grid;place-items:center;background:#f5f7fa;color:#1c2939;font:16px system-ui,sans-serif}
main{width:min(440px,calc(100% - 40px));padding:32px;background:#fff;border:1px solid #e2e7ee;border-radius:16px;text-align:center;box-shadow:0 12px 40px #1c293912}
.dot{width:12px;height:12px;margin:0 auto 20px;border:3px solid #d9e3fa;border-top-color:#315fdb;border-radius:50%;animation:s 1s linear infinite}
h1{font-size:22px;margin:0 0 10px}p{color:#536275;line-height:1.55;margin:0}#state{margin-top:18px;font-size:14px}
@keyframes s{to{transform:rotate(360deg)}}</style></head><body><main>
<div class="dot" aria-hidden="true"></div><h1>Очікування дозволу</h1>
<p>Сервер працює. Власник має підтвердити доступ у консолі. Файли до підтвердження недоступні.</p>
<p id="state">Перевіряємо дозвіл…</p></main><script>
async function check(){try{const r=await fetch('/api/auth/status',{cache:'no-store'});const j=await r.json();
if(j.status==='approved'){location.replace('/');return}if(j.status==='denied'){document.querySelector('.dot').style.display='none';state.textContent='Доступ відхилено власником.';return}
state.textContent='Очікуємо підтвердження…'}catch(e){state.textContent='Сервер тимчасово не відповідає. Повторюємо…'}setTimeout(check,1200)}check();
</script></body></html>)html";

enum class ApprovalState { pending, approved, denied };
struct ApprovalEntry {
    std::string ip;
    ApprovalState state;
    std::chrono::steady_clock::time_point updated;
};

static std::mutex g_approved_mutex;
static std::vector<ApprovalEntry> g_approved_clients;

static ApprovalState approval_state(const std::string& ip) {
    if (ip.empty()) return ApprovalState::denied;
    std::lock_guard<std::mutex> lock(g_approved_mutex);
    auto now = std::chrono::steady_clock::now();
    g_approved_clients.erase(std::remove_if(g_approved_clients.begin(), g_approved_clients.end(),
        [&](const ApprovalEntry& entry) { return now - entry.updated > std::chrono::minutes(5); }),
        g_approved_clients.end());
    for (const auto& entry : g_approved_clients) if (entry.ip == ip) return entry.state;
    return ApprovalState::pending;
}

static void request_approval_async(const std::string& ip) {
    if (ip.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_approved_mutex);
        for (const auto& entry : g_approved_clients) if (entry.ip == ip) return;
        g_approved_clients.push_back({ip, ApprovalState::pending, std::chrono::steady_clock::now()});
    }
    std::thread([ip] {
        const bool allowed = request_manual_approval(ip);
        std::lock_guard<std::mutex> lock(g_approved_mutex);
        for (auto& entry : g_approved_clients) {
            if (entry.ip == ip) {
                entry.state = allowed ? ApprovalState::approved : ApprovalState::denied;
                entry.updated = std::chrono::steady_clock::now();
                break;
            }
        }
    }).detach();
}

static std::string remote_ip(tcp::socket& socket) {
    try { return socket.remote_endpoint().address().to_string(); }
    catch (const std::exception&) { return {}; }
}

static http::response<http::string_body> auth_status_response(
    unsigned version, ApprovalState state) {
    auto text = state == ApprovalState::approved ? "approved" :
                state == ApprovalState::denied ? "denied" : "pending";
    auto res = make_response<http::string_body>(http::status::ok, version,
        "application/json; charset=utf-8", std::string("{\"status\":\"") + text + "\"}");
    if (state == ApprovalState::approved)
        res.set(http::field::set_cookie, "LocalPathSession=" + g_session_token + "; Path=/; HttpOnly; SameSite=Lax");
    return res;
}

static void stream_file(tcp::socket&, const fs::path&, unsigned, bool, beast::error_code&);

void do_session(tcp::socket socket) {
    beast::error_code ec; beast::flat_buffer buf;
    try {
    for (;;) {
        http::request_parser<http::buffer_body> parser;
        parser.body_limit(boost::none); // Uploads are streamed, not buffered.
        http::read_header(socket, buf, parser, ec); if (ec) break;
        const std::string target = std::string(parser.get().target());
        const auto method = parser.get().method();
        const unsigned version = parser.get().version();
        const bool keep_alive = parser.get().keep_alive();
        // The bearer link is accepted only on the entry page. A redirect removes
        // it from the address bar; subsequent requests use an HttpOnly cookie.
        if (method == http::verb::get && target.rfind("/?token=", 0) == 0 &&
            token_equals(target.substr(8))) {
            http::write(socket, grant_session(version), ec);
            break;
        }
        if (!has_session_cookie(std::string(parser.get()[http::field::cookie]))) {
            const std::string client_ip = remote_ip(socket);
            if (method == http::verb::get && target == "/api/auth/status") {
                http::write(socket, auth_status_response(version, approval_state(client_ip)), ec);
            } else if (method == http::verb::get &&
                       (target == "/" || target == "/index.html" || target.rfind("/?token=", 0) == 0)) {
                request_approval_async(client_ip);
                auto res = make_response<http::string_body>(http::status::ok, version,
                    "text/html; charset=utf-8", WAITING_HTML);
                res.keep_alive(keep_alive);
                http::write(socket, res, ec);
            } else {
                auto res = make_response<http::string_body>(http::status::unauthorized, version,
                    "application/json; charset=utf-8", "{\"error\":\"authorization_required\"}");
                res.keep_alive(keep_alive);
                http::write(socket, res, ec);
            }
            if (ec || !keep_alive) break;
            continue;
        }
        const bool upload = method == http::verb::post && target.rfind("/upload?path=", 0) == 0;
        const auto boundary = upload ? extract_boundary(std::string(parser.get()[http::field::content_type])) : "";
        std::string folder = upload ? url_decode(target.substr(13)) : "";
        std::optional<MultipartSink> sink;
        if (upload && !boundary.empty() && safe_path(folder, true)) sink.emplace(boundary, folder);
        std::string body;
        bool valid = true;
        while (!parser.is_done()) {
            std::array<char, 65536> chunk{};
            parser.get().body().data = chunk.data();
            parser.get().body().size = chunk.size();
            http::read(socket, buf, parser, ec);
            if (ec == http::error::need_buffer) ec = {};
            if (ec) break;
            size_t n = chunk.size() - parser.get().body().size;
            if (upload) {
                try { if (!sink || !sink->feed(chunk.data(), n)) valid = false; }
                catch (const fs::filesystem_error&) { valid = false; }
            } else if (body.size() + n <= 65536) body.append(chunk.data(), n);
            else valid = false;
            if (!valid) break;
        }
        if (ec) break;
        if (!valid) {
            http::write(socket, make_response<http::string_body>(http::status::bad_request, version, "text/plain", "Invalid request"), ec);
            break;
        }
        if (upload) {
            bool ok = sink && sink->complete();
            http::write(socket, make_response<http::string_body>(ok ? http::status::ok : http::status::bad_request, version, "text/plain", ok ? "OK" : "Invalid upload"), ec);
            if (ec || !keep_alive) break;
            continue;
        }

        if (method == http::verb::get && (target == "/" || target == "/index.html")) {
            http::write(socket, make_response<http::string_body>(http::status::ok, version, "text/html; charset=utf-8", g_index_template), ec);
        }
        else if (method == http::verb::post && target.rfind("/upload", 0) == 0) {
            http::write(socket, make_response<http::string_body>(http::status::bad_request, version, "text/plain", "Missing path"), ec);
        }
        else if (method == http::verb::get && target.rfind("/api/files", 0) == 0) {
            std::string req_path = "";
            auto q = target.find("?path=");
            if (q != std::string::npos) req_path = url_decode(target.substr(q + 6));

            auto target_abs = safe_path(req_path, true);
            std::string json = "[]";

            if (target_abs && fs::exists(*target_abs) && fs::is_directory(*target_abs)) {
                json = "["; bool first = true;
                std::lock_guard<std::mutex> lock(g_dir_mutex);
                std::vector<fs::directory_entry> entries;
                for (auto& e : fs::directory_iterator(*target_abs))
                    if (!e.is_symlink() &&
                        path_to_utf8(e.path().filename()).rfind(".localpath-", 0) != 0)
                        entries.push_back(e);

                std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
                    if (a.is_directory() != b.is_directory()) return a.is_directory() > b.is_directory();
                    return a.last_write_time() > b.last_write_time();
                    });

                for (auto& e : entries) {
                    std::string name = path_to_utf8(e.path().filename());
                    bool is_dir = e.is_directory();
                    uintmax_t sz = is_dir ? 0 : e.file_size();
                    std::string sz_h = is_dir ? "Folder" : human_size(sz);
                    std::string date = format_time(e.last_write_time());
                    std::string jname = json_escape(name);
                    if (!first) json += ",";
                    json += "{\"name\":\"" + jname + "\",\"is_dir\":" + (is_dir ? "true" : "false") + ",\"size\":" + std::to_string(sz) + ",\"size_human\":\"" + sz_h + "\",\"date\":\"" + date + "\"}";
                    first = false;
                }
                json += "]";
            }
            http::response<http::string_body> res{ http::status::ok, version };
            res.set(http::field::server, "LocalPath/1.0"); res.set(http::field::content_type, "application/json");
            res.set(http::field::cache_control, "no-store"); res.body() = std::move(json);
            res.prepare_payload(); http::write(socket, res, ec);
        }
        else if (method == http::verb::get && target.rfind("/download", 0) == 0) {
            auto q = target.find("?file=");
            if (q != std::string::npos) {
                std::string fname = url_decode(target.substr(q + 6));
                auto fpath = safe_path(fname);
                if (fpath && fs::is_regular_file(*fpath)) {
                    stream_file(socket, *fpath, version, false, ec);
                    break;
                }
            }
            http::write(socket, make_response<http::string_body>(http::status::not_found, version, "text/plain", "Not found"), ec);
        }
        else if (method == http::verb::get && target.rfind("/archive", 0) == 0) {
            std::string req_path = ""; auto q = target.find("?path=");
            if (q != std::string::npos) req_path = url_decode(target.substr(q + 6));
            auto target_abs = safe_path(req_path, true);

            if (!target_abs || !fs::is_directory(*target_abs)) {
                http::write(socket, make_response<http::string_body>(http::status::bad_request, version, "text/plain", "Bad path"), ec);
            }
            else {
                stream_file(socket, *target_abs, version, true, ec);
                break;
            }
        }
        else if (method == http::verb::post && target == "/delete") {
            std::string fname; auto pos = body.find("file=");
            if (pos != std::string::npos) fname = url_decode(body.substr(pos + 5));
            if (!fname.empty()) {
                std::lock_guard<std::mutex> lock(g_dir_mutex);
                auto fpath = safe_path(fname);
                if (fpath && fs::exists(*fpath)) {
                    fs::remove_all(*fpath); console_log("[delete] " + fname);
                }
            }
            http::write(socket, redirect(version, "/"), ec);
        }
        else { http::write(socket, make_response<http::string_body>(http::status::not_found, version, "text/plain", "404 Not Found"), ec); }
        if (ec || !keep_alive) break;
    }
    } catch (const std::exception& e) { console_log("[request] " + std::string(e.what())); }
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

int main(int argc, char* argv[]) {
    unsigned short port = 8080; if (argc >= 2) port = static_cast<unsigned short>(std::stoi(argv[1]));
    try {
        fs::create_directories(UPLOAD_DIR); load_template("index.html");
        g_session_token = generate_token();
        auto const address = net::ip::make_address("0.0.0.0");
        net::io_context ioc{ 1 }; tcp::acceptor acceptor{ ioc, {address, port} };
        const auto suffix = ":" + std::to_string(port) + "/?token=" + g_session_token;
        show_access_panel("http://" + get_local_ip() + suffix, "http://127.0.0.1" + suffix);
        for (;;) { tcp::socket socket{ ioc }; acceptor.accept(socket); std::thread(&do_session, std::move(socket)).detach(); }
    }
    catch (std::exception const& e) { std::cerr << "Fatal: " << e.what() << "\n"; return 1; }
}

static void stream_file(tcp::socket& socket, const fs::path& path, unsigned version, bool archive, beast::error_code& ec) {
    http::response<http::empty_body> res{http::status::ok, version};
    res.set(http::field::server, "LocalPath/1.0");
    res.set(http::field::content_type,
        archive ? "application/x-tar" : guess_content_type(path_to_utf8(path.extension())));
    res.set(http::field::cache_control, "no-store");
    res.set(http::field::x_content_type_options, "nosniff");
    std::string download_name = path_to_utf8(path.filename());
    if (archive) download_name += ".tar";
    res.set(http::field::content_disposition,
        content_disposition_filename(
            archive || path.extension() == ".html" ? "attachment" : "inline",
            download_name));
    res.keep_alive(false);
    res.chunked(true);
    http::response_serializer<http::empty_body> serializer{res};
    http::write_header(socket, serializer, ec);
    if (ec) return;
    auto emit = [&](const char* data, size_t n) {
        if (n && !ec) net::write(socket, http::make_chunk(net::buffer(data, n)), ec);
    };
    auto copy = [&](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { ec = net::error::fault; return; }
        std::array<char, 65536> block{};
        while (in && !ec) {
            in.read(block.data(), block.size());
            emit(block.data(), static_cast<size_t>(in.gcount()));
        }
        if (in.bad()) ec = net::error::fault;
    };
    if (!archive) copy(path);
    else {
        auto append = [&](const fs::path& file) {
            auto rel = generic_path_to_utf8(fs::relative(file, path));
            if (rel.size() > 99) return;
            char header[512]{};
            std::copy(rel.begin(), rel.end(), header);
            write_tar_octal(header + 100, 8, 0644);
            write_tar_octal(header + 108, 8, 0);
            write_tar_octal(header + 116, 8, 0);
            auto size = fs::file_size(file);
            write_tar_octal(header + 124, 12, size);
            auto sc = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                fs::last_write_time(file) - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            write_tar_octal(header + 136, 12, std::chrono::system_clock::to_time_t(sc));
            header[156] = '0';
            std::copy_n("ustar", 5, header + 257);
            header[262] = ' '; header[263] = '0'; header[264] = '0';
            std::fill(header + 148, header + 156, ' ');
            unsigned sum = 0;
            for (unsigned char c : header) sum += c;
            write_tar_octal(header + 148, 8, sum);
            emit(header, sizeof(header));
            copy(file);
            std::array<char, 512> padding{};
            emit(padding.data(), (512 - size % 512) % 512);
        };
        try {
            for (auto& entry : fs::recursive_directory_iterator(path)) {
                if (ec) break;
                if (entry.is_symlink() || !entry.is_regular_file()) continue;
                append(entry.path());
            }
            std::array<char, 1024> ending{};
            emit(ending.data(), ending.size());
        } catch (const fs::filesystem_error&) { ec = net::error::fault; }
    }
    if (!ec) net::write(socket, http::make_chunk_last(), ec);
}
