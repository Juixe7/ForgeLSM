# ForgeLSM Final Project Plan

## Purpose

ForgeLSM will be developed into a local-first, correctness-verifiable key-value database project.

The final project is not meant to claim production-grade database maturity. Its goal is more precise and more honest:

- implement a real storage-engine architecture using WAL, Memtable, SSTables, Bloom filters, Manifest, VLog, compaction, and GC;
- expose those internals through a plain browser-based verification console;
- allow anyone to run mass-data tests and see evidence that the engine behaves correctly;
- clearly label which guarantees are proven, partially proven, or not yet proven.

The final result should feel like a small local database laboratory: a person can start the database on their machine, open `localhost:8080`, run verification workloads, inspect internal files, and understand whether the system is behaving correctly.

## Final Project Identity

The final project will be:

**A local-first persistent key-value database with a verification console.**

It will run like this:

```bash
./flsm serve
```

Then the user opens:

```text
http://localhost:8080
```

The browser UI will not be a marketing dashboard. It will be an evidence-first verification console.

The project will focus on proving behavior such as:

- keys can be written and read back correctly;
- overwrites return the newest value;
- deletes do not resurrect old values;
- WAL recovery restores recent committed writes;
- flushes produce SSTable files;
- SSTables are searched correctly;
- Bloom filters skip missing-key searches without hiding existing keys;
- compaction preserves newest-write-wins behavior;
- manifest state matches the actual SSTable files;
- VLog pointers return the correct values;
- GC preserves live values and does not rewrite stale versions;
- any unproven crash-safety claims are clearly shown as unproven.

## What The Current Project Already Has

The current codebase already contains the core pieces:

- `WAL`: append-only write-ahead log with checksums and replay.
- `VLog`: append-only value storage.
- `Memtable`: in-memory sorted key-to-pointer map.
- `SSTable`: immutable sorted disk files.
- `BloomFilter`: per-SSTable probabilistic negative lookup.
- `Manifest`: list of visible SSTable files.
- `Compaction`: L0-to-L1 merge logic.
- `VLog GC`: live-value rewrite mechanism.
- `CLI`: simple interactive commands.
- `HTTP server`: dashboard and JSON API.
- `Benchmark`: simple workload runner.

The project already runs basic operations, but several guarantees need stronger implementation and stronger verification.

## Honest Current Risk Assessment

### Strong Or Mostly Working

- Basic `put` and `get`.
- Basic overwrite behavior.
- Basic tombstone/delete behavior.
- Memtable storage.
- SSTable binary search.
- Bloom filter integration.
- HTTP API for simple commands.
- Dashboard startup.
- Built-in test suite execution.

### Needs Hardening

- WAL taint reporting after recovery.
- WAL corrupt-tail handling.
- SSTable write durability.
- Manifest atomicity and durability.
- Compaction edge cases, especially tombstone handling.
- VLog GC crash behavior.
- Verification test quality.
- UI evidence quality.

### Must Be Labeled Carefully

The final project should not claim full production crash safety unless it is actually implemented and tested.

Claims should be split into:

- **Proven:** verified by deterministic or model-based tests.
- **Partially proven:** works for normal execution but not all crash boundaries.
- **Not proven:** known gap or future work.

## Final UI Direction

The current dashboard should be redesigned into a plain verification console.

It should avoid flashy visuals and instead show:

- system state tables;
- verification matrix;
- before-and-after evidence;
- detailed test steps;
- raw counts and mismatches;
- internal file state;
- clear pass/fail/partial labels.

### Main UI Sections

#### 1. System State

Shows current engine internals:

```text
Memtable entries
Memtable bytes
WAL files
WAL tainted status
VLog size
L0 SSTable count
L1 SSTable count
Manifest version
Storage directory
```

#### 2. Verification Matrix

Shows each feature and its latest verification result:

