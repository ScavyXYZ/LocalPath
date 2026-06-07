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


#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

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

static std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string get_local_ip() {
    std::string local_ip = "127.0.0.1";

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return local_ip;
    }
#endif
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        return local_ip;
    }

    sockaddr_in remote_addr;
    std::memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    remote_addr.sin_port = htons(53);

    if (connect(sock, reinterpret_cast<sockaddr*>(&remote_addr), sizeof(remote_addr)) != SOCKET_ERROR) {
        sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);

        if (getsockname(sock, reinterpret_cast<sockaddr*>(&local_addr), &addr_len) == 0) {
            char buffer[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &(local_addr.sin_addr), buffer, INET_ADDRSTRLEN) != nullptr) {
                local_ip = buffer;
            }
        }
    }
    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif

    return local_ip;
}

static const char* FALLBACK_HTML = R"html(<!DOCTYPE html>
<html lang="uk">
<head><meta charset="UTF-8"><title>FileBeam Fallback</title></head>
<body><h2>FileBeam Server is Running</h2><p>Please place index.html in the execution folder.</p></body>
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

struct ParsedFile { std::string filename; std::string data; bool ok = false; };

static ParsedFile parse_multipart(const std::string& body, const std::string& boundary) {
    ParsedFile pf; const std::string delim = "--" + boundary;
    auto pos = body.find(delim); if (pos == std::string::npos) return pf;
    pos += delim.size(); if (pos + 2 <= body.size() && body[pos] == '\r') pos += 2;
    auto header_end = body.find("\r\n\r\n", pos); if (header_end == std::string::npos) return pf;
    std::string part_headers = body.substr(pos, header_end - pos);
    auto cd_pos = part_headers.find("filename=\"");
    if (cd_pos != std::string::npos) {
        cd_pos += 10; auto cd_end = part_headers.find('"', cd_pos);
        pf.filename = part_headers.substr(cd_pos, cd_end - cd_pos);
    }
    if (pf.filename.empty()) pf.filename = "uploaded_file.bin";

    if (pf.filename.find("..") != std::string::npos) return pf;
    while (!pf.filename.empty() && (pf.filename[0] == '/' || pf.filename[0] == '\\')) {
        pf.filename.erase(pf.filename.begin());
    }

    size_t data_start = header_end + 4;
    const std::string end_delim = "\r\n" + delim;
    auto data_end = body.find(end_delim, data_start);
    if (data_end == std::string::npos) return pf;
    pf.data = body.substr(data_start, data_end - data_start); pf.ok = true; return pf;
}

static void write_tar_octal(char* dest, size_t size, uintmax_t value) {
    std::ostringstream ss;
    ss << std::setw(size - 1) << std::setfill('0') << std::oct << value;
    std::string str = ss.str();
    if (str.size() >= size) str = str.substr(str.size() - size + 1);
    std::copy(str.begin(), str.end(), dest);
    dest[size - 1] = '\0';
}

