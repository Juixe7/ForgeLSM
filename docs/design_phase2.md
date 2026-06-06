# Phase 2 — Engineering Design: Value Log + Memtable Flush + L0 SSTable

SSD-Optimized WiscKey-style LSM Engine

> **Extends Phase 1**. All Phase 1 invariants (I1–I10) remain unchanged and enforced.
> No existing component behavior is modified. Phase 2 adds new components and extends the write/read paths.

---

## 1. Value Log Design

### 1.1 Purpose

The Value Log (vlog) implements **WiscKey key-value separation**. Large values are stored in an append-only log on disk, and only small **pointers** to those values are stored in the memtable, SSTables, and WAL recovery path. This dramatically reduces write amplification during compaction (future Phase 3+) because compaction only rewrites small key-pointer pairs, not full values.

### 1.2 Record Format

Each vlog record is a contiguous byte sequence:

```
Offset  Size (bytes)  Field
──────  ────────────  ──────────────────
0       4             value_size  (uint32_t, little-endian)
4       value_size    value_bytes (raw bytes)
```

**Total record size** = 4 + value_size bytes.

There is no checksum in the vlog record. Integrity is guaranteed by the WAL checksum (which covers the full key + value). The vlog is a secondary storage optimization, not a source of truth.

### 1.3 Pointer Structure

A value log pointer uniquely identifies a value on disk:

```
struct VLogPointer {
    uint32_t file_id;   // vlog file identifier (for future rotation)
    uint64_t offset;    // exact byte offset of the record start in the vlog file
    uint32_t length;    // value_size (bytes of value, excluding the 4-byte header)
};
```

- `file_id`: In Phase 2, there is a single vlog file, so this is always 0. The field exists for forward compatibility with vlog garbage collection and rotation in future phases.
- `offset`: The byte position of the record start in the vlog file, tracked by a user-space variable (see §1.4). This is the exact start of the `value_size` field.
- `length`: The number of value bytes (not including the 4-byte size header). To read the value, seek to `offset + 4` and read `length` bytes. Alternatively, seek to `offset`, read `value_size`, and use it for validation.

### 1.4 Append Behavior

- The vlog file is opened in **binary append mode** with a persistent file descriptor (same lifetime strategy as WAL — see Phase 1 §2.8).
- `append(value)` serializes `[value_size][value_bytes]`, writes via the `write_all` loop (EINTR-safe, short-write-safe), and returns the `VLogPointer` indicating where the value was stored.
- `sync()` calls `fdatasync` / `_commit` on the vlog fd, same as WAL sync.

#### Offset Tracking (Critical Correctness Detail)

> [!CAUTION]
> The vlog offset is tracked via an **internal `current_offset_` variable** maintained in user space. It is **NOT** derived from `lseek(fd, SEEK_CUR)` or any file descriptor query.

Using `lseek` with `O_APPEND` is **unsafe**: `O_APPEND` atomically seeks to EOF on each `write()` call, but `lseek(fd, 0, SEEK_CUR)` does not reflect this atomic position — it returns the descriptor's logical cursor, which may not match the actual write offset under concurrent access or OS buffering edge cases.

**Correct approach:**

- On VLog construction (or recovery), initialize `current_offset_` to the file size (obtained via `fstat` or `lseek(fd, 0, SEEK_END)` **once**, before any writes).
- On each `append()`, capture `current_offset_` as the record's offset **before** calling `write_all`.
- After `write_all` succeeds, advance `current_offset_` by the number of bytes written (4 + value_size).
- If `write_all` fails, `current_offset_` is **not** advanced — the pointer is never created.

This guarantees that every `VLogPointer.offset` is exact and deterministic.

### 1.5 WAL Interaction (Critical)

> [!IMPORTANT]
> The WAL **still stores the FULL key and FULL value**. This is non-negotiable.

The Value Log is a **read-path optimization**, not a durability mechanism. The WAL remains the sole source of truth for crash recovery.

**Why?**
- Recovery replays the WAL to rebuild the memtable. If the WAL only stored pointers, recovery would depend on the vlog being intact — adding a second failure point.
- By keeping full values in the WAL, recovery is self-contained: WAL replay produces the complete state without reading the vlog.
- The vlog is populated as a side effect of the write path but is **never read during recovery**.

**Write path interaction** (updated from Phase 1):

