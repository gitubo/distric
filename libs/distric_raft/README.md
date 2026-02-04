# DistriC 2.0 - Session 3.3: Raft Log Replication

## Session Overview

**Phase**: 3 (Raft Consensus)  
**Session**: 3.3 (Log Replication)  
**Duration**: ~2-3 hours  
**Dependencies**: Sessions 3.1 (Leader Election), 3.2 (RPC Integration)

This session implements the core log replication mechanism that ensures consistency across the Raft cluster.

## What Was Implemented

### 1. Leader Heartbeat Mechanism (`raft_replication.c`)

Leaders send periodic empty AppendEntries RPCs (heartbeats) to:
- Maintain authority and prevent elections
- Advance follower commit indices
- Detect failed followers

**Key Functions**:
- `raft_should_send_heartbeat()` - Check if heartbeat needed
- `raft_mark_heartbeat_sent()` - Update heartbeat timestamp

**Configuration**:
```c
config.heartbeat_interval_ms = 50;  // Send heartbeat every 50ms
```

### 2. Log Replication to Followers

Leaders replicate log entries to all followers:
- Sends entries from `next_index[i]` onwards
- Batches up to 100 entries per RPC (configurable)
- Includes `prev_log_index` and `prev_log_term` for consistency check

**Key Functions**:
- `raft_get_entries_for_peer()` - Get entries to replicate
- `raft_should_replicate_to_peer()` - Check if peer needs entries
- `raft_free_log_entries()` - Free allocated entries

**Example**:
```c
raft_log_entry_t* entries = NULL;
size_t count = 0;
uint32_t prev_index, prev_term;

raft_get_entries_for_peer(node, peer_idx, &entries, &count, &prev_index, &prev_term);

// Send via RPC...

raft_free_log_entries(entries, count);
```

### 3. Commit Index Advancement

Leader advances `commit_index` when:
- Majority of peers have replicated entry
- Entry is from current term (safety requirement)

**Algorithm**:
```
For each N from commit_index+1 to last_log_index:
    If entry[N].term == current_term:
        Count replicas (including self)
        If replicas >= majority:
            commit_index = N
```

**Implementation**:
- `update_commit_index()` - Called after each successful replication
- Atomic operations ensure thread safety

### 4. Log Consistency Checks

Followers verify log consistency via `prev_log` check:
- Reject if `log[prev_log_index].term != prev_log_term`
- Leader decrements `next_index` and retries

**Optimization**: Instead of decrementing by 1, jump to last entry of conflicting term.

**Key Functions**:
- `raft_handle_log_conflict()` - Optimized backoff
- `raft_handle_append_entries_response()` - Process responses

### 5. Apply to State Machine

Once committed, entries are applied to state machine:
- Followers apply when `commit_index` advances
- Leaders apply after commit_index updated
- NO-OP entries are skipped

**Key Functions**:
- `apply_committed_entries()` - Apply all committed but unapplied entries
- Called in `raft_tick()` for both leaders and followers

**Example Callback**:
```c
void my_apply_fn(const raft_log_entry_t* entry, void* user_data) {
    if (entry->type == RAFT_ENTRY_COMMAND) {
        // Apply to state machine
        execute_command(entry->data, entry->data_len);
    }
}

config.apply_fn = my_apply_fn;
```

### 6. Waiting for Commit

Clients can wait for entries to commit:

```c
uint32_t index;
raft_append_entry(node, data, len, &index);

// Wait up to 5 seconds for commit
distric_err_t err = raft_wait_committed(node, index, 5000);
if (err == DISTRIC_OK) {
    printf("Entry committed!\n");
} else {
    printf("Timeout waiting for commit\n");
}
```

### 7. Replication Monitoring

Track replication progress:

```c
replication_stats_t stats;
raft_get_replication_stats(node, &stats);

printf("Last log: %u\n", stats.last_log_index);
printf("Committed: %u\n", stats.commit_index);
printf("Peers up-to-date: %u\n", stats.peers_up_to_date);
printf("Peers lagging: %u\n", stats.peers_lagging);

// Per-peer lag
for (size_t i = 0; i < peer_count; i++) {
    uint32_t lag = raft_get_peer_lag(node, i);
    printf("Peer %zu lag: %u entries\n", i, lag);
}
```

## Architecture

### Replication Flow

```
┌─────────────┐
│   Leader    │
│  (append)   │
└──────┬──────┘
       │
       │ 1. raft_append_entry()
       │    └─> Appends to local log
       │
       │ 2. raft_tick()
       │    └─> Checks if should replicate
       │
       │ 3. raft_get_entries_for_peer()
       │    └─> Gets entries to send
       │
       │ 4. RPC layer sends AppendEntries
       ▼
┌─────────────┐
│  Follower   │
│  (receive)  │
└──────┬──────┘
       │
       │ 5. raft_handle_append_entries()
       │    └─> Checks consistency, appends
       │
       │ 6. Returns success/failure
       │
       ▼
┌─────────────┐
│   Leader    │
│ (response)  │
└──────┬──────┘
       │
       │ 7. raft_handle_append_entries_response()
       │    └─> Updates next_index, match_index
       │
       │ 8. update_commit_index()
       │    └─> Advances commit if majority
       │
       │ 9. apply_committed_entries()
       │    └─> Calls apply_fn callback
       ▼
┌─────────────┐
│State Machine│
└─────────────┘
```

### Data Structures

**Leader State (per peer)**:
```c
uint32_t next_index[peer_count];   // Next entry to send
uint32_t match_index[peer_count];  // Highest entry replicated
```

**Global State**:
```c
_Atomic uint32_t commit_index;  // Highest committed entry
uint32_t last_applied;          // Highest applied entry
```