```text
Feature              Status     Evidence
Put/Get              PASS       100000/100000 keys matched
Overwrite            PASS       newest values returned
Delete/Tombstone     PASS       10000/10000 deleted keys hidden
WAL Recovery         PASS       restart recovered committed keys
Flush/SSTable        PASS       L0 count changed 0 -> 3
Bloom Filter         PASS       0 false negatives, 48123 skips
Compaction           PASS       L0 16 -> 0, L1 0 -> 2
VLog GC              PARTIAL    live keys preserved, crash safety not proven
Manifest Safety      PARTIAL    load/commit works, directory fsync not implemented
```

#### 3. Evidence Panel

When a user selects a verification result, the UI shows:

- test name;
- exact operations performed;
- expected invariant;
- actual observed result;
- keys written;
- keys read;
- mismatches;
- files created;
- state before;
- state after;
- raw JSON returned by the backend.

#### 4. Storage Internals

Shows actual database files:

```text
WAL files:
file              size       replayable
wal_000001.log   2.1 MB     yes

SSTables:
level   file              entries   min_key        max_key
L0      sst_000003.sst    5000      verify:0000    verify:4999
L1      sst_000001.sst    20000     verify:0000    verify:19999

Manifest:
version: 7
L0: [3, 4]
L1: [1, 2]
```

#### 5. Workload Controls

Manual typing is not enough to trigger real storage-engine behavior. The UI needs deterministic workload buttons:

```text
Insert 1K keys
Insert 10K keys
Insert 100K keys
Overwrite 10K keys
Delete 10K keys
Force flush
Run compaction
Run GC
Restart/recover verification
Run full verification
Reset test database
```

## Backend API Plan

### Verification Endpoints

Add endpoints:

```text
POST /api/verify/basic
POST /api/verify/overwrite
POST /api/verify/delete
POST /api/verify/recovery
POST /api/verify/flush
POST /api/verify/bloom
POST /api/verify/compaction
POST /api/verify/gc
POST /api/verify/full
```

Each endpoint should return structured JSON:

```json
{
  "name": "Flush and SSTable verification",
  "status": "pass",
  "summary": "All written keys were recovered from SSTable-backed storage.",
  "before": {
    "memtable_entries": 0,
    "l0_count": 0,
    "l1_count": 0
  },
  "after": {
    "memtable_entries": 120,
    "l0_count": 2,
    "l1_count": 0
  },
  "evidence": {
    "keys_written": 10000,
    "keys_read": 10000,
    "mismatches": 0,
    "sstables_created": ["sst_000001.sst", "sst_000002.sst"]
  },
  "steps": [
    {
      "action": "insert deterministic keys",
      "expected": "all puts complete successfully",
      "actual": "10000 puts completed",
      "pass": true
    },
    {
      "action": "trigger flush",
      "expected": "at least one L0 SSTable exists",
      "actual": "2 L0 SSTables exist",
      "pass": true
    },
    {
      "action": "read all keys",
      "expected": "all keys match reference model",
      "actual": "0 mismatches",
      "pass": true
    }
  ]
}
```

### Debug Endpoints

Add endpoints:

```text
GET /api/debug/state
GET /api/debug/files
GET /api/debug/wal
GET /api/debug/manifest
GET /api/debug/sstables
GET /api/debug/key-location?key=...
```

These endpoints should expose internal state for verification, not for performance.

Example `key-location` response:

```json
{
  "key": "verify:key:000042",
  "found": true,
  "source": "L1",
  "sstable": "sst_000004.sst",
  "vlog_pointer": {
    "file_id": 0,
    "offset": 88124,
    "length": 128
  }
}
```

## Verification Strategy

The final verification system should use three layers.

### 1. Deterministic Feature Tests

These are predictable tests for specific features.

Examples:

- write 1,000 keys and read them back;
- overwrite the same keys and confirm newest values;
- delete keys and confirm they are not found;
- force flush and confirm SSTables exist;
- query missing keys and confirm Bloom skips occur;
- compact L0 files and confirm values still match.

### 2. Model-Based Tests