| Step | Action | On Failure |
|------|--------|------------|
| 1 | `WAL.append(key, value)` | → throw, STOP |
| 2 | `WAL.sync()` | → throw, STOP |
| 3 | `VLog.append(value)` → returns pointer | → throw, STOP (WAL has data; recovered on restart) |
| 4 | `VLog.sync()` | → throw, STOP — pointer is **not** inserted (I12) |
| 5 | `Memtable.put(key, vlog_pointer)` | Only reached if steps 1–4 all succeed |

> [!IMPORTANT]
> **Step 5 MUST NOT execute if step 3 or step 4 fails.** A pointer must never reference a non-durable value. If `VLog.sync()` fails, the value may not be on stable storage, so the pointer would be invalid. KVStore must enforce this gate identically to the WAL sync gate in Phase 1.

Steps 3–4 happen **after** the WAL sync. If the system crashes between steps 2 and 3, the WAL has the full value — recovery replays the WAL, re-appends values to the vlog, and reconstructs pointers. No data is lost.

### 1.6 Crash Safety Analysis

**Crash before vlog append (after WAL sync):**
- WAL has the full key + value. Vlog does not.
- On recovery: WAL replay produces (key, value). Recovery re-appends the value to vlog, obtains a new pointer, and inserts (key, pointer) into the memtable.
- **Safe**: WAL is the source of truth.

**Crash after vlog append, before vlog sync:**
- The value may or may not be on disk (in OS page cache).
- WAL has the full key + value. Recovery re-appends to vlog regardless.
- The old (possibly partial) vlog bytes become orphaned — harmless, cleaned up by future GC.
- **Safe**: WAL is the source of truth.

**Crash after vlog sync, before memtable update:**
- Value is durable in both WAL and vlog. Memtable not yet updated.
- Recovery replays WAL, re-appends to vlog, inserts pointer.
- **Safe**: identical to the non-crash outcome.

### 1.7 VLog Replay Contract (Recovery)

> [!IMPORTANT]
> Recovery **always creates a fresh vlog file**. The old vlog content is discarded entirely.

During recovery:

1. If an existing vlog file is present, **delete it** (or rename it for forensics). Its contents are untrusted — they may be incomplete, from a partial write, or from a previous recovery.
2. Create a **new, empty vlog file**.
3. Replay the WAL sequentially. For each replayed `(key, value)` entry:
   - Append `value` to the new vlog → obtain `VLogPointer`.
   - Insert `(key, VLogPointer)` into the memtable.
4. After replay, the new vlog is a faithful, sequential reconstruction of all values from the WAL.

**Why discard the old vlog?**
- The old vlog may contain orphaned bytes from partial writes, duplicate values from previous recoveries, or be entirely absent.
- Reconstructing from WAL is deterministic and complete — the WAL has every value.
- This eliminates any need to validate, parse, or trust the old vlog.

**Cost**: Recovery re-writes all values. For large datasets this adds recovery time. This is the correct tradeoff for Phase 2 — correctness over speed. Phase 3+ can optimize by validating existing vlog content.

---

## 2. Memtable Modification

### 2.1 Change

**Phase 1 (old):**
```
std::map<std::string, std::string>    key → full value
```

**Phase 2 (new):**
```
std::map<std::string, VLogPointer>    key → vlog pointer
```

The memtable no longer stores values inline. It stores only keys and pointers. This reduces memtable memory consumption and keeps the in-memory footprint proportional to key count, not total data size.

### 2.2 Memory Layout

Each memtable entry now occupies:

- Key: `std::string` (~32 bytes overhead + key length)
- Value: `VLogPointer` = `uint32_t + uint64_t + uint32_t` = **16 bytes fixed**

Compare to Phase 1 where each value was a full `std::string` (32 bytes overhead + value length). For values larger than ~48 bytes, Phase 2 uses less memory per entry.

### 2.3 API Changes

- `put(key, vlog_pointer)` — stores pointer, not value.
- `get(key, out_pointer) → bool` — returns `VLogPointer`, not value. The caller (KVStore) resolves the pointer by reading from the vlog.

### 2.4 Why Value Is No Longer Stored

1. **Write amplification**: When the memtable flushes to an SSTable, only keys and small pointers are written. The full values (which can be arbitrarily large) are not copied — they already exist in the vlog.
2. **Memory efficiency**: The memtable's memory is bounded by key count × 16 bytes (plus key sizes), not by total value size.
3. **WiscKey design**: This is the core WiscKey insight — separate keys from values to exploit SSD random-read performance during point lookups while keeping writes sequential.

