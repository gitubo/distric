/**
 * @file raft_persistence.h
 * @brief Raft Persistence - Simple File-Based Implementation
 * 
 * Simple file-based persistence for Raft state and log.
 * No external dependencies (no LMDB, SQLite, etc.)
 * 
 * File Layout:
 *   /data_dir/state.json     - Current term and voted_for
 *   /data_dir/log.dat        - Binary log entries
 *   /data_dir/snapshot.dat   - Snapshot (future)
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_RAFT_PERSISTENCE_H
#define DISTRIC_RAFT_PERSISTENCE_H

#include <distric_raft/raft_core.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PERSISTENCE CONTEXT
 * ========================================================================= */

/**
 * @brief Persistence context (opaque)
 */
typedef struct raft_persistence raft_persistence_t;

/**
 * @brief Persistence configuration
 */
typedef struct {
    const char* data_dir;       /**< Directory for persistent storage */
    logger_t* logger;           /**< Logger for persistence operations */
} raft_persistence_config_t;

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

/**
 * @brief Initialize persistence
 * 
 * Creates data directory if needed and opens/creates state and log files.
 * 
 * @param config Persistence configuration
 * @param persistence_out Output: created persistence context
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_init(
    const raft_persistence_config_t* config,
    raft_persistence_t** persistence_out
);

/**
 * @brief Destroy persistence and close files
 * 
 * @param persistence Persistence context
 */
void raft_persistence_destroy(raft_persistence_t* persistence);

/* ============================================================================
 * STATE OPERATIONS (term, voted_for)
 * ========================================================================= */

/**
 * @brief Save current term
 * 
 * Atomically updates state file with new term.
 * Uses fsync() for durability.
 * 
 * @param persistence Persistence context
 * @param term Current term
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_save_term(
    raft_persistence_t* persistence,
    uint32_t term
);

/**
 * @brief Save voted_for
 * 
 * Atomically updates state file with vote.
 * Uses fsync() for durability.
 * 
 * @param persistence Persistence context
 * @param voted_for Node ID voted for (NULL or empty = none)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_save_vote(
    raft_persistence_t* persistence,
    const char* voted_for
);

/**
 * @brief Save both term and vote atomically
 * 
 * More efficient than separate calls.
 * 
 * @param persistence Persistence context
 * @param term Current term
 * @param voted_for Node ID voted for (NULL or empty = none)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_save_state(
    raft_persistence_t* persistence,
    uint32_t term,
    const char* voted_for
);

/**
 * @brief Load saved state
 * 
 * Reads term and voted_for from state file.
 * 
 * @param persistence Persistence context
 * @param term_out Output: loaded term (0 if not found)
 * @param voted_for_out Output: voted for (buffer must be 64 bytes)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_load_state(
    raft_persistence_t* persistence,
    uint32_t* term_out,
    char* voted_for_out
);

/* ============================================================================
 * LOG OPERATIONS
 * ========================================================================= */

/**
 * @brief Append log entry to persistent storage
 * 
 * Appends entry to log file and fsyncs.
 * Entry data is copied internally.
 * 
 * @param persistence Persistence context
 * @param entry Log entry to append
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_append_log(
    raft_persistence_t* persistence,
    const raft_log_entry_t* entry
);

/**
 * @brief Load all log entries from storage
 * 
 * Reads entire log file and reconstructs entries.
 * Caller must free entries with raft_persistence_free_log().
 * 
 * @param persistence Persistence context
 * @param entries_out Output: array of log entries (caller must free)
 * @param count_out Output: number of entries
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_load_log(
    raft_persistence_t* persistence,
    raft_log_entry_t** entries_out,
    size_t* count_out
);

/**
 * @brief Truncate log from given index
 * 
 * Removes all entries from from_index onwards.
 * Used for log conflict resolution.
 * 
 * @param persistence Persistence context
 * @param from_index Index to truncate from (inclusive)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_truncate_log(
    raft_persistence_t* persistence,
    uint32_t from_index
);

/**
 * @brief Free log entries loaded by raft_persistence_load_log
 * 
 * @param entries Array of entries
 * @param count Number of entries
 */
void raft_persistence_free_log(raft_log_entry_t* entries, size_t count);

/* ============================================================================
 * SNAPSHOT OPERATIONS (Future)
 * ========================================================================= */

/**
 * @brief Compact log by creating snapshot
 * 
 * Removes log entries up to last_included_index.
 * Stores snapshot data in separate file.
 * 
 * @param persistence Persistence context
 * @param last_included_index Last index in snapshot
 * @param last_included_term Term of last index
 * @param snapshot_data Snapshot data
 * @param snapshot_len Snapshot length
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_save_snapshot(
    raft_persistence_t* persistence,
    uint32_t last_included_index,
    uint32_t last_included_term,
    const uint8_t* snapshot_data,
    size_t snapshot_len
);

/**
 * @brief Load snapshot from storage
 * 
 * @param persistence Persistence context
 * @param last_included_index_out Output: last index in snapshot
 * @param last_included_term_out Output: term of last index
 * @param snapshot_data_out Output: snapshot data (caller must free)
 * @param snapshot_len_out Output: snapshot length
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if no snapshot
 */
distric_err_t raft_persistence_load_snapshot(
    raft_persistence_t* persistence,
    uint32_t* last_included_index_out,
    uint32_t* last_included_term_out,
    uint8_t** snapshot_data_out,
    size_t* snapshot_len_out
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_PERSISTENCE_H */