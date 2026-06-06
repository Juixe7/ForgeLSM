# Phase 1 — Engineering Design: WAL + Memtable + Crash Recovery

SSD-Optimized WiscKey-style LSM Engine

---

## 1. Project Structure

```
/flsm
  /include
    wal.h            — WAL class declaration + record format
    memtable.h       — Memtable class declaration
    kvstore.h        — KVStore engine declaration
    crc32.h          — CRC32 checksum utility
  /src
    wal.cpp          — WAL implementation (append, sync, replay)
    memtable.cpp     — Memtable implementation
    kvstore.cpp      — KVStore write path + recovery logic
    crc32.cpp        — CRC32 lookup table + computation
  /tests
    test_wal.cpp     — WAL-specific unit tests
    test_memtable.cpp — Memtable unit tests
    test_kvstore.cpp — Integration tests (write path + recovery)
  /docs
    design.md        — This document
    invariants.md    — System invariants
  main.cpp           — Entry point / test runner
  Makefile           — Build configuration
```

### Rationale

- **`include/` and `src/` separation**: Headers define the public API contract. Implementation lives separately. This enforces clear module boundaries, prevents circular dependencies, and allows each component to be compiled and tested independently.
- **`crc32` as its own module**: Checksum logic is a pure utility with no dependency on WAL, memtable, or engine state. Isolating it allows unit testing the checksum independently and swapping algorithms later (e.g., xxHash) without touching WAL internals.
- **`tests/` directory**: Each component gets its own test file scoped to its responsibilities. `test_kvstore.cpp` serves as the integration test covering the full write-path and recovery flow. This structure allows running targeted tests during development and full regression on CI.
- **Single `Makefile`**: No build system complexity. C++20, g++, direct compilation. Dependencies are explicit and minimal.
- **`docs/`**: Design documents live next to the code. `design.md` covers architecture. `invariants.md` captures safety guarantees that must never be violated.

---

## 2. WAL Design

### 2.1 Purpose

The WAL is the **sole source of durability**. Every write must be persisted to the WAL and fsynced before the system acknowledges it. The WAL is the ground truth from which all in-memory state is rebuilt after a crash.

### 2.2 Record Format (Binary Layout)

Each record is a contiguous byte sequence with **no padding and no alignment**:

```
Offset  Size (bytes)  Field
──────  ────────────  ──────────────────
0       4             key_size    (uint32_t, little-endian)
4       4             value_size  (uint32_t, little-endian)
8       4             checksum    (uint32_t, CRC32)
12      key_size      key_bytes   (raw bytes)
12+K    value_size    value_bytes (raw bytes)
```

**Total record size** = 12 + key_size + value_size bytes.

There are **no delimiters** between records. Each record is self-describing via its size fields.

### 2.3 Endianness Contract

All multi-byte integer fields (`key_size`, `value_size`, `checksum`) are encoded in **native little-endian** byte order. This is the byte order of x86/x64 and ARM (in default configuration) — the architectures this engine targets.

**Same-architecture assumption**: The WAL file is not portable across architectures with different endianness. A WAL written on a little-endian machine must be replayed on a little-endian machine. This is an explicit design tradeoff: avoiding byte-swapping overhead on the hot write path in exchange for not supporting cross-architecture WAL migration.

If cross-architecture portability becomes a requirement in a future phase, all integer fields would need explicit serialization to a fixed byte order (e.g., always little-endian via `htole32`/`le32toh`). This change is localized to the record serialization/deserialization functions and does not affect the rest of the design.

### 2.4 Checksum Coverage

The CRC32 checksum covers **four fields in order**:

1. `key_size` (4 bytes, raw binary)
2. `value_size` (4 bytes, raw binary)
3. `key_bytes` (key_size bytes)
4. `value_bytes` (value_size bytes)

The checksum itself is **not** included in the checksum computation. This is standard practice — you compute the checksum over the data, then store the result alongside it.

**Why include sizes in the checksum?** If only the payload were checksummed, a corrupted size field could cause the replay to misinterpret record boundaries, reading into the next record's data. Including sizes ensures that any corruption to the header is detected.

### 2.5 Size Limit Validation