---

## 3. Flush Mechanism

### 3.1 Trigger

The flush is triggered when the **active memtable's byte count** exceeds a configurable threshold.

- **Default threshold**: 4 MiB
- **Byte count calculation**: Sum of (key.size() + sizeof(VLogPointer)) for all entries. This is an approximation — it does not account for `std::map` node overhead. Exact accounting is unnecessary; the threshold is a policy hint, not a hard limit.

### 3.2 Flow

1. **Freeze the active memtable**: The current memtable becomes the "immutable memtable." It accepts no further writes but remains available for reads.
2. **Create a new active memtable**: A fresh, empty memtable takes over as the write target. All subsequent `put()` calls go to this new memtable.
3. **Flush the immutable memtable to an L0 SSTable**: Iterate the immutable memtable in sorted key order, write each (key, vlog_pointer) entry to a new SSTable file on disk.
4. **Sync the SSTable file**: `fdatasync` the SSTable to ensure durability.
5. **Discard the immutable memtable**: Free its memory. The data now lives in the SSTable.

### 3.3 Why Immutable Is Required

If we flushed the active memtable directly, new writes arriving during the flush would interleave with the flush iteration, producing an inconsistent SSTable. By freezing the memtable:

- The flush iterates a **stable snapshot** — no concurrent modifications.
- New writes go to the fresh memtable — no blocking.
- The read path checks both memtables (active first, then immutable), so no data is invisible during the flush.

### 3.4 Synchronous Flush (Phase 2 Constraint)

In Phase 2, the flush is **synchronous**. When the threshold is exceeded during a `put()`, the flush happens inline before the `put()` completes. This means:

- The `put()` that triggers the flush will have higher latency.
- No background thread is needed — simplicity over performance.
- Phase 3+ may introduce asynchronous flush with a background thread.

### 3.5 WAL Rotation After Flush

After a successful flush (SSTable is durable on disk), the WAL entries corresponding to the flushed memtable are no longer needed for recovery — their data is now in the SSTable.

**Phase 2 approach**: Start a **new WAL file** (WAL segment rotation). The old WAL file is deleted **only after** the new WAL is created and fsynced. This is simpler and safer than truncating the existing WAL.

**Sequence (crash-safe ordering):**

1. Flush immutable memtable → SSTable → `fdatasync` SSTable
2. Create **new WAL file** (e.g., `wal_002.bin`) → `fdatasync` new WAL
3. Atomically switch the active WAL fd to the new file
4. **Then** delete old WAL file (e.g., `wal_001.bin`)
5. Continue writes to new WAL

> [!CAUTION]
> The old WAL is deleted **last**, after the new WAL is confirmed durable. If deletion happened before new WAL creation, a crash in that window would leave the system with no WAL and no SSTable for the active memtable's data — **data loss**.

**Crash analysis for each step:**

| Crash Point | State | Recovery |
|-------------|-------|----------|
| After step 1, before step 2 | SSTable durable, old WAL exists, no new WAL | WAL replay + SSTable load. Duplicates resolved by I8. Safe. |
| After step 2, before step 4 | SSTable durable, old WAL exists, new WAL exists (empty) | Both WALs replayed (old has data, new is empty). SSTable loaded. Duplicates resolved. Safe. |
| After step 4 | SSTable durable, new WAL exists, old WAL gone | SSTable loaded. New WAL replayed (may have new entries). Clean state. |

At every crash point, at least one durable representation (WAL or SSTable) exists for every committed entry. This satisfies I15 and I19.

---

## 4. SSTable Design (L0 Only)

### 4.1 Purpose

An SSTable (Sorted String Table) is an **immutable file** containing key-pointer pairs sorted by key. It is the on-disk persistence format for flushed memtables.

### 4.2 File Layout

```
┌──────────────────────────────────────────────┐
│ Data Section                                 │
│   Entry 0: [key_size][key][file_id][off][len]│
│   Entry 1: [key_size][key][file_id][off][len]│
│   ...                                        │
│   Entry N: [key_size][key][file_id][off][len]│
├──────────────────────────────────────────────┤
│ Footer                                       │
│   [uint32_t entry_count]                     │
│   [uint32_t checksum]  — CRC32 over data     │
└──────────────────────────────────────────────┘
```

**Entry format:**

