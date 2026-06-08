#include "http_server.h"
#include "benchmark.h"
#include "verification.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────
// Platform-specific recv / send wrappers
// On UCRT/MinGW, ssize_t is already typedef'd as __int64 in corecrt.h,
// so we only define it ourselves on platforms where it's missing.
// ─────────────────────────────────────────────────────────────────
#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
  // MinGW UCRT already provides ssize_t as __int64 — do not redefine.
  static inline int sock_recv(socket_t fd, char* buf, int len, int flags) {
      return ::recv(fd, buf, len, flags);
  }
  static inline int sock_send(socket_t fd, const char* buf, int len, int flags) {
      return ::send(fd, buf, len, flags);
  }
#else
  static inline ssize_t sock_recv(socket_t fd, char* buf, int len, int flags) {
      return ::recv(fd, buf, static_cast<size_t>(len), flags);
  }
  static inline ssize_t sock_send(socket_t fd, const char* buf, int len, int flags) {
      return ::send(fd, buf, static_cast<size_t>(len), flags);
  }
#endif

// ─────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────
HttpServer::HttpServer(KVStore& store, int port)
    : store_(store), port_(port), server_fd_(INVALID_SOCK)
{
#ifdef _WIN32
    // Initialize Winsock (idempotent — safe to call multiple times)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        throw std::runtime_error("[HttpServer] WSAStartup failed");
#endif

    // Create TCP socket
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == INVALID_SOCK)
        throw std::runtime_error("[HttpServer] socket() failed");

    // SO_REUSEADDR — allows immediate rebind after restart
#ifndef _WIN32
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, static_cast<socklen_t>(sizeof(opt)));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(server_fd_);
        throw std::runtime_error("[HttpServer] bind() failed on port " + std::to_string(port_));
    }

    if (::listen(server_fd_, 8) != 0) {
        close_sock(server_fd_);
        throw std::runtime_error("[HttpServer] listen() failed");
    }
}

HttpServer::~HttpServer() {
    stream_stop_ = true;
    if (stream_thread_.joinable()) stream_thread_.join();
    if (server_fd_ != INVALID_SOCK) {
        close_sock(server_fd_);
        server_fd_ = INVALID_SOCK;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

// ─────────────────────────────────────────────────────────────────
// Main accept loop
// ─────────────────────────────────────────────────────────────────
void HttpServer::run() {
    std::cout << "[HttpServer] Listening on http://localhost:" << port_ << "\n";
    std::cout << "[HttpServer] Dashboard: http://localhost:" << port_ << "/\n";

    while (true) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        socket_t client_fd = ::accept(
            server_fd_,
            reinterpret_cast<sockaddr*>(&client_addr),
            &addr_len
        );

        if (client_fd == INVALID_SOCK) {
            // Accept errors are non-fatal — log and continue
            std::cerr << "[HttpServer] accept() error, retrying\n";
            continue;
        }

        // Set read timeout to prevent blocking forever on browser pre-connections
#ifdef _WIN32
        DWORD timeout = 1000; // 1 second
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, static_cast<socklen_t>(sizeof(tv)));
#endif

        handle_client(client_fd);
        close_sock(client_fd);
    }
}