During replay, `key_size` and `value_size` are validated against a **sanity upper bound** (64 MiB per field). If either size exceeds this bound, replay stops immediately.

**Purpose**: This limit is a **corruption guard**, not a product constraint. It exists to prevent a corrupt size field from causing the system to attempt a multi-gigabyte allocation or read, which would crash the process or exhaust memory. Under normal operation, no legitimate key or value approaches this limit.

**Future configurability**: The 64 MiB bound is a compile-time constant in Phase 1. In future phases, this can be promoted to a runtime-configurable parameter if the engine needs to support large values (e.g., in a WiscKey value-log scenario). The validation logic is isolated in the replay function, making this change trivial.

### 2.6 Append Behavior

- The WAL file is opened in **binary append mode** (`O_WRONLY | O_APPEND | O_CREAT` on POSIX).
- All writes go to the **end of the file**. No seeking, no in-place updates.
- A single `append(key, value)` call serializes one complete record (12-byte header + key + value) and writes it to the file.
- **Write completeness**: A single call to `write()` is **not guaranteed** to write all requested bytes. The operating system may perform a short write (returning fewer bytes than requested) due to signal interruption, resource pressure, or other transient conditions. Therefore, the append implementation **must loop** until all bytes of the record have been written, retrying on short writes. A record is considered appended only when all bytes have been accepted by the OS.
- The WAL class does **not** buffer internally beyond the OS write buffer. Durability is enforced by explicit `sync()`.

### 2.7 Sync Behavior

- `sync()` calls `fdatasync()` (POSIX) or `FlushFileBuffers()` (Windows) on the underlying file descriptor.
- This guarantees that all bytes written before the sync are flushed to **stable storage** (not just the OS page cache).
- `sync()` is called **after every `append()`** in the write path. This is the durability boundary — the point after which the data is guaranteed to survive a crash.

### 2.8 File Descriptor Lifetime

The WAL keeps its file descriptor **open for the entire lifetime** of the WAL object. The file is opened once during construction and closed during destruction.

**Reasoning**:

- **Performance**: Opening and closing a file descriptor on every `append()` would incur unnecessary system call overhead. The WAL is on the critical write path — every microsecond matters.
- **Ordering guarantee**: With a single persistent file descriptor, all `append()` calls are serialized through the same descriptor. Combined with `O_APPEND`, the OS guarantees that each write is atomically positioned at the end of the file. Re-opening the file on each call would lose this ordering guarantee in the presence of concurrent processes (not relevant in Phase 1, but a sound design principle).
- **Sync correctness**: `fdatasync()` operates on a file descriptor. The descriptor must refer to the same file that was written to. A persistent descriptor ensures this association is never broken.

### 2.9 Replay Algorithm

Replay reads the WAL **sequentially from byte 0** to reconstruct the ordered history of writes.

**Step-by-step:**

1. Open the WAL file in read-only binary mode.
2. Read 4 bytes → `key_size`.
3. Read 4 bytes → `value_size`.
4. Read 4 bytes → `stored_checksum`.
5. Validate sizes: both must be ≤ the sanity upper bound (64 MiB). If not → **STOP**.
6. Read `key_size` bytes → `key_bytes`.
7. Read `value_size` bytes → `value_bytes`.
8. Compute CRC32 over (`key_size`, `value_size`, `key_bytes`, `value_bytes`).
9. Compare computed CRC32 to `stored_checksum`. If mismatch → **STOP**.
10. Emit the `(key, value)` entry as valid.
11. Go to step 2 for the next record.
12. If any read returns fewer bytes than expected (EOF mid-record) → **STOP**.

**"STOP" means**: cease reading, return all entries collected so far. The system does **not** crash, does **not** throw, does **not** skip forward.

### 2.10 Corrupt Tail Policy

In Phase 1, the WAL is **never truncated**. If replay encounters corruption at the tail, it stops and returns all valid records before the corruption point. The corrupt bytes remain in the file.

**What happens on the next write?** After recovery, the WAL is opened in append mode. New records are appended **after** the corrupt tail. On the next replay, the same valid prefix is recovered; the corrupt region is hit again (replay stops); the newly appended records after the corruption are not reached.