This is the most important correctness strategy.

The test runner keeps a simple reference model:

```cpp
std::map<std::string, std::optional<std::string>> model;
```

Every operation is applied to both:

```text
ForgeLSM
reference model
```

Then the test compares results.

If ForgeLSM disagrees with the simple model, that is a correctness failure.

Example:

```text
operation 1: put key_1 value_a
operation 2: put key_1 value_b
operation 3: delete key_2
operation 4: get key_1

model says: value_b
ForgeLSM says: value_b
PASS
```

This should run with thousands or hundreds of thousands of operations.

### 3. Structural Invariant Tests

These verify that the internal data structures are shaped correctly.

Examples:

- SSTable entries are sorted.
- L0 files are read newest-first.
- L1 files do not overlap after compaction.
- Manifest references existing files only.
- Bloom filters never reject inserted keys.
- Tombstones stop reads from older SSTables.
- VLog pointers read the expected value length.

## Two-Month Execution Plan

## Month 1: Build Verification Foundation

### Week 1: Stabilize And Document Reality

Tasks:

- Review current code and list proven/partial/unproven guarantees.
- Fix README overclaims or add an honest guarantee matrix.
- Add a project status page in the UI.
- Add backend state snapshot function.
- Add `/api/debug/state`.
- Add `/api/debug/files`.

Deliverables:

- project guarantee matrix;
- debug state endpoint;
- UI system state table;
- clear distinction between metrics and proof.

### Week 2: Verification Runner Core

Tasks:

- Implement a backend verification result structure.
- Implement deterministic workload generation.
- Implement reference model comparison.
- Add `/api/verify/basic`.
- Add `/api/verify/overwrite`.
- Add `/api/verify/delete`.
- Add `/api/verify/full` for initial basic suite.

Deliverables:

- model-based test runner;
- PASS/FAIL JSON output;
- verification matrix in UI;
- evidence panel for basic tests.

### Week 3: Flush, SSTable, And Bloom Verification

Tasks:

- Add force-flush backend hook.
- Add SSTable metadata inspection.
- Add `/api/debug/sstables`.
- Add `/api/verify/flush`.
- Add `/api/verify/bloom`.
- Show SSTable files, levels, key ranges, and entry counts in UI.

Deliverables:

- visible proof that memtable data becomes SSTables;
- proof that flushed keys are still readable;
- proof that Bloom filters skip missing-key lookups;
- proof of zero Bloom false negatives for inserted keys.

### Week 4: WAL And Recovery Verification

Tasks:

- Fix WAL taint reporting.
- Decide whether to implement corrupt-tail truncation or label it as not implemented.
- Add isolated test database directories for verification runs.
- Add restart/recovery test path.
- Add `/api/verify/recovery`.
- Add WAL file inspection endpoint.

Deliverables:

- UI-visible WAL file list;
- restart recovery verification;
- recovery evidence with keys written, keys recovered, mismatches;
- explicit WAL status reporting.

## Month 2: Hardening And Advanced Internals

### Week 5: Compaction Verification

Tasks:

- Add force-compaction endpoint.
- Add compaction before/after state snapshot.
- Strengthen tombstone compaction tests.
- Verify newest-write-wins across L0 and L1.
- Add `/api/verify/compaction`.

Deliverables:

- proof that L0 compacts into L1;
- proof that values survive compaction;
- proof that overwrites do not revert;
- proof that deletes are not resurrected.

### Week 6: Manifest And SSTable Durability Hardening

Tasks:

- Write SSTables through temp files.
- Add file flush/fsync where possible.
- Rename SSTables into place after successful write.
- Improve manifest commit path.
- Add manifest inspection endpoint.
- Add manifest consistency verification.

Deliverables:

- safer SSTable write path;
- clearer manifest behavior;
- UI showing manifest version and referenced files;
- test that manifest references only existing valid SSTables.

### Week 7: VLog And GC Verification

Tasks:

