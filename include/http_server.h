#ifndef FLSM_HTTP_SERVER_H
#define FLSM_HTTP_SERVER_H

// ── Platform socket abstraction ───────────────────────────────────
// On Windows (MinGW/MSVC) we use Winsock2.
// On POSIX (Linux/macOS — Docker target) we use standard BSD sockets.
// ─────────────────────────────────────────────────────────────────
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
  // close() equivalent for SOCKET handles:
  inline void close_sock(socket_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int socket_t;
  static constexpr socket_t INVALID_SOCK = -1;
  inline void close_sock(socket_t s) { ::close(s); }
#endif

#include "kvstore.h"
#include <atomic>
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// ── Parsed HTTP request ───────────────────────────────────────────
struct HttpRequest {
    std::string method;   // "GET", "POST", "OPTIONS"
    std::string path;     // "/api/metrics"
    std::string body;     // raw request body (for POST)
    std::unordered_map<std::string, std::string> headers;
};

// ── HTTP response to be serialized and sent ───────────────────────
struct HttpResponse {
    int         status       = 200;
    std::string content_type = "application/json";
    std::string body;
};

inline uint64_t stream2_device_for_operation(uint64_t operation_index,
                                             uint64_t op_bucket,
                                             uint64_t devices) {
    if (devices == 0) return 0;
    return (((operation_index / 100) * 17 + op_bucket * 13 + 7) % devices);
}

inline const char* stream2_state_family(uint64_t operation_index, uint64_t op_bucket) {
    static constexpr std::array<const char*, 5> families = {
        "config:sampling",
        "config:threshold",
        "status:health",
        "status:firmware",
        "calibration:vibration"
    };
    return families[(operation_index / 100 + op_bucket) % families.size()];
}

// ── HttpServer ────────────────────────────────────────────────────
//
// A minimal single-threaded HTTP/1.1 server built on raw BSD sockets
// (or Winsock2 on Windows). No dependencies beyond the C++ standard
// library and the platform socket API.
//
// Accepted routes:
//   GET  /                  → serve web/index.html
//   GET  /style.css         → serve web/style.css
//   GET  /app.js            → serve web/app.js
//   GET  /api/metrics       → EngineMetrics + derived ratios (JSON)
//   GET  /api/lsm-state     → LSM tree state (JSON)
//   GET  /api/debug/state   → Current storage state evidence (JSON)
//   GET  /api/debug/files   → WAL/SSTable/VLog/Manifest file evidence (JSON)
//   POST /api/put           → body: {"key":"k","value":"v"}
//   POST /api/bulk/put      → body: {"records":[{"key":"k","value":"v"}]}
//   POST /api/get           → body: {"key":"k"}
//   POST /api/delete        → body: {"key":"k"}
//   POST /api/bench         → body: {"type":"random_write","ops":500}
//   POST /api/iot/bulk      → body: {"events":10000,"devices":16}
//   POST /api/production/reset → reset production store directory
//   POST /api/stream/start  → start background mixed production IoT stream
//   POST /api/stream/stop   → stop background production stream
//   GET  /api/stream/status → stream counters and recent trace
//   POST /api/demo/run      → run small-threshold visual LSM demo
//   POST /api/verify/<name> → isolated correctness verification run
//   POST /api/experiment/run → run user-authored isolated experiment script
//
// All API responses include CORS headers so the dashboard can be
// loaded from any origin during development.
// ─────────────────────────────────────────────────────────────────
class HttpServer {
public:
    explicit HttpServer(KVStore& store, int port = 8080);
    ~HttpServer();

    // Start the blocking accept loop. Returns only on fatal error.
    void run();

private:
    // ── Connection handling ───────────────────────────────────────
    void       handle_client(socket_t client_fd);
    bool       read_request(socket_t fd, std::string& raw);
    bool       parse_request(const std::string& raw, HttpRequest& req);
    std::string build_response(const HttpResponse& resp);

    // ── Router ────────────────────────────────────────────────────
    HttpResponse route(const HttpRequest& req);

    // ── Route handlers ────────────────────────────────────────────
    HttpResponse handle_static(const std::string& rel_path);
    HttpResponse handle_metrics();
    HttpResponse handle_lsm_state();
    HttpResponse handle_debug_state();
    HttpResponse handle_debug_files();
    HttpResponse handle_trace_status();
    HttpResponse handle_trace_read();
    HttpResponse handle_trace_toggle(const HttpRequest& req);
    HttpResponse handle_put(const HttpRequest& req);
    HttpResponse handle_bulk_put(const HttpRequest& req);
    HttpResponse handle_get(const HttpRequest& req);
    HttpResponse handle_delete(const HttpRequest& req);
    HttpResponse handle_bench(const HttpRequest& req);
    HttpResponse handle_iot_bulk(const HttpRequest& req);
    HttpResponse handle_production_reset();
    HttpResponse handle_stream_start(const HttpRequest& req);
    HttpResponse handle_stream_stop();
    HttpResponse handle_stream_status();
    HttpResponse handle_demo_run(const HttpRequest& req);
    HttpResponse handle_verify(const HttpRequest& req, const std::string& test);
    HttpResponse handle_experiment_run(const HttpRequest& req);
    void run_stream_worker(uint64_t operations, uint64_t devices, const std::string& mode);
    void append_stream_log(const std::string& line);

    // ── Minimal hand-rolled JSON helpers ─────────────────────────
    // These avoid any external JSON library dependency.
    static std::string json_escape(const std::string& s);
    static std::string json_str(const std::string& key, const std::string& val, bool last = false);
    static std::string json_num(const std::string& key, uint64_t val, bool last = false);
    static std::string json_dbl(const std::string& key, double   val, bool last = false);
    static std::string json_bool(const std::string& key, bool    val, bool last = false);

    // Minimal JSON body parser — extracts a string value for a given key
    // from a flat {"k":"v",...} object. Returns "" if key not found.
    static bool        json_has_key(const std::string& json, const std::string& key);
    static std::string json_get_str(const std::string& json, const std::string& key);
    static long long   json_get_int(const std::string& json, const std::string& key,
                                    long long default_val = 0);

    // ── HTTP utilities ────────────────────────────────────────────
    static const char* status_text(int code);

    // ── Members ───────────────────────────────────────────────────
    KVStore&  store_;
    int       port_;
    socket_t  server_fd_;

    mutable std::mutex store_mutex_;
    mutable std::mutex stream_mutex_;
    std::thread stream_thread_;
    std::atomic<bool> stream_running_{false};
    std::atomic<bool> stream_stop_{false};
    uint64_t stream_target_ = 0;
    uint64_t stream_completed_ = 0;
    uint64_t stream_puts_ = 0;
    uint64_t stream_updates_ = 0;
    uint64_t stream_deletes_ = 0;
    uint64_t stream_gets_ = 0;
    uint64_t stream_mismatches_ = 0;
    uint64_t stream_devices_ = 0;
    uint64_t stream_user_before_ = 0;
    uint64_t stream_storage_before_ = 0;
    uint64_t stream_mem_before_ = 0;
    uint64_t stream_l0_before_ = 0;
    uint64_t stream_l1_before_ = 0;
    uint64_t stream_l2_before_ = 0;
    uint64_t stream_unique_telemetry_ = 0;
    uint64_t stream_unique_state_ = 0;
    uint64_t stream_successful_delete_targets_ = 0;
    std::string stream_mode_ = "stream1";
    std::string stream_status_ = "idle";
    std::string stream_workload_file_;
    std::deque<std::string> stream_log_;
};

#endif // FLSM_HTTP_SERVER_H