```
Offset  Size (bytes)  Field
──────  ────────────  ──────────────────
0       4             key_size    (uint32_t)
4       key_size      key_bytes   (raw bytes)
4+K     4             file_id     (uint32_t)
8+K     8             offset      (uint64_t)
16+K    4             length      (uint32_t)
```

**Footer** (last 8 bytes of file):
- `entry_count`: number of entries in the data section.
- `checksum`: CRC32 over the entire data section (all entry bytes).

### 4.3 Properties

- **Sorted by key**: Entries are written in lexicographic key order (inherited from `std::map` iteration order).
- **Immutable**: Once written and fsynced, the SSTable is never modified.
- **Binary search**: To find a key, load the SSTable into memory and binary search the sorted entries.
- **Full in-memory loading (Phase 2)**: The entire SSTable is loaded into memory for lookups. This is a deliberate Phase 2 simplification, acceptable because:
  - L0 SSTables contain only key-pointer pairs, not full values — each entry is ~(key_size + 16) bytes.
  - The flush threshold is 4 MiB, so each SSTable is ≤ 4 MiB.
  - Without compaction, the number of L0 files is small (bounded by total data volume / 4 MiB).
  - **Future phases** will introduce block-based indexing or memory-mapped I/O to avoid loading full SSTables for large datasets.

### 4.4 File Naming and Ordering

SSTable files are named with a **monotonically increasing sequence number**:

```
sst_000001.sst
sst_000002.sst
sst_000003.sst
```

The sequence number is the **sole ordering mechanism**. SSTables are always searched in descending sequence number order (newest first). This guarantees that for duplicate keys across SSTables, the most recent value is found first.

The next sequence number is derived at startup by scanning the data directory for `sst_*.sst` files and taking `max(existing sequence numbers) + 1`. If no SSTables exist, the sequence starts at 1.

### 4.5 Loading at Startup

On KVStore construction:
1. Scan the data directory for `sst_*.sst` files.
2. **Validate each SSTable** before loading:
   - Read the 8-byte footer (last 8 bytes of file).
   - Extract `entry_count` and `checksum`.
   - Compute CRC32 over the data section (all bytes before the footer).
   - If the footer is missing, `entry_count` is zero, or the checksum mismatches → **discard the file entirely** (log a warning, delete or rename it). This catches partial flushes that crashed before completion.
3. Sort valid SSTables by sequence number (ascending = oldest first).
4. Load each valid SSTable into an in-memory sorted structure.
5. During reads, search SSTables in **reverse** order (newest first) — newer SSTables shadow older ones for the same key.

---

## 5. Updated Read Path

### 5.1 Lookup Order (Strict)

```
Active Memtable → Immutable Memtable → L0 SSTables (newest → oldest) → NOT FOUND
```

Each level returns a `VLogPointer` if the key is found. The first match wins (because newer data shadows older data).

### 5.2 Step-by-Step: `get(key)`

1. **Check active memtable**: O(log n) lookup. If found → have `VLogPointer`, go to step 5.
2. **Check immutable memtable** (if one exists during flush): O(log n) lookup. If found → have `VLogPointer`, go to step 5.
3. **Check L0 SSTables** in reverse chronological order (newest first): Binary search each SSTable. If found → have `VLogPointer`, go to step 5.
4. **Key not found**: Return false / not-found. No disk I/O occurs beyond SSTable searches.
5. **Resolve VLogPointer**: Open (or use cached fd for) the vlog file. Seek to `pointer.offset + 4` (skip the `value_size` header). Read `pointer.length` bytes. Return the value.

### 5.3 Performance Characteristics

- **Memtable hit**: O(log n) — no disk I/O.
- **SSTable hit**: O(S × log E) where S = number of L0 SSTables, E = entries per SSTable. Involves reading from disk (or page cache).
- **VLog read**: One random read at the exact offset — O(1) seek + sequential read. This is the WiscKey advantage: SSDs handle random reads efficiently.

---

## 6. Failure Cases

### 6.1 Crash Before WAL Append

| Aspect | State |
|--------|-------|
| On disk | Nothing written |
| Recovery | No trace of this write. System state is consistent |
| Why safe | Write was never acknowledged. Client observes no change |

### 6.2 Crash After WAL Sync, Before VLog Append

| Aspect | State |
|--------|-------|
| On disk | WAL has full (key, value). VLog does not have the value |
| Recovery | WAL replay produces (key, value). Recovery re-appends value to vlog, obtains new pointer, inserts into memtable |
| Why safe | WAL is the sole source of truth for recovery. VLog is rebuilt as a side effect |