- Add VLog size and pointer inspection.
- Add deterministic overwrite-heavy workload.
- Add GC verification endpoint.
- Check all live keys before and after GC.
- Clearly label GC crash safety as proven or not proven.

Deliverables:

- proof that normal GC preserves live values;
- stale-value behavior explained;
- UI-visible VLog before/after size;
- honest GC guarantee label.

### Week 8: Polish, Documentation, And Final Validation

Tasks:

- Replace remaining decorative dashboard elements with evidence tables.
- Add raw JSON view for each verification result.
- Add final full verification suite.
- Add final documentation.
- Add a guided demo script.
- Run large workload tests.

Deliverables:

- final verification console;
- final guarantee matrix;
- final demo workflow;
- documentation explaining architecture and proof strategy;
- known limitations section.

## Final Acceptance Criteria

The project is successful if a reviewer can:

1. Start ForgeLSM locally.
2. Open the browser UI.
3. Run full verification.
4. See deterministic PASS/FAIL results for all major features.
5. Inspect evidence for each test.
6. See internal files and state changes.
7. Run mass-data workloads without manually typing keys.
8. Confirm ForgeLSM agrees with a reference model.
9. See which guarantees are not yet proven.
10. Understand the architecture from the UI and documentation.

## Final Feature Guarantee Matrix

| Feature | Target Status | Verification Method |
|---|---|---|
| Basic put/get | Proven | deterministic and model-based tests |
| Overwrite newest-wins | Proven | repeated overwrite workload |
| Delete/tombstone | Proven | delete and read-after-delete tests |
| Memtable behavior | Proven | state inspection and model comparison |
| WAL replay recovery | Proven | restart verification |
| WAL corrupt-tail truncation | Partial or future | depends on implementation |
| VLog read path | Proven | pointer-backed reads and value checks |
| SSTable flush | Proven | forced flush and readback |
| SSTable checksum | Proven | corruption detection test |
| Bloom filter no false negatives | Proven | all inserted keys checked |
| Bloom skip effectiveness | Proven | missing-key workload |
| Manifest load/commit | Partial to proven | consistency and recovery tests |
| Compaction newest-wins | Proven | multi-level overwrite tests |
| Tombstone compaction safety | Proven if tests pass | delete plus compaction workloads |
| VLog GC normal correctness | Proven | live-key preservation tests |
| VLog GC crash safety | Partial or future | likely not fully proven in 2 months |
| Production crash safety | Not claimed | out of scope |

## What We Should Not Claim

The final project should not claim:

- production database readiness;
- distributed database behavior;
- cloud persistence without persistent volumes;
- complete crash safety for every possible filesystem failure;
- high-concurrency correctness;
- MVCC or snapshots;
- range scans;
- full SQL behavior.

These can be listed as future work.

## Final Demo Story

The final demonstration should go like this:

1. Start the database locally.
2. Open the verification console.
3. Show clean initial system state.
4. Run `Basic Put/Get`.
5. Run `Mass Insert`.
6. Force flush and show SSTables appear.
7. Run Bloom verification and show missing-key skips.
8. Run overwrite/delete tests.
9. Run compaction and show L0-to-L1 movement.
10. Restart database and run recovery verification.
11. Run GC verification.
12. Open evidence for each test.
13. Show final guarantee matrix with proven and partial areas.

This demonstrates not just that the database has metrics, but that its internal design actually behaves correctly under controlled workloads.

## Summary

In two months, ForgeLSM can realistically become a strong local-first storage-engine project with an honest verification console.

The best final version is not a flashy website and not a fake production database. It is a clear, inspectable system where a learner or reviewer can see:

- how writes move through WAL, VLog, Memtable, SSTables, and compaction;
- how reads find the newest value;
- how deletes and tombstones work;
- how Bloom filters reduce unnecessary searches;
- how recovery restores state;
- how correctness is tested against a simple reference model;
- where the implementation is proven and where it is still future work.

That is a realistic, valuable, and technically impressive two-month outcome.
