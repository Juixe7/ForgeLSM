# ForgeLSM

**ForgeLSM is a C++20 WiscKey-style LSM key-value storage engine built from scratch for edge/IoT event storage experiments.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](#build)
[![Tests](https://img.shields.io/badge/tests-82%20passing-brightgreen)](#testing)
[![Docker](https://img.shields.io/badge/docker-ready-blue)](#docker)

ForgeLSM separates small keys from larger values: keys, tombstones, and value pointers are indexed through an LSM tree, while event payloads are appended to a value log. The project is aimed at demonstrating how an embedded event store can survive restarts, compact old files, skip unnecessary reads with Bloom filters, and prove correctness through deterministic workloads.

## Current Capabilities

- Write-ahead log recovery with checksum validation and corrupt-tail truncation.
- Memtable writes, flushes, SSTable files, Bloom filters, and dynamically tracked levels.
- WiscKey-style value log storage for payload bytes.
- VLog garbage collection with crash-state recovery.
- L0 -> L1 -> L2 compaction with tombstone preservation for deleted keys.
- Atomic SSTable commits using temp file write, fsync, and rename.
- Persistent production statistics in a `STATS` file.
- Production IoT stream generator with mixed puts, updates, deletes, and gets.
- MQTT-style IoT simulator mode with topic-shaped payloads through the production stream path.
- External MQTT broker bridge tooling for realistic broker -> ingestion -> ForgeLSM demos.
- Browser-visible verbose engine trace console for WAL, VLog, memtable, flush, compaction, and recovery events.
- Experiment Lab that runs scripted workloads against an in-memory reference model.
- Minimal raw C++ HTTP server and browser UI served from the same binary.

## Why This Project Exists

IoT edge devices generate many small events over time. A plain file append is easy but weak for lookups, updates, deletes, recovery, and compaction. A traditional B-tree updates pages in place, which can increase random writes on flash storage.

An LSM tree takes a different approach:

1. New writes first land in memory and the WAL.
2. Memory is flushed into immutable sorted files called SSTables.
3. Background compaction merges older files and removes overwritten values or tombstones when safe.
4. Bloom filters quickly reject files that cannot contain a searched key.

ForgeLSM uses this design to show the tradeoff clearly: writes are append-friendly, reads may need to check multiple levels, and compaction converts many immutable files into cleaner lower-level files.

## Architecture

```
Browser UI
   |
   v
Raw C++ HTTP server
   |
   v
KVStore
   |
   +-- WAL: crash recovery log
   +-- Memtable: in-memory sorted index
   +-- SSTables: immutable sorted key -> value-pointer files
   +-- Bloom filters: negative lookup accelerator
   +-- Manifest: level/file metadata
   +-- VLog: append-only payload storage
```

### Write Path

1. Append the value bytes to `vlog.bin`.
2. Append the key, value pointer, and tombstone flag to the WAL.
3. Insert or overwrite the key in the memtable.
4. When the memtable reaches its threshold, flush it to an SSTable.
5. When level limits are reached, compact L0 into L1, L1 into L2, and deeper levels as needed.

### Read Path

1. Check the memtable first.
2. Search newer L0 SSTables before older L0 SSTables.
3. Search L1 and L2 in order.
4. Use Bloom filters to skip SSTables that definitely do not contain the key.
5. If a value pointer is found, read the payload from the VLog.
6. If a tombstone is found, report the key as deleted.

### Recovery Path

1. Load the manifest and known SSTables.
2. Replay WAL records into the memtable.
3. Stop cleanly at the last valid WAL record if a corrupt tail is found.
4. Truncate the corrupt tail so later boots start cleanly.

## Build

ForgeLSM requires a C++20 compiler. On Linux/macOS:

```bash
make
./flsm
```

On Windows with MinGW:

```powershell
mingw32-make
.\flsm.exe
```

Running the binary without arguments executes the test suite. Use `cli` for the terminal interface or `web` for the browser console.

## Web Console

Run the web server and open `http://localhost:8080`.

```bash
make
./flsm web 8080
```

On Windows with MinGW:

```powershell
mingw32-make
.\flsm.exe web 8080
```

The UI currently has two main tabs.

### Production IoT

This tab uses the real production directory: `flsm_production`.

It is used to observe a persistent store across runs. The reset button clears this production store when you want a fresh experiment.

Main controls:

- Run Stream 1: original deterministic mixed IoT workload.
- Run Stream 2: improved mixed workload with wider key distribution and more realistic updates/deletes.
- Run MQTT-style: topic/payload-shaped device messages that mimic MQTT ingestion semantics while still using the local engine API.
- Stop Stream: stop a running stream.
- Reset Store: remove production data and start clean.
- Engine Terminal Log: enable or disable verbose engine tracing and inspect the latest engine-level events.

Main evidence shown:

- Last run result: operations requested, operations completed, logical bytes, physical bytes, and trace.
- Current database state: memtable entries, WAL size, SSTable counts, VLog size, live keys, tombstones, disk usage, and amplification estimates.
- Storage internals: manifest state, SSTable levels, WAL files, and VLog bytes.

### Experiment Lab

This tab uses an isolated directory: `flsm_experiment_lab`.

It is designed for correctness demonstrations. Every script is checked against an in-memory reference model, so the UI can show whether ForgeLSM returned the same result as the expected model.

Experiment Lab uses smaller thresholds than production:

- Memtable threshold: 32 KB.
- L0 limit: 2 files.
- L1 limit: 2 files.
- Dynamic compaction can create L3+ levels when explicitly forced or when deeper level limits are exceeded.

This makes flushes and compactions visible with only a few thousand operations.

Example script:

```text
insert 1..10000 random
update 2000..5000 random
get 1..1000 random
delete 3000..4000 random
flush
compact
compact_l1
recover
verify
state
```

Supported commands:

```text
put <key> <value>
update <key> <value>
get <key>
delete <key>

insert <start>..<end> random
insert <start>..<end> sequential
update <start>..<end> random
update <start>..<end> sequential
delete <start>..<end> random
delete <start>..<end> sequential
get <start>..<end> random
get <start>..<end> sequential

flush
compact
compact_l1
recover
restart
verify
state
```

The Experiment Lab result includes:

- PASS/FAIL status.
- Number of generated operations.
- Number of verification checks.
- Mismatches against the reference model.
- Final memtable/SSTable/VLog state.
- Raw JSON evidence for inspection.

## HTTP API

The web console is backed by a small JSON API implemented in `src/http_server.cpp`.

Read-only endpoints:

```text
GET /api/metrics
GET /api/lsm-state
GET /api/debug/state
GET /api/debug/files
GET /api/trace/status
GET /api/trace/read
GET /api/stream/status
```

Mutation and workload endpoints:

```text
POST /api/put
POST /api/get
POST /api/delete
POST /api/bench
POST /api/iot/bulk
POST /api/production/reset
POST /api/trace/toggle
POST /api/stream/start
POST /api/stream2/start
POST /api/stream/stop
POST /api/experiment/run
```

## External MQTT Broker Integration

ForgeLSM also includes optional Python tooling for a real MQTT broker flow:

```text
MQTT devices/publisher
   |
   v
External MQTT broker, for example Mosquitto on port 1883
   |
   v
tools/mqtt_bridge.py
   |
   v
ForgeLSM HTTP API on http://localhost:8080
   |
   v
KVStore, WAL, Memtable, SSTables, VLog
```

The bridge is intentionally outside the C++ engine. That keeps the storage
engine focused on database behavior while still demonstrating realistic IoT
ingestion through MQTT.

Install the MQTT Python dependency:

```powershell
python -m pip install -r requirements-mqtt.txt
```

Start ForgeLSM:

```powershell
.\flsm.exe web 8080
```

Start a local Mosquitto broker with Docker:

```powershell
docker run --rm -it -p 1883:1883 eclipse-mosquitto:2 mosquitto -c /mosquitto-no-auth.conf
```

Start the MQTT bridge:

```powershell
python tools\mqtt_bridge.py --broker localhost --engine http://localhost:8080 --verbose
```

Publish deterministic IoT traffic:

```powershell
python tools\mqtt_publisher.py --broker localhost --events 10000 --devices 100
```

The publisher emits a deterministic mix:

```text
80% telemetry puts      -> unique event keys
10% state updates       -> stable per-device state keys
5% deletes              -> tombstones for earlier telemetry keys
5% gets                 -> read checks through the bridge
```

The MQTT bridge maps messages like this:

```text
factory/<site>/device/<id>/telemetry   -> POST /api/put with a unique key
factory/<site>/device/<id>/state/...   -> POST /api/put with a stable update key
factory/<site>/device/<id>/delete      -> POST /api/delete
factory/<site>/device/<id>/query       -> POST /api/get
```

Docker Compose starts both ForgeLSM and a local Mosquitto broker:

```powershell
docker compose up -d
```

When Compose is used, run the Python bridge on the host with:

```powershell
python tools\mqtt_bridge.py --broker localhost --engine http://localhost:8080
```

Older fixed verification endpoints are still present for compatibility:

```text
POST /api/verify/basic
POST /api/verify/overwrite
POST /api/verify/delete
POST /api/verify/flush
POST /api/verify/bloom
POST /api/verify/compaction
POST /api/verify/recovery
POST /api/verify/gc
POST /api/verify/full
```

## CLI

ForgeLSM also has a small terminal interface:

```bash
./flsm cli
```

Typical commands:

```text
put <key> <value>
get <key>
del <key>
bench random_write
bench sequential_write
bench random_read
bench mixed
exit
```

The CLI is useful for direct engine interaction without the browser UI.

## Testing

Run the binary without arguments to execute the built-in test suite:

```bash
./flsm
```

On the current codebase, the suite reports:

```text
Results: 82 passed, 0 failed.
```

The tests cover:

- Basic put/get and overwrite semantics.
- WAL recovery and corrupt-tail handling.
- VLog round trips.
- Flush and recovery cycles.
- SSTable reads and multi-SSTable shadowing.
- Tombstone correctness.
- Dynamic L0/L1/L2/L3+ compaction behavior.
- Bloom filter correctness and checksum coverage.
- Benchmark isolation.
- Experiment Lab reference-model verification.
- L2 tombstone preservation regression.
- VLog GC rollback/finalize recovery.
- Stream 2 workload ratio and uniqueness math.

## Project Structure

```text
include/                 Public C++ headers
src/                     Engine, HTTP server, CLI, benchmark, and lab code
web/                     Browser UI: HTML, CSS, and JavaScript
assets/                  Architecture diagrams
docs/                    Design notes and project planning docs
tools/                   External MQTT bridge and publisher scripts
main.cpp                 Test runner and program entry point
Makefile                 Local build recipe
Dockerfile               Container build
docker-compose.yml       Local container runner
```

Key source files:

```text
src/kvstore.cpp          Main database orchestration
src/wal.cpp              Write-ahead log append/replay/truncation
src/vlog.cpp             Append-only value log
src/memtable.cpp         In-memory key index
src/sstable.cpp          SSTable serialization, checksums, Bloom section
src/manifest.cpp         Atomic manifest commits
src/compaction.cpp       L0/L1/L2 merge logic
src/vlog_gc.cpp          Value-log garbage collection support
src/http_server.cpp      Raw socket HTTP server and API routing
src/experiment_lab.cpp   Scripted verification lab with reference model
src/verification.cpp     Legacy fixed verification endpoints
src/benchmark.cpp        Benchmark workloads
src/cli.cpp              Terminal interface
```

## Runtime Data

ForgeLSM creates local data directories while running:

```text
flsm_production/         Persistent production IoT store
flsm_experiment_lab/     Isolated Experiment Lab store
flsm_verify_lab/         Isolated legacy verification store
test_flsm/               Temporary test suite store
```

Common files inside a store:

```text
wal_*.log                WAL segments
vlog.bin                 Append-only payload log
sst_*.sst                Sorted table files
MANIFEST                 Level/file metadata
STATS                    Persistent logical/physical write counters
```

## Docker

```bash
docker build -t forgelsm .
docker run -p 8080:8080 forgelsm
```

With a persistent production data volume:

```bash
docker run -p 8080:8080 -v forgelsm_data:/app/flsm_production forgelsm
```

Docker Compose:

```bash
docker compose up -d
```

## Current Limits

- VLog GC is crash-aware through `GC_STATE`, but still runs synchronously and uses a single active VLog file.
- External MQTT is implemented as Python bridge tooling; the C++ engine itself still exposes HTTP instead of embedding a native MQTT client.
- The HTTP server is intentionally minimal and hand-written for learning; it is not a general-purpose production web framework.

## Future Work

- Multi-file VLog generations with richer GC metadata.
- Native C++ MQTT client integration, if the project later needs broker ingestion inside the engine binary.
- README screenshots refreshed after the final UI settles.
