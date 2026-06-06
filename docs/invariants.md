# System Invariants — Phase 1

These invariants must hold at all times. Any violation is a correctness bug.

---

## I1: Post-Sync Durability

After `WAL.sync()` returns, the appended record is recoverable across any subsequent crash.

## I2: Replay Completeness

WAL replay recovers every record that was successfully synced, in the order it was written.

## I3: Replay Safety

WAL replay never produces invalid, corrupted, or fabricated entries. Every emitted entry has passed CRC32 validation.

## I4: Fail-Stop on Corruption

Replay stops at the first invalid record. It does not skip forward or attempt to recover records beyond the corruption point.

## I5: Replay Idempotency

Replaying the same WAL file any number of times produces the same memtable state.

## I6: Write Ordering

The write path always executes: WAL append → sync → memtable update. This order is never reordered.

## I7: Memtable-WAL Consistency

After recovery, the memtable is a faithful representation of the WAL's valid content. No extra state, no missing records.

## I8: Last-Write-Wins

For any key written multiple times, `get()` returns the value from the most recent `put()` — both live and after recovery.

## I9: Append Atomicity

A record is either fully written to the WAL or not written at all. No partial record is left by the append implementation under normal operation. The write loop retries short writes until complete.

## I10: WAL Immutability During Replay

The WAL file is never modified during replay. Replay is a pure read-only operation, safe to retry after a crash during recovery.