static std::string create_tar_archive(const fs::path& base_dir) {
    std::string out;
    if (!fs::exists(base_dir)) return out;
    try {
        for (auto& entry : fs::recursive_directory_iterator(base_dir)) {
            if (!entry.is_regular_file()) continue;
            fs::path rel = fs::relative(entry.path(), base_dir);
            std::string rel_str = rel.string();
            std::replace(rel_str.begin(), rel_str.end(), '\\', '/');
            if (rel_str.size() > 99) continue;

            char header[512] = { 0 };
            std::copy(rel_str.begin(), rel_str.end(), header);
            write_tar_octal(header + 100, 8, 0644);
            write_tar_octal(header + 108, 8, 0);
            write_tar_octal(header + 116, 8, 0);
            uintmax_t fsize = entry.file_size();
            write_tar_octal(header + 124, 12, fsize);

            auto sc = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                entry.last_write_time() - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            std::time_t t = std::chrono::system_clock::to_time_t(sc);
            write_tar_octal(header + 136, 12, t);

            header[156] = '0';
            std::string magic = "ustar";
            std::copy(magic.begin(), magic.end(), header + 257);
            header[262] = ' '; header[263] = '0'; header[264] = '0';

            std::fill(header + 148, header + 156, ' ');
            unsigned int chksum = 0;
            for (int i = 0; i < 512; ++i) chksum += static_cast<unsigned char>(header[i]);
            write_tar_octal(header + 148, 8, chksum);

            out.append(header, 512);

            std::ifstream ifs(entry.path(), std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            out.append(content);

            size_t pad = (512 - (content.size() % 512)) % 512;
            if (pad > 0) out.append(pad, '\0');
        }
        out.append(1024, '\0');
    }
    catch (...) {}
    return out;
}

template<class Body>
static auto make_response(http::status status, unsigned version, const std::string& ct, std::string body)
-> http::response<http::string_body> {
    http::response<http::string_body> res{ status, version };
    res.set(http::field::server, "FileBeam/1.0"); res.set(http::field::content_type, ct);
    res.body() = std::move(body); res.prepare_payload(); return res;
}

static http::response<http::string_body> redirect(unsigned version, const std::string& location) {
    http::response<http::string_body> res{ http::status::found, version };
    res.set(http::field::location, location); res.set(http::field::server, "FileBeam/1.0");
    res.body() = ""; res.prepare_payload(); return res;
}

void do_session(tcp::socket socket) {
    beast::error_code ec; beast::flat_buffer buf;
    for (;;) {
        http::request_parser<http::string_body> parser; parser.body_limit(boost::none);
        http::read_header(socket, buf, parser, ec); if (ec) break;
        http::read(socket, buf, parser, ec); if (ec) break;
        auto req = parser.release(); const std::string target = std::string(req.target()); const auto method = req.method();

        if (method == http::verb::get && (target == "/" || target == "/index.html")) {
            http::write(socket, make_response<http::string_body>(http::status::ok, req.version(), "text/html; charset=utf-8", g_index_template), ec);
        }
        else if (method == http::verb::post && target.rfind("/upload", 0) == 0) {
            std::string req_path = "";
            auto q = target.find("?path=");
            if (q != std::string::npos) req_path = url_decode(target.substr(q + 6));

            std::string ct = std::string(req[http::field::content_type]); auto boundary = extract_boundary(ct);
            std::string resp_body = "Error"; http::status resp_status = http::status::bad_request;
            if (!boundary.empty()) {
                auto pf = parse_multipart(req.body(), boundary);
                if (pf.ok) {
                    std::lock_guard<std::mutex> lock(g_dir_mutex);
                    fs::path base_abs = fs::absolute(UPLOAD_DIR);
                    fs::path dest = fs::absolute(base_abs / req_path / pf.filename);

                    if (dest.string().rfind(base_abs.string(), 0) == 0) {
                        fs::create_directories(dest.parent_path());
                        if (fs::exists(dest)) {
                            std::string stem = dest.stem().string(), ext = dest.extension().string(); int n = 1;
                            while (fs::exists(dest)) dest = dest.parent_path() / (stem + "_" + std::to_string(n++) + ext);
                        }
                        std::ofstream ofs(dest, std::ios::binary); ofs.write(pf.data.data(), static_cast<std::streamsize>(pf.data.size()));
                        resp_body = "OK"; resp_status = http::status::ok;
                        std::cout << "[upload] " << fs::relative(dest, base_abs) << " (" << human_size(pf.data.size()) << ")\n";
                    }
                }
            }
            http::write(socket, make_response<http::string_body>(resp_status, req.version(), "text/plain", resp_body), ec);
        }
        else if (method == http::verb::get && target.rfind("/api/files", 0) == 0) {
            std::string req_path = "";
            auto q = target.find("?path=");
            if (q != std::string::npos) req_path = url_decode(target.substr(q + 6));

            fs::path base_abs = fs::absolute(UPLOAD_DIR);
            fs::path target_abs = fs::absolute(base_abs / req_path);
            std::string json = "[]";

            if (target_abs.string().rfind(base_abs.string(), 0) == 0 && fs::exists(target_abs) && fs::is_directory(target_abs)) {
                json = "["; bool first = true;
                std::lock_guard<std::mutex> lock(g_dir_mutex);
                std::vector<fs::directory_entry> entries;
                for (auto& e : fs::directory_iterator(target_abs)) entries.push_back(e);

                std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
                    if (a.is_directory() != b.is_directory()) return a.is_directory() > b.is_directory();
                    return a.last_write_time() > b.last_write_time();
                    });

                for (auto& e : entries) {
                    std::string name = e.path().filename().string();
                    bool is_dir = e.is_directory();
                    uintmax_t sz = is_dir ? 0 : e.file_size();
                    std::string sz_h = is_dir ? "Folder" : human_size(sz);
                    std::string date = format_time(e.last_write_time());
                    std::string jname;
                    for (char c : name) { if (c == '"') jname += "\\\""; else if (c == '\\') jname += "\\\\"; else jname += c; }
                    if (!first) json += ",";
                    json += "{\"name\":\"" + jname + "\",\"is_dir\":" + (is_dir ? "true" : "false") + ",\"size\":" + std::to_string(sz) + ",\"size_human\":\"" + sz_h + "\",\"date\":\"" + date + "\"}";
                    first = false;
                }
                json += "]";
            }
            http::response<http::string_body> res{ http::status::ok, req.version() };
            res.set(http::field::server, "FileBeam/1.0"); res.set(http::field::content_type, "application/json");
            res.set(http::field::access_control_allow_origin, "*"); res.body() = std::move(json);
            res.prepare_payload(); http::write(socket, res, ec);
        }
        else if (method == http::verb::get && target.rfind("/download", 0) == 0) {
            auto q = target.find("?file=");
            if (q != std::string::npos) {
                std::string fname = url_decode(target.substr(q + 6));
                fs::path base_abs = fs::absolute(UPLOAD_DIR);
                fs::path fpath = fs::absolute(base_abs / fname);
                if (fpath.string().rfind(base_abs.string(), 0) == 0 && fs::exists(fpath) && fs::is_regular_file(fpath)) {
                    std::ifstream ifs(fpath, std::ios::binary);
                    std::string file_data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    http::response<http::string_body> res{ http::status::ok, req.version() };
                    res.set(http::field::server, "FileBeam/1.0"); res.set(http::field::content_type, guess_content_type(fpath.extension().string()));
                    res.set(http::field::content_disposition, "inline; filename=\"" + fpath.filename().string() + "\"");
                    res.body() = std::move(file_data); res.prepare_payload(); http::write(socket, res, ec);
                    continue;
                }
            }
            http::write(socket, make_response<http::string_body>(http::status::not_found, req.version(), "text/plain", "Not found"), ec);
        }
        else if (method == http::verb::get && target.rfind("/archive", 0) == 0) {
            std::string req_path = ""; auto q = target.find("?path=");
            if (q != std::string::npos) req_path = url_decode(target.substr(q + 6));
            fs::path base_abs = fs::absolute(UPLOAD_DIR);
            fs::path target_abs = fs::absolute(base_abs / req_path);

            if (target_abs.string().rfind(base_abs.string(), 0) != 0 || !fs::exists(target_abs)) {
                http::write(socket, make_response<http::string_body>(http::status::bad_request, req.version(), "text/plain", "Bad path"), ec);
            }
            else {
                std::string tar = create_tar_archive(target_abs);
                std::string dl_name = (target_abs == base_abs) ? "all_files.tar" : target_abs.filename().string() + ".tar";
                http::response<http::string_body> res{ http::status::ok, req.version() };
                res.set(http::field::server, "FileBeam/1.0"); res.set(http::field::content_type, "application/x-tar");
                res.set(http::field::content_disposition, "attachment; filename=\"" + dl_name + "\"");
                res.body() = std::move(tar); res.prepare_payload(); http::write(socket, res, ec);
            }
        }
        else if (method == http::verb::post && target == "/delete") {
            std::string body = req.body(); std::string fname; auto pos = body.find("file=");
            if (pos != std::string::npos) fname = url_decode(body.substr(pos + 5));
            if (!fname.empty()) {
                std::lock_guard<std::mutex> lock(g_dir_mutex);
                fs::path base_abs = fs::absolute(UPLOAD_DIR); fs::path fpath = fs::absolute(base_abs / fname);
                if (fpath.string().rfind(base_abs.string(), 0) == 0 && fs::exists(fpath)) {
                    fs::remove_all(fpath); std::cout << "[delete] " << fname << "\n";
                }
            }
            http::write(socket, redirect(req.version(), "/"), ec);
        }
        else { http::write(socket, make_response<http::string_body>(http::status::not_found, req.version(), "text/plain", "404 Not Found"), ec); }
        if (ec || !req.keep_alive()) break;
    }
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

int main(int argc, char* argv[]) {
    unsigned short port = 8080; if (argc >= 2) port = static_cast<unsigned short>(std::stoi(argv[1]));
    fs::create_directories(UPLOAD_DIR); load_template("index.html");
    try {
        auto const address = net::ip::make_address("0.0.0.0");
        net::io_context ioc{ 1 }; tcp::acceptor acceptor{ ioc, {address, port} };
        std::cout << "\n  LocalPath - local network folder & file transfer\n  ------------------------------------------------\n"
            << "  Network: http://" << get_local_ip() << ":" << port << "\n\n";
        for (;;) { tcp::socket socket{ ioc }; acceptor.accept(socket); std::thread(&do_session, std::move(socket)).detach(); }
    }
    catch (std::exception const& e) { std::cerr << "Fatal: " << e.what() << "\n"; return 1; }
}