**Invariants**:
- `commit_index <= last_log_index`
- `last_applied <= commit_index`
- `match_index[i] < next_index[i]` (for all peers)

## Performance Characteristics

### Throughput
- **Batch Size**: 100 entries per RPC
- **Parallel Replication**: All peers replicated concurrently
- **Expected**: 10,000+ entries/sec on 5-node cluster

### Latency
- **Single Entry**: ~2 * RTT (append + commit)
- **Batched**: Amortized over batch
- **Target**: <10ms p99 for LAN

### Resource Usage
- **Memory**: O(log_size) for entries
- **CPU**: <5% per core at 10k entries/sec
- **Network**: ~100 KB/s per peer at 1k entries/sec

## Testing

### Unit Tests (11 tests)

```bash
./test_raft_replication
```

**Tests**:
1. `test_heartbeat_timing` - Heartbeat intervals
2. `test_replication_check` - Detection of replication needs
3. `test_get_entries_for_peer` - Entry retrieval
4. `test_append_entries_response_success` - Success handling
5. `test_append_entries_response_failure` - Failure handling
6. `test_commit_index_advancement` - Majority commit
7. `test_wait_committed` - Blocking wait for commit
8. `test_replication_stats` - Statistics accuracy
9. `test_log_conflict_resolution` - Conflict handling
10. `test_peer_lag_calculation` - Lag tracking
11. `test_apply_committed_entries` - Apply callback

### Expected Output

```
=== DistriC Raft - Log Replication Tests ===

[TEST] test_heartbeat_timing...
  Heartbeat timing works correctly
[PASS] test_heartbeat_timing

[TEST] test_commit_index_advancement...
  Commit index advances with majority replication
[PASS] test_commit_index_advancement

...

=== Test Results ===
Passed: 11
Failed: 0

✓ All Raft replication tests passed!
✓ Session 3.3 (Log Replication) COMPLETE
```

## Integration with Existing Code

### Modified Files

1. **raft_core.c**:
   - Added `last_heartbeat_sent_ms` field
   - Updated `raft_tick()` leader case
   - Added `apply_committed_entries()` helper
   - Added `update_commit_index()` helper

2. **raft_core.h**:
   - Added `raft_wait_committed()` declaration

3. **raft_rpc.c**:
   - Updated `append_entries_thread()` to use replication helpers
   - Added response handling via `raft_handle_append_entries_response()`

### New Files

1. **raft_replication.c** - Implementation (350 lines)
2. **raft_replication.h** - Public API (120 lines)
3. **test_raft_replication.c** - Tests (450 lines)

## Common Issues and Solutions

### Issue: Commit Index Not Advancing

**Symptoms**: Entries appended but never committed

**Causes**:
- Not enough peers replicated (need majority)
- Entry not from current term
- `update_commit_index()` not called

**Solution**:
```c
// Verify in raft_tick():
case RAFT_STATE_LEADER:
    update_commit_index(node);  // Must be called!
    break;
```

### Issue: Apply Callback Not Invoked

**Symptoms**: `apply_fn` callback never called

**Causes**:
- `apply_committed_entries()` not called in tick
- `last_applied` >= `commit_index`
- NO-OP entries (skipped automatically)

**Solution**:
```c
// Verify in raft_tick():
apply_committed_entries(node);  // For both leader and follower
```

### Issue: Replication Stuck

**Symptoms**: Peer's `match_index` not increasing

**Causes**:
- Log conflict not resolved
- Network partition
- `next_index` stuck at conflicting entry

**Solution**:
- Check `raft_handle_log_conflict()` is working
- Verify RPC retry logic
- Monitor `next_index` in logs

## Metrics to Monitor

Add these metrics to `raft_replication.c`:

```c
// Replication lag (per peer)
metrics_register_gauge(metrics, "raft_peer_lag", 
    "Entries behind leader", labels, &lag_metric);

// Commit lag
metrics_register_gauge(metrics, "raft_commit_lag",
    "Uncommitted entries", NULL, &commit_lag_metric);

// Apply lag
metrics_register_gauge(metrics, "raft_apply_lag",
    "Unapplied committed entries", NULL, &apply_lag_metric);

// Replication duration
metrics_register_histogram(metrics, "raft_replication_duration_seconds",
    "Time to replicate to peer", NULL, &repl_duration_metric);
```

## Files Delivered

```
distric_session_3_3/
├── raft_replication.c           # Implementation
├── raft_replication.h           # Public API
├── test_raft_replication.c      # Tests
├── INTEGRATION.md               # Integration guide
└── README.md                    # This file
```

## Validation Checklist

Before proceeding to Session 3.4 (Persistence):

- [x] All 11 tests pass
- [x] Valgrind reports no memory leaks
- [x] Commit index advances with majority
- [x] Apply callback invoked for committed entries
- [x] Heartbeats prevent unnecessary elections
- [x] Conflict resolution works correctly
- [x] Replication statistics accurate
- [x] `raft_wait_committed()` works with timeout
- [x] Batch replication limits entries per RPC
- [x] Parallel replication to all peers

## Next Session: Persistence (3.4)

Session 3.4 will add:
- Durable log storage (LMDB)
- State persistence (current_term, voted_for)
- Write-Ahead Logging (WAL)
- Crash recovery
- Snapshot creation and restoration

## References

- [Raft Paper](https://raft.github.io/raft.pdf) - Section 5.3 (Log replication)
- DistriC Implementation Strategy - Phase 3.3
- DistriC Requirements - REQ-RAFT-005 (Log replication)

---

**Session 3.3 Status**: ✅ COMPLETE  
**Time to Complete**: ~2 hours  
**Lines of Code**: ~920 lines (implementation + tests)  
**Test Coverage**: 11 unit tests, all passing  
**Ready for**: Session 3.4 (Persistence)