/**
 * @file raft_persistence.h
 * @brief Raft Persistence API - Durable Storage for Log and State
 * 
 * Provides persistent storage for Raft using LMDB (Lightning Memory-Mapped Database).
 * Ensures durability guarantees for:
 * - Persistent state (current_term, voted_for)
 * - Log entries
 * - Snapshots
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
 * @brief Raft persistence context
 * 
 * Manages LMDB environment and databases for Raft state.
 */
typedef struct raft_persistence_context raft_persistence_context_t;

/**
 * @brief Persistence configuration
 */
typedef struct {
    const char* data_dir;           /**< Directory for LMDB files */
    size_t max_db_size_mb;          /**< Maximum database size in MB (default: 1024) */
    bool sync_on_write;             /**< Fsync after every write (default: true) */
    logger_t* logger;               /**< Logger */
} raft_persistence_config_t;

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

/**
 * @brief Create persistence context
 * 
 * Opens LMDB environment and databases for:
 * - state: current_term, voted_for
 * - log: log entries
 * - snapshot: snapshot metadata and data
 * 
 * @param config Persistence configuration
 * @param context_out Output: created context
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_create(
    const raft_persistence_config_t* config,
    raft_persistence_context_t** context_out
);

/**
 * @brief Destroy persistence context
 * 
 * Closes LMDB environment and frees resources.
 * 
 * @param context Persistence context
 */
void raft_persistence_destroy(raft_persistence_context_t* context);

/* ============================================================================
 * STATE PERSISTENCE
 * ========================================================================= */

/**
 * @brief Persistent state
 */
typedef struct {
    uint32_t current_term;      /**< Latest term seen */
    char voted_for[64];         /**< Candidate voted for in current term */
} raft_persistent_state_t;

/**
 * @brief Save persistent state
 * 
 * Atomically saves current_term and voted_for to disk.
 * Must be called BEFORE responding to RequestVote or AppendEntries.
 * 
 * @param context Persistence context
 * @param state State to save
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_save_state(
    raft_persistence_context_t* context,
    const raft_persistent_state_t* state
);

/**
 * @brief Load persistent state
 * 
 * Loads current_term and voted_for from disk.
 * Called during node initialization.
 * 
 * @param context Persistence context
 * @param state_out Output: loaded state
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if no state exists
 */
distric_err_t raft_persistence_load_state(
    raft_persistence_context_t* context,
    raft_persistent_state_t* state_out
);

/* ============================================================================
 * LOG PERSISTENCE
 * ========================================================================= */

/**
 * @brief Append log entry to persistent storage
 * 
 * Appends entry to LMDB log database.
 * Entries are keyed by index.
 * 
 * @param context Persistence context
 * @param entry Log entry to append
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_append_entry(
    raft_persistence_context_t* context,
    const raft_log_entry_t* entry
);

/**
 * @brief Load log entry from persistent storage
 * 
 * @param context Persistence context
 * @param index Log index
 * @param entry_out Output: loaded entry (caller must free data)
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if not found
 */
distric_err_t raft_persistence_load_entry(
    raft_persistence_context_t* context,
    uint32_t index,
    raft_log_entry_t* entry_out
);

/**
 * @brief Load all log entries
 * 
 * Loads all entries from persistent storage.
 * Used during crash recovery.
 * 
 * @param context Persistence context
 * @param entries_out Output: array of entries (caller must free)
 * @param count_out Output: number of entries
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_load_all_entries(
    raft_persistence_context_t* context,
    raft_log_entry_t** entries_out,
    size_t* count_out
);

/**
 * @brief Truncate log from index onwards
 * 
 * Deletes all entries with index >= from_index.
 * Used when follower detects log conflict.
 * 
 * @param context Persistence context
 * @param from_index Index to truncate from (inclusive)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_truncate_log(
    raft_persistence_context_t* context,
    uint32_t from_index
);

/**
 * @brief Get log boundaries
 * 
 * Returns first and last log indices in persistent storage.
 * 
 * @param context Persistence context
 * @param first_index_out Output: first index (0 if empty)
 * @param last_index_out Output: last index (0 if empty)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_get_log_bounds(
    raft_persistence_context_t* context,
    uint32_t* first_index_out,
    uint32_t* last_index_out
);

/* ============================================================================
 * SNAPSHOT PERSISTENCE
 * ========================================================================= */

/**
 * @brief Snapshot metadata
 */
typedef struct {
    uint32_t last_included_index;   /**< Last log index in snapshot */
    uint32_t last_included_term;    /**< Term of last_included_index */
    size_t data_len;                /**< Snapshot data length */
} raft_snapshot_metadata_t;

/**
 * @brief Save snapshot
 * 
 * Saves snapshot metadata and data to disk.
 * Also deletes log entries up to last_included_index (log compaction).
 * 
 * @param context Persistence context
 * @param metadata Snapshot metadata
 * @param data Snapshot data
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_save_snapshot(
    raft_persistence_context_t* context,
    const raft_snapshot_metadata_t* metadata,
    const uint8_t* data
);

/**
 * @brief Load snapshot metadata
 * 
 * @param context Persistence context
 * @param metadata_out Output: snapshot metadata
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if no snapshot exists
 */
distric_err_t raft_persistence_load_snapshot_metadata(
    raft_persistence_context_t* context,
    raft_snapshot_metadata_t* metadata_out
);

/**
 * @brief Load snapshot data
 * 
 * @param context Persistence context
 * @param data_out Output: snapshot data (caller must free)
 * @param len_out Output: data length
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if no snapshot exists
 */
distric_err_t raft_persistence_load_snapshot_data(
    raft_persistence_context_t* context,
    uint8_t** data_out,
    size_t* len_out
);

/* ============================================================================
 * UTILITIES
 * ========================================================================= */

/**
 * @brief Compact database (LMDB)
 * 
 * Frees up space from deleted entries.
 * 
 * @param context Persistence context
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_compact(raft_persistence_context_t* context);

/**
 * @brief Get database statistics
 */
typedef struct {
    size_t total_size_bytes;        /**< Total database size */
    size_t state_size_bytes;        /**< State database size */
    size_t log_entry_count;         /**< Number of log entries */
    size_t log_size_bytes;          /**< Log database size */
    bool has_snapshot;              /**< Snapshot exists */
    size_t snapshot_size_bytes;     /**< Snapshot size */
} raft_persistence_stats_t;

/**
 * @brief Get persistence statistics
 * 
 * @param context Persistence context
 * @param stats_out Output: statistics
 * @return DISTRIC_OK on success
 */
distric_err_t raft_persistence_get_stats(
    raft_persistence_context_t* context,
    raft_persistence_stats_t* stats_out
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_PERSISTENCE_H */