### 6.3 Crash After VLog Append, Before Memtable Update

| Aspect | State |
|--------|-------|
| On disk | WAL has full (key, value). VLog has the value (if sync completed) |
| Recovery | Same as 6.2 — WAL replay rebuilds everything. Old vlog bytes become orphaned (harmless) |
| Why safe | WAL replay is self-contained. Orphaned vlog bytes do not affect correctness |

### 6.4 Crash During Flush

| Aspect | State |
|--------|-------|
| On disk | Partially written SSTable file (may be corrupt/incomplete). WAL still exists (not yet rotated). Immutable memtable was in memory (lost) |
| Recovery | WAL replay rebuilds the memtable with all entries. The partial SSTable is detected (missing/corrupt footer, checksum mismatch) and discarded. Flush will re-trigger when the memtable grows past the threshold again |
| Why safe | The WAL is not deleted until the SSTable is confirmed durable. A crash during flush means the WAL survives, and recovery is equivalent to a Phase 1 recovery. The partial SSTable is simply ignored |

### 6.5 Crash After Flush, Before WAL Deletion

| Aspect | State |
|--------|-------|
| On disk | SSTable is durable. WAL still exists |
| Recovery | WAL replay rebuilds memtable entries. SSTable loading also provides those entries. Duplicates are resolved by Last-Write-Wins (I8) — same key in both produces the same final value |
| Why safe | Recovery is idempotent. Duplicates are harmless. WAL deletion is retried on next clean startup |

---

## 7. New Invariants (Phase 2)

> Phase 1 invariants I1–I10 remain unchanged and fully enforced.

### I11: WAL Remains Source of Truth for Recovery

> **Recovery uses the WAL exclusively. The Value Log is never read during recovery.**

The vlog is a read-path optimization. If the vlog is lost or corrupt, recovery from the WAL can reconstruct it entirely.

### I12: Pointer Correctness

> **Every `VLogPointer` in the memtable or an SSTable points to a valid, durable value in the vlog. The value at `(file_id, offset, length)` matches the value originally written.**

A pointer is only inserted into the memtable after the vlog append and sync succeed. The vlog is append-only and immutable — values are never overwritten.

### I13: SSTable Immutability

> **Once an SSTable file is fsynced and confirmed durable, it is never modified.**

SSTables are write-once, read-many. Any mutation would violate sorted order and pointer correctness.

### I14: Flush Atomicity

> **A flush either produces a complete, valid, durable SSTable, or it produces nothing. Partial SSTables are discarded on recovery.**

The SSTable footer (with entry count and checksum) is the commit marker. If the footer is missing or the checksum mismatches, the file is invalid and ignored.

### I15: No Data Loss Across Flush

> **Data is never lost during a flush. The WAL is not deleted until the SSTable is confirmed durable on disk (fsynced).**

This ensures that at least one durable representation (WAL or SSTable) exists at all times.

### I16: Read Consistency

> **For any key, `get()` returns the most recently `put()` value, regardless of whether that value is in the active memtable, immutable memtable, or an SSTable.**

The lookup order (active → immutable → SSTables newest-first) guarantees that the most recent write is always found first.

### I17: VLog Append-Only

> **The Value Log is append-only. No in-place updates, no deletions. Values are never overwritten.**

This guarantees that existing pointers remain valid forever (until garbage collection in a future phase).

### I18: VLog Reconstruction Determinism

> **Recovery always produces an identical vlog state from the same WAL content. Replaying the same WAL twice creates the same vlog with the same offsets and pointers.**

Values are appended to the fresh vlog in WAL replay order. Since WAL replay is deterministic (I5) and vlog append is sequential with user-space offset tracking, the resulting vlog is byte-for-byte identical across recoveries.

### I19: Flush-WAL Safety Boundary

> **At least one durable representation (WAL or SSTable) exists for every committed entry at all times. There is no window where a committed entry has no durable backing.**

The old WAL is deleted only after the new WAL is created and the SSTable is confirmed durable. This ordering (§3.5) ensures that a crash at any point leaves at least one complete copy of every committed entry on stable storage.

---

## 8. File Structure Update