**Tradeoff acknowledged**: This means that any record written after a corrupt tail in the same WAL file is effectively invisible to replay. This is a known limitation of Phase 1. The system prioritizes simplicity and safety — truncation requires careful coordination (e.g., ensuring we don't accidentally truncate valid data) and is deferred to a future phase where WAL segment rotation provides a cleaner solution.

**Phase 2+ resolution**: WAL segment rotation (starting a new WAL file) will eliminate this issue entirely. After recovery, the corrupt old segment is archived or discarded, and a fresh segment is started. No truncation is needed.

### 2.11 Key Questions Answered

**How do you detect partial writes?**
A partial write manifests as an incomplete record at the tail. During replay, a read of `key_size`, `value_size`, `checksum`, `key_bytes`, or `value_bytes` will return fewer bytes than requested. The read call will signal failure (EOF / short read), and replay stops immediately. Even if a partial write produced a complete header with garbage payload, the CRC32 mismatch will catch it.

**What happens on checksum mismatch?**
Replay stops. All records read before the mismatch are valid and returned. The mismatched record and everything after it is discarded. This is safe because the WAL is append-only: corruption at position N means nothing after N can be trusted.

**Where does replay stop?**
At the first point of failure: short read, invalid sizes, or checksum mismatch. Whichever comes first.

**Is replay idempotent? Why?**
Yes. Replay is a **read-only** operation on an **immutable file** (the WAL is not modified during replay). It produces the same sequence of entries every time it is called on the same file. The memtable rebuild applies entries in WAL order; duplicate keys are simply overwritten with the same value, producing the same final state.

---

## 3. Memtable Design

### 3.1 Data Structure Choice

**`std::map<std::string, std::string>`** (red-black tree).

### 3.2 Why Chosen

| Factor | `std::map` | Custom SkipList |
|--------|-----------|-----------------|
| Correctness guarantee | Standard library, well-tested | Must be hand-written and proven |
| Ordered iteration | Built-in (for future SSTable flush) | Must implement |
| Complexity (read/write) | O(log n) | O(log n) expected |
| Implementation risk | Zero | High (concurrency, memory management) |
| Phase 1 scope | Sufficient | Over-engineered |

For Phase 1, correctness and simplicity dominate. `std::map` delivers the same algorithmic complexity with zero implementation risk. A SkipList with arena allocation is appropriate for Phase 2+ when write throughput matters.

### 3.3 Complexity

| Operation | Time Complexity |
|-----------|----------------|
| `put(key, value)` | O(log n) — tree insertion/update |
| `get(key) → value` | O(log n) — tree lookup |
| Ordered scan (future) | O(n) — in-order traversal |

### 3.4 Memory Behavior

- Each entry stores a full copy of both key and value as `std::string`.
- Memory grows linearly with the number of unique keys.
- Overwriting a key replaces the value in-place (no duplicate key entries).
- **Unbounded growth acknowledgment**: In Phase 1 there is **no flush or eviction**. The memtable grows without limit for the lifetime of the process. This is a deliberate, temporary simplification — Phase 1 has no SSTable layer, so there is nowhere to flush to. The WAL is the only persistence mechanism.
- **Phase 2 resolution**: The SSTable flush mechanism introduced in Phase 2 will impose a size threshold on the memtable (e.g., 4 MiB or configurable). When the threshold is reached, the current memtable is frozen and flushed to an SSTable on disk, and a new empty memtable takes its place. This eliminates unbounded growth and transitions the system from a "WAL-only" store to a full LSM.
- Memory is freed entirely when the memtable is destroyed (e.g. on KVStore shutdown).

### 3.5 API

- **`put(key, value)`** — Insert or update. Returns nothing.
- **`get(key, out_value) → bool`** — Lookup. Returns true and populates `out_value` if found, false otherwise.
- **`size() → size_t`** — Number of unique keys. Used for testing and diagnostics.

---

## 4. KVStore Write Path

### 4.1 Step-by-Step: `put(key, value)`

The write path executes **three steps in strict order**. This ordering is non-negotiable.

| Step | Action | Purpose |
|------|--------|---------|
| 1 | `WAL.append(key, value)` | Serialize the record to the WAL file |
| 2 | `WAL.sync()` | Flush to stable storage — **durability boundary** |
| 3 | `Memtable.put(key, value)` | Update the in-memory index |

**Why this order?**

- Step 1 before Step 2: The record must be written before it can be synced.
- Step 2 before Step 3: The data must be durable **before** the system considers it committed. If the memtable is updated first and the system crashes before sync, the client may have observed the write (via a subsequent `get()`), but the data would be lost — violating durability.
- Step 3 after Step 2: Once we reach Step 3, the data is on disk. Even if we crash during Step 3, recovery will replay the WAL and rebuild the memtable, so the entry will be present.

### 4.2 `get(key)`

Reads are served **entirely from the memtable**. There is no disk I/O on the read path.

- Lookup in `std::map` → O(log n).
- If key exists, return the value. If not, return "not found."
- The memtable always reflects the full committed state (either from writes in this session or from WAL replay at startup).

### 4.3 Crash Scenario Analysis

#### Crash Point A: Before WAL write completes

**State on disk**: The record was never written (or only partially written — the OS may have buffered a partial write but never flushed it).

**On recovery**: WAL replay reads sequentially. If the record was never written, it simply does not exist. If partially written, replay will encounter a short read or checksum mismatch and stop. All records before this point are recovered.

**Data loss**: The in-flight write is lost. This is correct behavior — the client never received an acknowledgment (sync never completed).

#### Crash Point B: After WAL write, before fsync

**State on disk**: The record exists in the OS page cache but has **not** been flushed to stable storage. The physical disk may or may not have received the data.

**On recovery**: The record may or may not be in the WAL file. If the OS flushed the page to disk before the crash (opportunistically), the record will be replayed. If not, the record is lost. This is the same as Crash Point A from the client's perspective — sync never returned, so the write was never acknowledged.

**Data loss**: Possible loss of the unsynced record. This is correct — durability is guaranteed only **after** sync returns.

#### Crash Point C: After fsync, before memtable update

**State on disk**: The record is **durably stored** in the WAL. The memtable has not been updated.

**On recovery**: WAL replay will encounter this record and insert it into the memtable. The system state after recovery is identical to the state that would have existed had the crash not occurred.

**Data loss**: None. The record is recovered from the WAL. This is the key design property — fsync is the durability boundary, and everything after it is reconstructible.

---

## 5. Recovery Design

### 5.1 Recovery Flow

Recovery happens **exactly once** at KVStore construction time. The steps are:

1. **Open the WAL file** at the configured path.
2. **Call `WAL.replay()`** to read all valid records from the beginning of the file.
3. **For each replayed entry**, call `Memtable.put(key, value)` to insert it into the in-memory table.
4. **Open the WAL in append mode** for subsequent writes (new records are appended after the existing content, including any corrupt tail bytes — see Section 2.10).

### 5.2 How the Memtable Is Rebuilt

The memtable starts empty. Replayed entries are inserted in WAL order (oldest to newest). Because the memtable is a key-value map, later writes to the same key overwrite earlier ones. After replay, the memtable reflects the **last written value** for each key — exactly the state that existed before the crash.

### 5.3 Why Replay Is Deterministic

- The WAL file is a **fixed, immutable byte sequence** on disk (it is not modified during replay).
- The replay algorithm is purely sequential: start at byte 0, read forward, stop at first error.
- The CRC32 function is a deterministic pure function.
- There is no randomness, no time-dependency, no external input.
- Therefore, replaying the same WAL file will **always** produce the same sequence of entries.

### 5.4 Why Replay Is Idempotent

- `WAL.replay()` is **read-only**. It does not modify the WAL file.
- `Memtable.put(key, value)` is a pure insert-or-overwrite. Inserting `(k, v)` when `k` already maps to `v` is a no-op in effect.
- Calling recovery multiple times on the same WAL produces the same memtable state every time.
- There are no counters, sequence numbers, or side effects that accumulate across replays.

---

## 6. Failure Matrix

| # | Failure Point | Expected Behavior | Why Safe |
|---|--------------|-------------------|----------|
| 1 | **Partial WAL record** (truncated mid-write) | Replay reads header or payload, hits EOF before expected bytes. Stops immediately. Returns all valid records before the truncation. | The short read is detected by the I/O layer. No partial data is emitted. Records before the truncation were fully written and checksummed. |
| 2 | **Corrupted checksum** (bit-flip or garbage in checksum field) | Replay reads the full record, computes CRC32, finds mismatch. Stops immediately. Returns all valid records before the corrupted one. | CRC32 detects single and multi-bit errors with high probability. The corrupt record and all subsequent records are discarded — safe because we cannot trust record boundaries after corruption. |
| 3 | **Corrupted payload** (correct header, corrupt key/value bytes) | CRC32 mismatch detected on the payload. Replay stops. | Checksum covers both header sizes and payload bytes. Any alteration is caught. |
| 4 | **Corrupted size fields** (key_size or value_size overwritten) | Two outcomes: (a) sizes are absurdly large → sanity check fails → replay stops. (b) sizes are plausible but wrong → wrong number of bytes read → CRC32 mismatch → replay stops. | The sanity bound (64 MiB) catches extreme corruption. For subtle corruption, the CRC32 (which covers sizes) provides the safety net. |
| 5 | **Empty WAL file** (fresh start or file was truncated to zero) | Replay opens file, first read returns EOF immediately. Returns empty entry list. Memtable is empty. | Valid state — the system simply has no committed data. |
| 6 | **WAL file does not exist** | WAL constructor creates the file. Replay opens it, finds it empty, returns empty list. | File creation on first open is standard. Non-existence is treated as "empty." |
| 7 | **Repeated replay** (replay called multiple times on same WAL) | Each call returns the same sequence of entries. Memtable ends in the same state. | Replay is read-only and deterministic. Memtable put is idempotent for same key-value pairs. |
| 8 | **Crash during recovery itself** (crash while replaying) | On next startup, recovery starts again from byte 0. Produces the same result. | Recovery does not modify the WAL. The WAL file is unchanged between crashes, so replay is repeatable. |
| 9 | **Valid data followed by garbage bytes** (e.g., appended junk after clean records) | Replay reads valid records, then hits the garbage. Garbage fails as a valid header (bad sizes or bad checksum). Replay stops. Valid records are preserved. | The stop-at-first-error strategy ensures garbage at the tail never contaminates valid data. |
| 10 | **Write after corrupt tail** (new records appended past corruption) | New records are invisible to replay — replay stops at the corrupt region. Data written after a corrupt tail is lost to replay. | This is a known Phase 1 limitation (see Section 2.10). Phase 2 WAL segment rotation eliminates this. Safety is preserved — no invalid data is ever returned. |
| 11 | **Short write during append** (OS returns fewer bytes than requested) | The append loop retries until all bytes are written. No partial record reaches the file without full intent. | The full-write loop (Section 2.6) ensures atomicity of the append at the application level. |

---

## 7. Test Plan

### Test 1: Basic Put / Get

**What it validates**: The fundamental write-read contract. Data written via `put()` is immediately readable via `get()`.

- Insert 4-5 key-value pairs with distinct keys and values.
- For each key, call `get()` and verify the returned value matches what was written.
- Call `get()` on a key that was never written — verify it returns false / not-found.
- Validates: memtable insertion, map lookup, and the basic API contract.

### Test 2: Restart Recovery

**What it validates**: That WAL replay correctly rebuilds the memtable after a simulated crash (destroy + reconstruct).

- Create a KVStore, insert several key-value pairs, then destroy the KVStore object (simulating a shutdown/crash).
- Create a new KVStore pointing at the same WAL file.
- Verify that all previously written key-value pairs are present and correct.
- Verify the memtable size matches the number of unique keys written.
- Validates: WAL append correctness, replay correctness, memtable rebuild.

### Test 3: Corrupted WAL Tail (Partial Write)

**What it validates**: That replay gracefully handles a truncated record at the end of the WAL without crashing or corrupting recovered data.

- Create a KVStore, insert several valid key-value pairs, destroy it.
- Open the WAL file in append mode and write a partial record (e.g., only the key_size field, or a few random bytes).
- Create a new KVStore from the same WAL.
- Verify: no crash occurs, all valid records before the corruption are recovered, the corrupt/partial record is silently discarded.
- Validates: the stop-at-first-error replay strategy, partial write detection.

### Test 4: Checksum Mismatch

**What it validates**: That a record with a valid structure but incorrect checksum is rejected.

- Create a clean WAL file manually (no KVStore).
- Write a single record with correct key_size, value_size, key, and value, but an **intentionally wrong** checksum.
- Create a KVStore from this WAL.
- Verify: memtable is empty, no crash, no entries recovered.
- Validates: CRC32 validation in the replay path.

### Test 5: Overwrite Semantics

**What it validates**: That writing to the same key multiple times results in the last value winning, both in the live memtable and after recovery.

- Create a KVStore and write the same key three times with different values (v1, v2, v3).
- Verify `get()` returns v3.
- Destroy and recreate the KVStore from the same WAL.
- Verify `get()` still returns v3 after recovery (the WAL contains all three writes, but replay applies them in order so the last write wins).
- Validates: memtable overwrite behavior, WAL replay ordering, last-write-wins semantics.

### Test 6: Empty WAL

**What it validates**: That the system starts cleanly with no prior state.

- Ensure no WAL file exists (or delete it).
- Create a KVStore.
- Verify memtable is empty, `get()` on any key returns false.
- Validates: graceful handling of non-existent or empty WAL.

### How to Run Tests

All tests compiled and executed from the project root:

```
g++ -std=c++20 -Iinclude -o flsm src/crc32.cpp src/wal.cpp src/memtable.cpp src/kvstore.cpp main.cpp
./flsm
```

Each test prints `[PASS]` or `[FAIL]` per assertion. The runner returns exit code 0 if all pass, non-zero otherwise.

---

## 8. Invariants

These are the safety guarantees that must hold at **all times**. Every design decision, every line of future code, must preserve these. Violations are bugs.

### Invariant 1: Post-Sync Durability

> **After `WAL.sync()` returns, the appended record is recoverable across any subsequent crash.**

This is the foundational guarantee. If sync returns, the data is on stable storage. Recovery will find it.

### Invariant 2: Replay Completeness

> **WAL replay recovers every record that was successfully synced, in the order it was written.**

No valid, synced record is ever skipped or reordered during replay. The replay output is a prefix of the full write history.

### Invariant 3: Replay Safety (No Invalid State)

> **WAL replay never produces invalid, corrupted, or fabricated entries.**

Every entry returned by replay has passed CRC32 validation. If a record is corrupt, it is discarded — it is never emitted as a valid entry.

### Invariant 4: Fail-Stop on Corruption

> **Replay stops at the first invalid record and does not attempt to read beyond it.**

The WAL is a sequential log. Once a record is invalid, all subsequent byte offsets are untrustworthy (because record boundaries depend on the sizes in previous records). The system does not skip forward or scan for a "next valid record."

### Invariant 5: Replay Idempotency

> **Replaying the same WAL file any number of times produces the same memtable state.**

This guarantees that recovery is safe to retry. A crash during recovery itself does not corrupt state — the next recovery attempt will produce the same result.

### Invariant 6: Write Ordering

> **The write path always executes: WAL append → sync → memtable update. This order is never reordered.**

This ensures the memtable never contains data that is not durable. A crash at any point leaves the system in a consistent state (see crash scenario analysis in Section 4.3).

### Invariant 7: Memtable-WAL Consistency

> **After recovery, the memtable is a faithful representation of the WAL's valid content.**

The memtable state equals the result of applying all valid WAL records in order. There is no state in the memtable that does not originate from the WAL, and no valid WAL record that is missing from the memtable.

### Invariant 8: Last-Write-Wins

> **For any key written multiple times, `get()` always returns the value from the most recent `put()` — both during a session and after recovery.**

The memtable uses insert-or-overwrite semantics. WAL replay applies records in temporal order. Both paths converge to the same result.

### Invariant 9: Append Atomicity

> **A record is either fully written to the WAL or not written at all. No partial record is left by the append implementation under normal operation.**

The write loop in `append()` retries short writes until the full record is flushed. Partial records can only result from a crash during the write — which is handled by the replay corruption detection.

### Invariant 10: WAL Immutability During Replay

> **The WAL file is never modified during replay. Replay is a pure read-only operation.**

This ensures that replay is safe to retry after a crash during recovery, and that the WAL remains a trustworthy source of truth.