// ─────────────────────────────────────────────────────────────────
// Read a complete HTTP request from the socket into `raw`
// We accumulate bytes until we see \r\n\r\n (end of headers), then
// read any remaining body bytes indicated by Content-Length.
// ─────────────────────────────────────────────────────────────────
bool HttpServer::read_request(socket_t fd, std::string& raw) {
    raw.clear();
    std::array<char, 4096> buf{};

    // Phase 1: read until end-of-headers marker
    while (true) {
        auto n = sock_recv(fd, buf.data(), static_cast<int>(buf.size()) - 1, 0);
        if (n <= 0) return false;
        raw.append(buf.data(), static_cast<size_t>(n));
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (raw.size() > 65536) return false; // guard against giant headers
    }

    // Phase 2: read body bytes if Content-Length is set
    auto cl_pos = raw.find("Content-Length:");
    if (cl_pos == std::string::npos) cl_pos = raw.find("content-length:");
    if (cl_pos != std::string::npos) {
        auto val_start = raw.find_first_not_of(" \t", cl_pos + 15);
        auto val_end   = raw.find("\r\n", val_start);
        if (val_start != std::string::npos && val_end != std::string::npos) {
            size_t content_len = 0;
            try {
                content_len = static_cast<size_t>(
                    std::stoul(raw.substr(val_start, val_end - val_start))
                );
            } catch (...) {
                return false;
            }
            auto header_end = raw.find("\r\n\r\n");
            size_t body_start = header_end + 4;
            size_t have = (body_start < raw.size()) ? raw.size() - body_start : 0;

            while (have < content_len) {
                auto n = sock_recv(fd, buf.data(),
                    static_cast<int>(std::min(buf.size() - 1, content_len - have)), 0);
                if (n <= 0) break;
                raw.append(buf.data(), static_cast<size_t>(n));
                have += static_cast<size_t>(n);
            }
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Parse the raw HTTP request string into an HttpRequest struct
// ─────────────────────────────────────────────────────────────────
bool HttpServer::parse_request(const std::string& raw, HttpRequest& req) {
    std::istringstream stream(raw);
    std::string request_line;
    if (!std::getline(stream, request_line)) return false;
    if (!request_line.empty() && request_line.back() == '\r')
        request_line.pop_back();

    // Parse: METHOD path HTTP/x.x
    std::istringstream rl(request_line);
    std::string version;
    rl >> req.method >> req.path >> version;
    if (req.method.empty() || req.path.empty()) return false;

    // Strip query string from path
    auto qs = req.path.find('?');
    if (qs != std::string::npos) req.path = req.path.substr(0, qs);

    // Parse headers until blank line
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string hname = line.substr(0, colon);
            std::string hval  = line.substr(colon + 1);
            // Trim leading whitespace from value
            auto vstart = hval.find_first_not_of(" \t");
            if (vstart != std::string::npos) hval = hval.substr(vstart);
            // Lowercase the header name for case-insensitive lookup
            std::transform(hname.begin(), hname.end(), hname.begin(), ::tolower);
            req.headers[hname] = hval;
        }
    }

    // Extract body — everything after \r\n\r\n
    auto sep = raw.find("\r\n\r\n");
    if (sep != std::string::npos) req.body = raw.substr(sep + 4);

    return true;
}

// ─────────────────────────────────────────────────────────────────
// Build the raw HTTP response string to send back to the client
// ─────────────────────────────────────────────────────────────────
std::string HttpServer::build_response(const HttpResponse& resp) {
    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << " " << status_text(resp.status) << "\r\n";
    out << "Content-Type: " << resp.content_type << "; charset=utf-8\r\n";
    out << "Content-Length: " << resp.body.size() << "\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: Content-Type\r\n";
    out << "Connection: close\r\n";
    out << "\r\n";
    out << resp.body;
    return out.str();
}

// ─────────────────────────────────────────────────────────────────
// Handle one client connection end-to-end
// ─────────────────────────────────────────────────────────────────
void HttpServer::handle_client(socket_t client_fd) {
    std::string raw;
    if (!read_request(client_fd, raw)) return;

    HttpRequest req;
    if (!parse_request(raw, req)) return;

    // OPTIONS pre-flight (CORS)
    if (req.method == "OPTIONS") {
        HttpResponse cors;
        cors.status = 204;
        cors.body   = "";
        std::string r = build_response(cors);
        sock_send(client_fd, r.c_str(), static_cast<int>(r.size()), 0);
        return;
    }

    HttpResponse resp = route(req);
    std::string wire  = build_response(resp);
    sock_send(client_fd, wire.c_str(), static_cast<int>(wire.size()), 0);
}

// ─────────────────────────────────────────────────────────────────
// Router — dispatch to the appropriate handler
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::route(const HttpRequest& req) {
    // Static assets
    if (req.method == "GET") {
        if (req.path == "/")            return handle_static("web/index.html");
        if (req.path == "/style.css")   return handle_static("web/style.css");
        if (req.path == "/app.js")      return handle_static("web/app.js");
        if (req.path == "/api/metrics") return handle_metrics();
        if (req.path == "/api/lsm-state") return handle_lsm_state();
        if (req.path == "/api/debug/state") return handle_debug_state();
        if (req.path == "/api/debug/files") return handle_debug_files();
        if (req.path == "/api/stream/status") return handle_stream_status();
    }
    if (req.method == "POST") {
        if (req.path == "/api/put")    return handle_put(req);
        if (req.path == "/api/get")    return handle_get(req);
        if (req.path == "/api/delete") return handle_delete(req);
        if (req.path == "/api/bench")  return handle_bench(req);
        if (req.path == "/api/iot/bulk") return handle_iot_bulk(req);
        if (req.path == "/api/stream/start") return handle_stream_start(req);
        if (req.path == "/api/stream/stop") return handle_stream_stop();
        if (req.path == "/api/demo/run") return handle_demo_run(req);
        if (req.path == "/api/verify/basic") return handle_verify(req, "basic");
        if (req.path == "/api/verify/overwrite") return handle_verify(req, "overwrite");
        if (req.path == "/api/verify/delete") return handle_verify(req, "delete");
        if (req.path == "/api/verify/flush") return handle_verify(req, "flush");
        if (req.path == "/api/verify/bloom") return handle_verify(req, "bloom");
        if (req.path == "/api/verify/compaction") return handle_verify(req, "compaction");
        if (req.path == "/api/verify/recovery") return handle_verify(req, "recovery");
        if (req.path == "/api/verify/gc") return handle_verify(req, "gc");
        if (req.path == "/api/verify/full") return handle_verify(req, "full");
    }

    HttpResponse not_found;
    not_found.status = 404;
    not_found.body   = R"({"error":"not found"})";
    return not_found;
}

// ─────────────────────────────────────────────────────────────────
// Static file serving from the web/ directory
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_static(const std::string& rel_path) {
    std::ifstream f(rel_path, std::ios::binary);
    HttpResponse resp;
    if (!f) {
        resp.status = 404;
        resp.body   = R"({"error":"static file not found"})";
        return resp;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    resp.body = ss.str();

    // Set correct content type
    if (rel_path.ends_with(".html")) resp.content_type = "text/html";
    else if (rel_path.ends_with(".css")) resp.content_type = "text/css";
    else if (rel_path.ends_with(".js"))  resp.content_type = "application/javascript";

    return resp;
}

// ─────────────────────────────────────────────────────────────────
// GET /api/metrics  — raw + derived engine metrics
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_metrics() {
    std::lock_guard<std::mutex> guard(store_mutex_);
    const auto& m = store_.metrics();
    auto summary = store_.storage_summary();

    double write_amp = (m.user_bytes_written > 0)
        ? static_cast<double>(m.storage_bytes_written) /
          static_cast<double>(m.user_bytes_written)
        : 0.0;

    double read_amp = (m.get_calls > 0)
        ? static_cast<double>(m.sst_searches + m.vlog_reads) /
          static_cast<double>(m.get_calls)
        : 0.0;

    double bloom_eff = (m.sst_considered > 0)
        ? static_cast<double>(m.bloom_skips) /
          static_cast<double>(m.sst_considered) * 100.0
        : 0.0;

    std::ostringstream j;
    j << "{\n";
    j << json_num ("user_bytes_written",    m.user_bytes_written);
    j << json_num ("session_user_bytes_written", m.user_bytes_written);
    j << json_num ("storage_bytes_written", m.storage_bytes_written);
    j << json_num ("session_storage_bytes_written", m.storage_bytes_written);
    j << json_num ("live_keys_estimate",    summary.live_keys);
    j << json_num ("live_logical_bytes_estimate", summary.live_logical_bytes);
    j << json_num ("tombstones_estimate",   summary.tombstones);
    j << json_num ("get_calls",             m.get_calls);
    j << json_num ("sst_considered",        m.sst_considered);
    j << json_num ("bloom_skips",           m.bloom_skips);
    j << json_num ("sst_searches",          m.sst_searches);
    j << json_num ("vlog_reads",            m.vlog_reads);
    j << json_dbl ("write_amplification",   write_amp);
    j << json_dbl ("read_amplification",    read_amp);
    j << json_dbl ("bloom_effectiveness",   bloom_eff, true);
    j << "}";

    HttpResponse resp;
    resp.body = j.str();
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// GET /api/lsm-state  — LSM tree dimensions and health
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_lsm_state() {
    std::lock_guard<std::mutex> guard(store_mutex_);
    size_t mem_entries  = store_.memtable_entries();
    size_t mem_bytes    = store_.active_byte_size();
    size_t flush_thresh = store_.flush_threshold_bytes();
    size_t l0           = store_.l0_count();
    size_t l1           = store_.l1_count();
    size_t l2           = store_.l2_count();
    size_t l0_limit     = store_.l0_hard_limit();
    bool   tainted      = store_.wal_tainted();

    double mem_fill_pct = (flush_thresh > 0)
        ? static_cast<double>(mem_bytes) /
          static_cast<double>(flush_thresh) * 100.0
        : 0.0;

    double l0_pressure_pct = (l0_limit > 0)
        ? static_cast<double>(l0) /
          static_cast<double>(l0_limit) * 100.0
        : 0.0;

    std::ostringstream j;
    j << "{\n";
    j << json_num ("memtable_entries",    static_cast<uint64_t>(mem_entries));
    j << json_num ("memtable_bytes",      static_cast<uint64_t>(mem_bytes));
    j << json_num ("flush_threshold",     static_cast<uint64_t>(flush_thresh));
    j << json_dbl ("memtable_fill_pct",   mem_fill_pct);
    j << json_num ("l0_count",            static_cast<uint64_t>(l0));
    j << json_num ("l1_count",            static_cast<uint64_t>(l1));
    j << json_num ("l2_count",            static_cast<uint64_t>(l2));
    j << json_num ("l0_limit",            static_cast<uint64_t>(l0_limit));
    j << json_dbl ("l0_pressure_pct",     l0_pressure_pct);
    j << json_bool("wal_tainted",         tainted, true);
    j << "}";

    HttpResponse resp;
    resp.body = j.str();
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// GET /api/debug/state
// Plain evidence snapshot used by the verification console.
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_debug_state() {
    std::lock_guard<std::mutex> guard(store_mutex_);
    const auto& m = store_.metrics();
    auto summary = store_.storage_summary();

    uint64_t wal_files = 0;
    uint64_t wal_bytes = 0;
    uint64_t sst_files = 0;
    uint64_t sst_bytes = 0;
    uint64_t vlog_bytes = 0;

    std::error_code ec;
    const std::filesystem::path data_dir("flsm_production");
    if (std::filesystem::exists(data_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
            if (ec) break;
            auto name = entry.path().filename().string();
            auto size = std::filesystem::is_regular_file(entry.path(), ec)
                ? static_cast<uint64_t>(std::filesystem::file_size(entry.path(), ec))
                : 0;
            if (name.rfind("wal_", 0) == 0 && entry.path().extension() == ".log") {
                wal_files++;
                wal_bytes += size;
            } else if (name.rfind("sst_", 0) == 0 && entry.path().extension() == ".sst") {
                sst_files++;
                sst_bytes += size;
            } else if (name == "vlog.bin") {
                vlog_bytes = size;
            }
        }
    }

    uint64_t total_disk_bytes = wal_bytes + sst_bytes + vlog_bytes;
    double space_amp = (summary.live_logical_bytes > 0)
        ? static_cast<double>(total_disk_bytes) / static_cast<double>(summary.live_logical_bytes)
        : 0.0;

    std::ostringstream j;
    j << "{\n";
    j << json_num ("memtable_entries",    static_cast<uint64_t>(store_.memtable_entries()));
    j << json_num ("memtable_bytes",      static_cast<uint64_t>(store_.active_byte_size()));
    j << json_num ("flush_threshold",     static_cast<uint64_t>(store_.flush_threshold_bytes()));
    j << json_num ("l0_count",            static_cast<uint64_t>(store_.l0_count()));
    j << json_num ("l1_count",            static_cast<uint64_t>(store_.l1_count()));
    j << json_num ("l2_count",            static_cast<uint64_t>(store_.l2_count()));
    j << json_bool("wal_tainted",         store_.wal_tainted());
    j << json_num ("wal_files",           wal_files);
    j << json_num ("wal_bytes",           wal_bytes);
    j << json_num ("sst_files",           sst_files);
    j << json_num ("sst_bytes",           sst_bytes);
    j << json_num ("vlog_bytes",          vlog_bytes);
    j << json_num ("total_disk_bytes",    total_disk_bytes);
    j << json_num ("live_keys_estimate",  summary.live_keys);
    j << json_num ("live_logical_bytes_estimate", summary.live_logical_bytes);
    j << json_num ("tombstones_estimate", summary.tombstones);
    j << json_dbl ("space_amplification_estimate", space_amp);
    j << json_num ("user_bytes_written",  m.user_bytes_written);
    j << json_num ("session_user_bytes_written", m.user_bytes_written);
    j << json_num ("storage_bytes_written", m.storage_bytes_written, true);
    j << "}";

    HttpResponse resp;
    resp.body = j.str();
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// GET /api/debug/files
// File-level storage evidence for the audit console.
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_debug_files() {
    const std::filesystem::path data_dir("flsm_production");
    const std::filesystem::path manifest_path = data_dir / "MANIFEST";

    auto file_size_json = [](const std::filesystem::path& path) -> uint64_t {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return 0;
        return static_cast<uint64_t>(std::filesystem::file_size(path, ec));
    };

    auto sst_name = [](uint32_t seq) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "sst_%06u.sst", seq);
        return std::string(buf);
    };

    std::vector<std::string> wal_rows;
    std::vector<std::filesystem::path> all_sst_paths;

    std::error_code ec;
    if (std::filesystem::exists(data_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
            if (ec) break;
            auto path = entry.path();
            auto name = path.filename().string();
            if (name.rfind("wal_", 0) == 0 && path.extension() == ".log") {
                std::ostringstream row;
                row << "{\"file\":\"" << json_escape(name)
                    << "\",\"bytes\":" << file_size_json(path) << "}";
                wal_rows.push_back(row.str());
            } else if (name.rfind("sst_", 0) == 0 && path.extension() == ".sst") {
                all_sst_paths.push_back(path);
            }
        }
    }

    Manifest manifest;
    bool manifest_loaded = manifest.load(manifest_path.string());
    std::set<std::string> referenced_ssts;
    if (manifest_loaded) {
        for (uint32_t seq : manifest.l0_seqs) referenced_ssts.insert(sst_name(seq));
        for (uint32_t seq : manifest.l1_seqs) referenced_ssts.insert(sst_name(seq));
        for (uint32_t seq : manifest.l2_seqs) referenced_ssts.insert(sst_name(seq));
    }

    std::vector<std::string> orphan_sst_rows;
    for (const auto& path : all_sst_paths) {
        auto name = path.filename().string();
        if (referenced_ssts.find(name) != referenced_ssts.end()) continue;
        std::ostringstream row;
        row << "{\"level\":\"unreferenced\",\"file\":\"" << json_escape(name)
            << "\",\"bytes\":" << file_size_json(path) << "}";
        orphan_sst_rows.push_back(row.str());
    }

    auto emit_sst_list = [&](const std::vector<uint32_t>& seqs, const std::string& level) {
        std::ostringstream out;
        bool first = true;
        for (uint32_t seq : seqs) {
            if (!first) out << ",";
            first = false;
            auto name = sst_name(seq);
            auto path = data_dir / name;
            out << "{\"level\":\"" << level
                << "\",\"sequence\":" << seq
                << ",\"file\":\"" << json_escape(name)
                << "\",\"bytes\":" << file_size_json(path)
                << ",\"exists\":" << (std::filesystem::exists(path) ? "true" : "false")
                << "}";
        }
        return out.str();
    };

    std::ostringstream j;
    j << "{\n";
    j << json_str("data_dir", data_dir.string());
    j << json_bool("manifest_loaded", manifest_loaded);
    j << json_num("manifest_version", manifest_loaded ? manifest.version : 0);
    j << json_num("manifest_bytes", file_size_json(manifest_path));
    j << "  \"wal_files\": [";
    for (size_t i = 0; i < wal_rows.size(); ++i) {
        if (i) j << ",";
        j << wal_rows[i];
    }
    j << "],\n";
    j << "  \"sstables\": [";
    if (manifest_loaded) {
        auto l0 = emit_sst_list(manifest.l0_seqs, "L0");
        auto l1 = emit_sst_list(manifest.l1_seqs, "L1");
        auto l2 = emit_sst_list(manifest.l2_seqs, "L2");
        j << l0;
        if (!l0.empty() && !l1.empty()) j << ",";
        j << l1;
        if ((!l0.empty() || !l1.empty()) && !l2.empty()) j << ",";
        j << l2;
    }
    j << "],\n";
    j << "  \"unreferenced_sstables\": [";
    for (size_t i = 0; i < orphan_sst_rows.size(); ++i) {
        if (i) j << ",";
        j << orphan_sst_rows[i];
    }
    j << "],\n";
    j << "  \"vlog\": {\"file\":\"vlog.bin\",\"bytes\":"
      << file_size_json(data_dir / "vlog.bin")
      << ",\"exists\":" << (std::filesystem::exists(data_dir / "vlog.bin") ? "true" : "false")
      << "}\n";
    j << "}";

    HttpResponse resp;
    resp.body = j.str();
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// POST /api/put  — {"key":"k","value":"v"}
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_put(const HttpRequest& req) {
    std::string key   = json_get_str(req.body, "key");
    std::string value = json_get_str(req.body, "value");
    HttpResponse resp;
    if (key.empty() || !json_has_key(req.body, "value")) {
        resp.status = 400;
        resp.body   = R"({"error":"key and value required"})";
        return resp;
    }
    try {
        std::lock_guard<std::mutex> guard(store_mutex_);
        store_.put(key, value);
        resp.body = R"({"ok":true})";
    } catch (const std::exception& e) {
        resp.status = 500;
        resp.body   = std::string(R"({"error":)") + "\"" + json_escape(e.what()) + "\"}";
    }
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// POST /api/get  — {"key":"k"}
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_get(const HttpRequest& req) {
    std::string key = json_get_str(req.body, "key");
    HttpResponse resp;
    if (key.empty()) {
        resp.status = 400;
        resp.body   = R"({"error":"key required"})";
        return resp;
    }
    std::string value;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(store_mutex_);
        found = store_.get(key, value);
    }
    if (found) {
        resp.body = std::string(R"({"found":true,"value":")") + json_escape(value) + "\"}";
    } else {
        resp.body = R"({"found":false})";
    }
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// POST /api/delete  — {"key":"k"}
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_delete(const HttpRequest& req) {
    std::string key = json_get_str(req.body, "key");
    HttpResponse resp;
    if (key.empty()) {
        resp.status = 400;
        resp.body   = R"({"error":"key required"})";
        return resp;
    }
    try {
        std::lock_guard<std::mutex> guard(store_mutex_);
        store_.delete_key(key);
        resp.body = R"({"ok":true})";
    } catch (const std::exception& e) {
        resp.status = 500;
        resp.body   = std::string(R"({"error":)") + "\"" + json_escape(e.what()) + "\"}";
    }
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// POST /api/bench  — {"type":"random_write","ops":500}
// Runs a mini benchmark synchronously (ops capped at 2000 for web).
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_bench(const HttpRequest& req) {
    std::string type = json_get_str(req.body, "type");
    long long ops    = json_get_int(req.body, "ops", 500);

    if (type != "random_write"    && type != "sequential_write" &&
        type != "random_read"     && type != "mixed") {
        HttpResponse bad;
        bad.status = 400;
        bad.body   = R"({"error":"invalid type"})";
        return bad;
    }

    // Cap at 2000 ops so the HTTP request doesn't time out in the browser
    if (ops < 50)   ops = 50;
    if (ops > 2000) ops = 2000;

    std::lock_guard<std::mutex> guard(store_mutex_);
    store_.metrics().reset();
    auto t0 = std::chrono::high_resolution_clock::now();

    std::string dummy_val(100, 'x');
    for (int i = 0; i < ops; ++i) {
        if (type == "random_write") {
            store_.put("key_" + std::to_string(rand() % ops), dummy_val);
        } else if (type == "sequential_write") {
            char buf[32];
            snprintf(buf, sizeof(buf), "seq_%08d", i);
            store_.put(buf, dummy_val);
        } else if (type == "random_read") {
            std::string val;
            store_.get("key_" + std::to_string(rand() % ops), val);
        } else if (type == "mixed") {
            if (rand() % 100 < 70) {
                std::string val;
                store_.get("key_" + std::to_string(rand() % ops), val);
            } else {
                store_.put("key_" + std::to_string(rand() % ops), dummy_val);
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();

    double elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
    double throughput = (elapsed_s > 0) ? ops / elapsed_s : 0.0;
    const auto& m = store_.metrics();
    double write_amp = (m.user_bytes_written > 0)
        ? static_cast<double>(m.storage_bytes_written) / static_cast<double>(m.user_bytes_written)
        : 0.0;
    double read_amp = (m.get_calls > 0)
        ? static_cast<double>(m.sst_searches + m.vlog_reads) / static_cast<double>(m.get_calls)
        : 0.0;

    std::ostringstream j;
    j << "{\n";
    j << json_str ("type",            type);
    j << json_num ("ops",             static_cast<uint64_t>(ops));
    j << json_dbl ("elapsed_s",       elapsed_s);
    j << json_dbl ("throughput",      throughput);
    j << json_dbl ("write_amp",       write_amp);
    j << json_dbl ("read_amp",        read_amp, true);
    j << "}";

    HttpResponse resp;
    resp.body = j.str();
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// POST /api/iot/bulk — production IoT event generator
// Writes deterministic sensor events into the live production store.
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_iot_bulk(const HttpRequest& req) {
    if (stream_running_) {
        HttpResponse busy;
        busy.status = 400;
        busy.body = R"({"error":"background stream is running; stop it before bulk writes"})";
        return busy;
    }
    std::lock_guard<std::mutex> guard(store_mutex_);
    long long requested_events = json_get_int(req.body, "events", 1000);
    long long requested_devices = json_get_int(req.body, "devices", 16);
    if (requested_events < 1) requested_events = 1;
    if (requested_events > 100000) requested_events = 100000;
    if (requested_devices < 1) requested_devices = 1;
    if (requested_devices > 1000) requested_devices = 1000;

    const uint64_t l0_before = static_cast<uint64_t>(store_.l0_count());
    const uint64_t l1_before = static_cast<uint64_t>(store_.l1_count());
    const uint64_t mem_before = static_cast<uint64_t>(store_.memtable_entries());
    const uint64_t user_before = store_.metrics().user_bytes_written;
    const uint64_t storage_before = store_.metrics().storage_bytes_written;

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::string> sample_keys;
    std::vector<std::string> sample_values;
    std::filesystem::create_directories("iot_workloads");
    const std::string workload_path = "iot_workloads/latest_production_workload.jsonl";
    std::ofstream workload_file(workload_path, std::ios::trunc);

    try {
        for (long long i = 0; i < requested_events; ++i) {
            long long device = i % requested_devices;
            long long minute = i / requested_devices;
            double temperature = 20.0 + static_cast<double>((i * 37) % 1700) / 100.0;
            double vibration = 0.15 + static_cast<double>((i * 17) % 900) / 1000.0;
            double voltage = 218.0 + static_cast<double>((i * 29) % 900) / 100.0;
            std::string status = (i % 97 == 0) ? "alert" : "normal";

            char key_buf[96];
            std::snprintf(key_buf, sizeof(key_buf), "iot:device_%04lld:event_%010lld", device, i);

            std::ostringstream value;
            value << "{\"device_id\":\"device_" << std::setw(4) << std::setfill('0') << device << "\","
                  << "\"site\":\"edge_lab_A\","
                  << "\"sensor\":\"machine_health\","
                  << "\"timestamp_minute\":" << minute << ","
                  << "\"sequence\":" << i << ","
                  << "\"temperature_c\":" << std::fixed << std::setprecision(2) << temperature << ","
                  << "\"vibration_g\":" << std::fixed << std::setprecision(3) << vibration << ","
                  << "\"voltage_v\":" << std::fixed << std::setprecision(2) << voltage << ","
                  << "\"status\":\"" << status << "\"}";

            std::string key = key_buf;
            std::string payload = value.str();
            if (workload_file) {
                workload_file << "{\"op\":\"put\",\"key\":\"" << json_escape(key)
                              << "\",\"value\":\"" << json_escape(payload) << "\"}\n";
            }
            store_.put(key, payload);

            if (i == 0 || i == requested_events / 2 || i == requested_events - 1) {
                sample_keys.push_back(key);
                sample_values.push_back(payload);
            }
        }
    } catch (const std::exception& e) {
        HttpResponse resp;
        resp.status = 500;
        resp.body = std::string(R"({"error":")") + json_escape(e.what()) + "\"}";
        return resp;
    }

    uint64_t sample_checked = 0;
    uint64_t sample_mismatches = 0;
    for (size_t i = 0; i < sample_keys.size(); ++i) {
        std::string actual;
        bool found = store_.get(sample_keys[i], actual);
        sample_checked++;
        if (!found || actual != sample_values[i]) sample_mismatches++;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
    double throughput = elapsed_s > 0 ? static_cast<double>(requested_events) / elapsed_s : 0.0;

    const uint64_t l0_after = static_cast<uint64_t>(store_.l0_count());
    const uint64_t l1_after = static_cast<uint64_t>(store_.l1_count());
    const uint64_t mem_after = static_cast<uint64_t>(store_.memtable_entries());
    const uint64_t user_delta = store_.metrics().user_bytes_written - user_before;
    const uint64_t storage_delta = store_.metrics().storage_bytes_written - storage_before;

    std::ostringstream j;
    j << "{\n";
    j << json_str("status", sample_mismatches == 0 ? "pass" : "fail");
    j << json_num("events_requested", static_cast<uint64_t>(requested_events));
    j << json_num("events_written", static_cast<uint64_t>(requested_events));
    j << json_num("devices", static_cast<uint64_t>(requested_devices));
    j << json_num("sample_checked", sample_checked);
    j << json_num("sample_mismatches", sample_mismatches);
    j << json_num("memtable_entries_before", mem_before);
    j << json_num("memtable_entries_after", mem_after);
    j << json_num("l0_before", l0_before);
    j << json_num("l0_after", l0_after);
    j << json_num("l1_before", l1_before);
    j << json_num("l1_after", l1_after);
    j << json_num("user_bytes_delta", user_delta);
    j << json_num("storage_bytes_delta", storage_delta);
    j << json_str("workload_file", workload_path);
    j << json_dbl("elapsed_s", elapsed_s);
    j << json_dbl("throughput", throughput);
    j << "  \"trace\": [";
    j << "{\"phase\":\"write\",\"detail\":\"Generated " << requested_events
      << " deterministic IoT JSON payloads across " << requested_devices << " devices\"},";
    j << "{\"phase\":\"storage\",\"detail\":\"L0 changed " << l0_before << " -> " << l0_after
      << ", L1 changed " << l1_before << " -> " << l1_after << "\"},";
    j << "{\"phase\":\"sample-read\",\"detail\":\"Checked " << sample_checked
      << " representative keys with " << sample_mismatches << " mismatches\"}";
    j << "]\n";
    j << "}";

    HttpResponse resp;
    resp.body = j.str();
    return resp;
}

void HttpServer::append_stream_log(const std::string& line) {
    std::lock_guard<std::mutex> guard(stream_mutex_);
    stream_log_.push_back(line);
    while (stream_log_.size() > 120) stream_log_.pop_front();
}

void HttpServer::run_stream_worker(uint64_t operations, uint64_t devices) {
    std::filesystem::create_directories("iot_workloads");
    const std::string workload_path = "iot_workloads/latest_production_stream.jsonl";
    std::ofstream workload_file(workload_path, std::ios::trunc);

    {
        std::lock_guard<std::mutex> guard(stream_mutex_);
        stream_workload_file_ = workload_path;
        stream_status_ = "running";
    }

    append_stream_log("stream:start | devices=" + std::to_string(devices) +
                      " operations=" + std::to_string(operations));

    for (uint64_t i = 0; i < operations && !stream_stop_; ++i) {
        uint64_t device = i % devices;
        uint64_t op_bucket = i % 100;
        std::string op;
        std::string key;
        std::string value;
        bool mismatch = false;

        if (op_bucket < 80) {
            op = "put";
            key = "iot:device_" + std::to_string(device) + ":telemetry:" + std::to_string(i);
            std::ostringstream payload;
            payload << "{\"device_id\":\"device_" << device
                    << "\",\"sequence\":" << i
                    << ",\"op\":\"telemetry\","
                    << "\"temperature_c\":" << std::fixed << std::setprecision(2)
                    << (20.0 + static_cast<double>((i * 31) % 1800) / 100.0)
                    << ",\"vibration_g\":" << std::fixed << std::setprecision(3)
                    << (0.10 + static_cast<double>((i * 13) % 700) / 1000.0)
                    << "}";
            value = payload.str();
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                store_.put(key, value);
            }
            {
                std::lock_guard<std::mutex> guard(stream_mutex_);
                stream_puts_++;
            }
        } else if (op_bucket < 90) {
            op = "update";
            key = "iot:device_" + std::to_string(device) + ":config";
            value = "{\"device_id\":\"device_" + std::to_string(device) +
                    "\",\"op\":\"config_update\",\"sampling_ms\":" +
                    std::to_string(250 + (i % 10) * 50) + "}";
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                store_.put(key, value);
            }
            {
                std::lock_guard<std::mutex> guard(stream_mutex_);
                stream_updates_++;
            }
        } else if (op_bucket < 95) {
            op = "delete";
            uint64_t victim = (i > devices) ? (i - devices) : i;
            key = "iot:device_" + std::to_string(victim % devices) + ":telemetry:" + std::to_string(victim);
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                store_.delete_key(key);
            }
            {
                std::lock_guard<std::mutex> guard(stream_mutex_);
                stream_deletes_++;
            }
        } else {
            op = "get";
            key = "iot:device_" + std::to_string(device) + ":config";
            std::string actual;
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                store_.get(key, actual);
            }
            {
                std::lock_guard<std::mutex> guard(stream_mutex_);
                stream_gets_++;
            }
        }

        if (workload_file) {
            workload_file << "{\"op\":\"" << op << "\",\"key\":\"" << json_escape(key) << "\"";
            if (!value.empty()) workload_file << ",\"value\":\"" << json_escape(value) << "\"";
            workload_file << "}\n";
        }

        {
            std::lock_guard<std::mutex> guard(stream_mutex_);
            stream_completed_++;
            if (mismatch) stream_mismatches_++;
        }

        append_stream_log(op + " | key=" + key);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        std::lock_guard<std::mutex> guard(stream_mutex_);
        stream_status_ = stream_stop_ ? "stopped" : "completed";
    }
    append_stream_log("stream:" + std::string(stream_stop_ ? "stopped" : "completed"));
    stream_running_ = false;
}

HttpResponse HttpServer::handle_stream_start(const HttpRequest& req) {
    if (stream_running_) {
        HttpResponse busy;
        busy.status = 400;
        busy.body = R"({"error":"stream already running"})";
        return busy;
    }

    uint64_t operations = static_cast<uint64_t>(std::clamp(json_get_int(req.body, "operations", 10000), 100LL, 500000LL));
    uint64_t devices = static_cast<uint64_t>(std::clamp(json_get_int(req.body, "devices", 100), 1LL, 5000LL));

    if (stream_thread_.joinable()) stream_thread_.join();

    {
        std::lock_guard<std::mutex> guard(stream_mutex_);
        stream_target_ = operations;
        stream_completed_ = 0;
        stream_puts_ = 0;
        stream_updates_ = 0;
        stream_deletes_ = 0;
        stream_gets_ = 0;
        stream_mismatches_ = 0;
        stream_devices_ = devices;
        stream_status_ = "starting";
        stream_workload_file_.clear();
        stream_log_.clear();
    }

    stream_stop_ = false;
    stream_running_ = true;
    stream_thread_ = std::thread(&HttpServer::run_stream_worker, this, operations, devices);

    HttpResponse resp;
    resp.body = R"({"ok":true,"status":"starting"})";
    return resp;
}

HttpResponse HttpServer::handle_stream_stop() {
    stream_stop_ = true;
    if (stream_thread_.joinable()) stream_thread_.join();
    stream_running_ = false;
    HttpResponse resp;
    resp.body = R"({"ok":true})";
    return resp;
}

HttpResponse HttpServer::handle_stream_status() {
    std::lock_guard<std::mutex> guard(stream_mutex_);
    double progress = stream_target_ > 0
        ? static_cast<double>(stream_completed_) / static_cast<double>(stream_target_) * 100.0
        : 0.0;

    std::ostringstream j;
    j << "{\n";
    j << json_bool("running", stream_running_);
    j << json_str("status", stream_status_);
    j << json_num("target_operations", stream_target_);
    j << json_num("completed_operations", stream_completed_);
    j << json_num("devices", stream_devices_);
    j << json_num("puts", stream_puts_);
    j << json_num("updates", stream_updates_);
    j << json_num("deletes", stream_deletes_);
    j << json_num("gets", stream_gets_);
    j << json_num("mismatches", stream_mismatches_);
    j << json_str("workload_file", stream_workload_file_);
    j << json_dbl("progress_pct", progress);
    j << "  \"log\": [";
    for (size_t i = 0; i < stream_log_.size(); ++i) {
        if (i) j << ",";
        j << "\"" << json_escape(stream_log_[i]) << "\"";
    }
    j << "]\n";
    j << "}";

    HttpResponse resp;
    resp.body = j.str();
    return resp;
}

// ─────────────────────────────────────────────────────────────────
// POST /api/demo/run — small-threshold visual LSM demo
// Demonstrates writes, overwrites, deletes, flushes, L0->L1, and L1->L2.
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_demo_run(const HttpRequest& req) {
    long long requested_events = json_get_int(req.body, "events", 240);
    if (requested_events < 60) requested_events = 60;
    if (requested_events > 2000) requested_events = 2000;

    const std::string demo_dir = "flsm_demo_lab";
    const std::string trace_path = demo_dir + "/engine_trace.log";
    std::error_code ec;
    std::filesystem::remove_all(demo_dir, ec);

    KVStoreOptions options;
    options.flush_threshold = 32u * 1024u;
    options.l0_hard_limit = 2;
    options.l1_hard_limit = 2;
    options.trace_enabled = true;
    options.trace_path = trace_path;

    std::vector<std::string> timeline;
    auto add_step = [&](const std::string& phase, KVStore& db) {
        std::ostringstream row;
        row << "{\"phase\":\"" << json_escape(phase)
            << "\",\"memtable_entries\":" << db.memtable_entries()
            << ",\"l0\":" << db.l0_count()
            << ",\"l1\":" << db.l1_count()
            << ",\"l2\":" << db.l2_count()
            << "}";
        timeline.push_back(row.str());
    };

    try {
        KVStore db(demo_dir, options);
        add_step("empty demo store", db);

        std::string pad(900, 'x');
        for (long long i = 0; i < requested_events; ++i) {
            std::ostringstream value;
            value << "{\"device_id\":\"demo_device_" << (i % 8)
                  << "\",\"sequence\":" << i
                  << ",\"kind\":\"insert\",\"payload\":\"" << pad << "\"}";
            db.put("demo:event:" + std::to_string(i), value.str());
        }
        add_step("bulk inserts reached multiple memtable flushes", db);

        for (long long i = 0; i < requested_events / 6; ++i) {
            std::ostringstream value;
            value << "{\"device_id\":\"demo_device_" << (i % 8)
                  << "\",\"sequence\":" << i
                  << ",\"kind\":\"overwrite\",\"payload\":\"" << pad << "\"}";
            db.put("demo:event:" + std::to_string(i), value.str());
        }
        add_step("overwrites added newer versions", db);

        for (long long i = requested_events / 2; i < requested_events / 2 + requested_events / 10; ++i) {
            db.delete_key("demo:event:" + std::to_string(i));
        }
        db.force_flush();
        add_step("deletes written as tombstones and flushed to L0", db);

        db.force_compaction();
        add_step("forced L0 to L1 compaction", db);

        for (long long i = requested_events; i < requested_events + requested_events / 3; ++i) {
            std::ostringstream value;
            value << "{\"device_id\":\"demo_device_" << (i % 8)
                  << "\",\"sequence\":" << i
                  << ",\"kind\":\"second-wave\",\"payload\":\"" << pad << "\"}";
            db.put("demo:event:" + std::to_string(i), value.str());
        }
        db.force_flush();
        db.force_compaction();
        add_step("second wave compacted into another L1 run", db);

        db.force_l1_compaction();
        add_step("forced L1 to L2 compaction", db);

        std::string deleted_value;
        bool deleted_found = db.get("demo:event:" + std::to_string(requested_events / 2), deleted_value);
        std::string overwritten_value;
        bool overwritten_found = db.get("demo:event:0", overwritten_value);

        std::vector<std::string> trace_lines;
        std::ifstream trace(trace_path);
        std::string line;
        while (std::getline(trace, line)) {
            trace_lines.push_back(line);
            if (trace_lines.size() > 80) trace_lines.erase(trace_lines.begin());
        }

        std::ostringstream j;
        j << "{\n";
        j << json_str("status", (!deleted_found && overwritten_found) ? "pass" : "fail");
        j << json_num("events_requested", static_cast<uint64_t>(requested_events));
        j << json_num("flush_threshold", static_cast<uint64_t>(options.flush_threshold));
        j << json_num("l0_limit", static_cast<uint64_t>(options.l0_hard_limit));
        j << json_num("l1_limit", static_cast<uint64_t>(options.l1_hard_limit));
        j << json_num("final_memtable_entries", static_cast<uint64_t>(db.memtable_entries()));
        j << json_num("final_l0", static_cast<uint64_t>(db.l0_count()));
        j << json_num("final_l1", static_cast<uint64_t>(db.l1_count()));
        j << json_num("final_l2", static_cast<uint64_t>(db.l2_count()));
        j << json_bool("deleted_key_found", deleted_found);
        j << json_bool("overwritten_key_found", overwritten_found);
        j << json_str("trace_file", trace_path);
        j << "  \"timeline\": [";
        for (size_t i = 0; i < timeline.size(); ++i) {
            if (i) j << ",";
            j << timeline[i];
        }
        j << "],\n";
        j << "  \"trace\": [";
        for (size_t i = 0; i < trace_lines.size(); ++i) {
            if (i) j << ",";
            j << "\"" << json_escape(trace_lines[i]) << "\"";
        }
        j << "]\n";
        j << "}";

        HttpResponse resp;
        resp.body = j.str();
        return resp;
    } catch (const std::exception& e) {
        HttpResponse resp;
        resp.status = 500;
        resp.body = std::string(R"({"error":")") + json_escape(e.what()) + "\"}";
        return resp;
    }
}

// ─────────────────────────────────────────────────────────────────
// POST /api/verify/<name> — isolated Edge/IoT event-store verification
// ─────────────────────────────────────────────────────────────────
HttpResponse HttpServer::handle_verify(const HttpRequest& req, const std::string& test) {
    long long requested_ops = json_get_int(req.body, "ops", 5000);
    VerificationOptions options;
    options.test = test;
    options.ops = static_cast<int>(requested_ops);

    HttpResponse resp;
    resp.body = run_verification(options);
    return resp;
}
// ─────────────────────────────────────────────────────────────────
// JSON helpers — minimal hand-rolled serialization
// ─────────────────────────────────────────────────────────────────
std::string HttpServer::json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string HttpServer::json_str(const std::string& key, const std::string& val, bool last) {
    return "  \"" + key + "\": \"" + json_escape(val) + "\"" + (last ? "\n" : ",\n");
}

std::string HttpServer::json_num(const std::string& key, uint64_t val, bool last) {
    return "  \"" + key + "\": " + std::to_string(val) + (last ? "\n" : ",\n");
}

std::string HttpServer::json_dbl(const std::string& key, double val, bool last) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << val;
    return "  \"" + key + "\": " + ss.str() + (last ? "\n" : ",\n");
}

std::string HttpServer::json_bool(const std::string& key, bool val, bool last) {
    return "  \"" + key + "\": " + (val ? "true" : "false") + (last ? "\n" : ",\n");
}

bool HttpServer::json_has_key(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}

// Minimal flat JSON string extractor — finds "key": "value" or "key":"value"
std::string HttpServer::json_get_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";

    // Skip past the key and find the colon
    pos += needle.size();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    // Skip whitespace and find the opening quote
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";

    // Find the closing quote, handling escaped quotes
    std::string val;
    ++pos; // skip opening quote
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            if (next == '"')  { val += '"';  pos += 2; continue; }
            if (next == '\\') { val += '\\'; pos += 2; continue; }
            if (next == 'n')  { val += '\n'; pos += 2; continue; }
            if (next == 'r')  { val += '\r'; pos += 2; continue; }
            if (next == 't')  { val += '\t'; pos += 2; continue; }
            if (next == 'b')  { val += '\b'; pos += 2; continue; }
            if (next == 'f')  { val += '\f'; pos += 2; continue; }
            if (next == 'u' && pos + 5 < json.size()) {
                std::string hex = json.substr(pos + 2, 4);
                try {
                    int cp = std::stoi(hex, nullptr, 16);
                    if (cp <= 0x7F) {
                        val += static_cast<char>(cp);
                    } else if (cp <= 0x7FF) {
                        val += static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
                        val += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        val += static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
                        val += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        val += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    pos += 6;
                    continue;
                } catch (...) {
                    // fallthrough to default if hex parsing fails
                }
            }
            val += next; pos += 2; continue;
        }
        if (c == '"') break;
        val += c;
        ++pos;
    }
    return val;
}

long long HttpServer::json_get_int(const std::string& json, const std::string& key,
                                   long long default_val) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return default_val;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return default_val;
    // Skip whitespace
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    // Check if it looks like a number
    if (pos >= json.size() || (!std::isdigit(json[pos]) && json[pos] != '-'))
        return default_val;
    try {
        return std::stoll(json.substr(pos));
    } catch (...) {
        return default_val;
    }
}

// ─────────────────────────────────────────────────────────────────
// HTTP status text
// ─────────────────────────────────────────────────────────────────
const char* HttpServer::status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}