```
/flsm
  /include
    crc32.h          — CRC32 checksum utility          [UNCHANGED]
    wal.h            — WAL class                        [UNCHANGED]
    memtable.h       — Memtable (updated: key → VLogPointer)
    kvstore.h        — KVStore engine (updated: vlog + flush + SST)
    vlog.h           — Value Log class                  [NEW]
    sstable.h        — SSTable writer + reader          [NEW]
  /src
    crc32.cpp         [UNCHANGED]
    wal.cpp           [UNCHANGED]
    memtable.cpp      (updated: stores VLogPointer)
    kvstore.cpp       (updated: write path + read path + flush)
    vlog.cpp          [NEW] — append, sync, read-at
    sstable.cpp       [NEW] — write sorted entries, load + binary search
  /tests
    test_vlog.cpp     [NEW] — vlog append + read roundtrip
    test_sstable.cpp  [NEW] — SSTable write + load + lookup
    test_flush.cpp    [NEW] — flush correctness
    test_recovery2.cpp[NEW] — recovery with vlog present
  /docs
    design.md         (Phase 1 design)                 [UNCHANGED]
    design_phase2.md  (this document)                  [NEW]
    invariants.md     (updated: I11–I17 appended)
  main.cpp            (updated: Phase 2 tests added)
  Makefile            (updated: new source files)
```

### New Components

| File | Responsibility |
|------|---------------|
| `vlog.h` / `vlog.cpp` | Append-only value log. `append(value) → VLogPointer`, `sync()`, `read_at(pointer) → value` |
| `sstable.h` / `sstable.cpp` | SSTable writer (from sorted iteration) + SSTable reader (load from file, binary search by key) |

### Modified Components

| File | What Changes |
|------|-------------|
| `memtable.h` / `memtable.cpp` | Map value type changes from `std::string` to `VLogPointer` |
| `kvstore.h` / `kvstore.cpp` | Write path adds vlog append. Read path adds SSTable + vlog resolution. Flush logic added. WAL rotation after flush |
| `main.cpp` | New test cases for Phase 2 |
| `Makefile` | New source files added to SRCS |
| `invariants.md` | I11–I17 appended |

---

## 9. Test Plan (Phase 2)

### Test 7: VLog Append + Read Roundtrip

**What it validates**: Values written to the vlog can be read back correctly via their pointers.

- Append several values to the vlog.
- For each returned `VLogPointer`, call `read_at(pointer)` and verify the value matches the original.
- Validates: vlog record format, offset tracking, pointer correctness (I12).

### Test 8: Flush Correctness

**What it validates**: Memtable flush produces a valid, sorted SSTable with correct pointers.

- Insert enough entries to exceed the 4 MiB flush threshold.
- Verify that an SSTable file was created on disk.
- Load the SSTable and verify: entry count matches, keys are sorted, each pointer resolves to the correct value via the vlog.
- Validates: flush flow, SSTable format, I14 (Flush Atomicity), I15 (No Data Loss).

### Test 9: Read From SSTable

**What it validates**: After flush, reads correctly resolve through SSTables and the vlog.

- Insert entries, trigger a flush, then query keys that are now in the SSTable (not the active memtable).
- Verify `get()` returns the correct value.
- Validates: full read path (memtable → SSTable → vlog), I16 (Read Consistency).

### Test 10: Recovery With VLog

**What it validates**: After a crash, WAL replay correctly re-populates the memtable and vlog, even if the vlog was partially written.

- Insert entries, destroy KVStore.
- Delete the vlog file (simulate vlog loss).
- Recreate KVStore from the same WAL.
- Verify all values are recovered correctly (WAL has full values, vlog is reconstructed).
- Validates: I11 (WAL remains source of truth), recovery does not depend on vlog.

### Test 11: Pointer Survives Flush + Recovery

**What it validates**: End-to-end: write → flush → crash → recover → read.

- Insert entries, trigger flush, destroy KVStore.
- Recreate KVStore.
- Verify all entries are readable (from SSTable + vlog).
- Validates: full cycle correctness.

### Test 12: Read After Multiple Flushes

**What it validates**: Multiple SSTables are searched correctly in reverse chronological order.

- Insert entries in batches, triggering multiple flushes (producing multiple SSTable files).
- Overwrite some keys across flushes.
- Verify `get()` returns the latest value for overwritten keys.
- Validates: SSTable ordering, Last-Write-Wins across SSTables (I8, I16).

### How to Run

```
make clean && make && ./flsm
```

All tests (Phase 1 + Phase 2) run sequentially. Pass criteria: all `[PASS]`, zero `[FAIL]`, exit code 0.
