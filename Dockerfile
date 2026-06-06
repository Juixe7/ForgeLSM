# ── Stage 1: Build ────────────────────────────────────────────────
# Use the official GCC 13 image on Debian slim — gives us C++20 support
# without pulling in the full Ubuntu bloat.
FROM gcc:13-bookworm AS builder

WORKDIR /build

# Copy source tree
COPY . .

# Compile the binary. Linux uses standard POSIX sockets; no -lws2_32 needed.
RUN g++ -std=c++20 -O2 -Wall -Wextra -Iinclude \
        src/crc32.cpp \
        src/wal.cpp \
        src/vlog.cpp \
        src/sstable.cpp \
        src/memtable.cpp \
        src/manifest.cpp \
        src/compaction.cpp \
        src/vlog_gc.cpp \
        src/bloom.cpp \
        src/benchmark.cpp \
        src/cli.cpp \
        src/kvstore.cpp \
        src/verification.cpp \
        src/http_server.cpp \
        main.cpp \
        -static-libstdc++ -static-libgcc \
        -o flsm

# ── Stage 2: Runtime ──────────────────────────────────────────────
# Debian slim — minimal footprint, just enough to run the binary.
FROM debian:bookworm-slim AS runtime

# Install libstdc++ runtime (bundled with GCC, needed for std::filesystem)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the compiled binary
COPY --from=builder /build/flsm ./flsm

# Copy the web dashboard assets
COPY --from=builder /build/web ./web

# Expose the dashboard port
EXPOSE 8080

# Health check — poll /api/metrics every 30 seconds
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD wget -qO- http://localhost:8080/api/metrics || exit 1

# Default: start the HTTP server in web mode
CMD ["./flsm", "web"]
