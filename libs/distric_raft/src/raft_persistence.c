/**
 * @file raft_persistence.c
 * @brief Raft Persistence Implementation using LMDB
 * 
 * Provides durable storage for Raft state, log, and snapshots.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_raft/raft_persistence.h"
#include <lmdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

/**
 * @brief Persistence context
 */
struct raft_persistence_context {
    /* Configuration */
    raft_persistence_config_t config;
    
    /* LMDB environment */
    MDB_env* env;
    
    /* LMDB databases */
    MDB_dbi state_db;       /**< Persistent state (term, voted_for) */
    MDB_dbi log_db;         /**< Log entries (key=index, value=entry) */
    MDB_dbi snapshot_db;    /**< Snapshot (metadata + data) */
    
    /* Statistics */
    _Atomic size_t total_writes;
    _Atomic size_t total_reads;
};

/* Database keys */
#define STATE_KEY_TERM "current_term"
#define STATE_KEY_VOTED_FOR "voted_for"
#define SNAPSHOT_KEY_METADATA "metadata"
#define SNAPSHOT_KEY_DATA "data"

/* ============================================================================
 * LMDB ERROR HANDLING
 * ========================================================================= */

static distric_err_t lmdb_err_to_distric(int mdb_err) {
    switch (mdb_err) {
        case MDB_SUCCESS:
            return DISTRIC_OK;
        case MDB_NOTFOUND:
            return DISTRIC_ERR_NOT_FOUND;
        case ENOMEM:
        case MDB_MAP_FULL:
            return DISTRIC_ERR_NO_MEMORY;
        default:
            return DISTRIC_ERR_INIT_FAILED;
    }
}

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

distric_err_t raft_persistence_create(
    const raft_persistence_config_t* config,
    raft_persistence_context_t** context_out
) {
    if (!config || !config->data_dir || !context_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    raft_persistence_context_t* context = (raft_persistence_context_t*)calloc(
        1, sizeof(raft_persistence_context_t));
    if (!context) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Copy configuration */
    memcpy(&context->config, config, sizeof(raft_persistence_config_t));
    if (context->config.max_db_size_mb == 0) {
        context->config.max_db_size_mb = 1024;  /* Default: 1GB */
    }
    
    /* Create data directory if it doesn't exist */
    struct stat st;
    if (stat(config->data_dir, &st) != 0) {
        if (mkdir(config->data_dir, 0755) != 0) {
            LOG_ERROR(config->logger, "raft_persistence", 
                     "Failed to create data directory",
                     "dir", config->data_dir,
                     "error", strerror(errno));
            free(context);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    /* Create LMDB environment */
    int rc = mdb_env_create(&context->env);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR(config->logger, "raft_persistence", 
                 "Failed to create LMDB environment",
                 "error", mdb_strerror(rc));
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Set map size (max database size) */
    size_t map_size = (size_t)context->config.max_db_size_mb * 1024 * 1024;
    rc = mdb_env_set_mapsize(context->env, map_size);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR(config->logger, "raft_persistence", 
                 "Failed to set LMDB map size",
                 "error", mdb_strerror(rc));
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Set max databases */
    rc = mdb_env_set_maxdbs(context->env, 3);  /* state, log, snapshot */
    if (rc != MDB_SUCCESS) {
        LOG_ERROR(config->logger, "raft_persistence", 
                 "Failed to set max databases",
                 "error", mdb_strerror(rc));
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Open environment */
    unsigned int env_flags = MDB_NOSUBDIR;
    if (!context->config.sync_on_write) {
        env_flags |= MDB_NOSYNC;  /* Faster but less durable */
    }
    
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/raft.mdb", config->data_dir);
    
    rc = mdb_env_open(context->env, db_path, env_flags, 0644);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR(config->logger, "raft_persistence", 
                 "Failed to open LMDB environment",
                 "path", db_path,
                 "error", mdb_strerror(rc));
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Open databases */
    MDB_txn* txn = NULL;
    rc = mdb_txn_begin(context->env, NULL, 0, &txn);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* State database */
    rc = mdb_dbi_open(txn, "state", MDB_CREATE, &context->state_db);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Log database (integer keys for indices) */
    rc = mdb_dbi_open(txn, "log", MDB_CREATE | MDB_INTEGERKEY, &context->log_db);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Snapshot database */
    rc = mdb_dbi_open(txn, "snapshot", MDB_CREATE, &context->snapshot_db);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Commit transaction */
    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(context->env);
        free(context);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    LOG_INFO(config->logger, "raft_persistence", "Persistence initialized",
            "data_dir", config->data_dir,
            "max_size_mb", &(int){context->config.max_db_size_mb},
            "sync", context->config.sync_on_write ? "enabled" : "disabled");
    
    *context_out = context;
    return DISTRIC_OK;
}

void raft_persistence_destroy(raft_persistence_context_t* context) {
    if (!context) return;
    
    /* Close databases */
    if (context->env) {
        mdb_env_sync(context->env, 1);  /* Force sync */
        mdb_env_close(context->env);
    }
    
    LOG_INFO(context->config.logger, "raft_persistence", "Persistence destroyed",
            "total_writes", &(int){atomic_load(&context->total_writes)},
            "total_reads", &(int){atomic_load(&context->total_reads)});
    
    free(context);
}

/* ============================================================================
 * STATE PERSISTENCE
 * ========================================================================= */

distric_err_t raft_persistence_save_state(
    raft_persistence_context_t* context,
    const raft_persistent_state_t* state
) {
    if (!context || !state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, 0, &txn);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR(context->config.logger, "raft_persistence",
                 "Failed to begin transaction",
                 "error", mdb_strerror(rc));
        return lmdb_err_to_distric(rc);
    }
    
    /* Save current_term */
    MDB_val key, val;
    key.mv_data = (void*)STATE_KEY_TERM;
    key.mv_size = strlen(STATE_KEY_TERM);
    val.mv_data = (void*)&state->current_term;
    val.mv_size = sizeof(state->current_term);
    
    rc = mdb_put(txn, context->state_db, &key, &val, 0);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        LOG_ERROR(context->config.logger, "raft_persistence",
                 "Failed to save current_term",
                 "error", mdb_strerror(rc));
        return lmdb_err_to_distric(rc);
    }
    
    /* Save voted_for */
    key.mv_data = (void*)STATE_KEY_VOTED_FOR;
    key.mv_size = strlen(STATE_KEY_VOTED_FOR);
    val.mv_data = (void*)state->voted_for;
    val.mv_size = strlen(state->voted_for) + 1;  /* Include null terminator */
    
    rc = mdb_put(txn, context->state_db, &key, &val, 0);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        LOG_ERROR(context->config.logger, "raft_persistence",
                 "Failed to save voted_for",
                 "error", mdb_strerror(rc));
        return lmdb_err_to_distric(rc);
    }
    
    /* Commit */
    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR(context->config.logger, "raft_persistence",
                 "Failed to commit state",
                 "error", mdb_strerror(rc));
        return lmdb_err_to_distric(rc);
    }
    
    atomic_fetch_add(&context->total_writes, 1);
    
    LOG_DEBUG(context->config.logger, "raft_persistence", "State saved",
             "term", &(int){state->current_term},
             "voted_for", state->voted_for);
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_load_state(
    raft_persistence_context_t* context,
    raft_persistent_state_t* state_out
) {
    if (!context || !state_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    memset(state_out, 0, sizeof(raft_persistent_state_t));
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    /* Load current_term */
    MDB_val key, val;
    key.mv_data = (void*)STATE_KEY_TERM;
    key.mv_size = strlen(STATE_KEY_TERM);
    
    rc = mdb_get(txn, context->state_db, &key, &val);
    if (rc == MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return DISTRIC_ERR_NOT_FOUND;
    } else if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    memcpy(&state_out->current_term, val.mv_data, sizeof(uint32_t));
    
    /* Load voted_for */
    key.mv_data = (void*)STATE_KEY_VOTED_FOR;
    key.mv_size = strlen(STATE_KEY_VOTED_FOR);
    
    rc = mdb_get(txn, context->state_db, &key, &val);
    if (rc == MDB_SUCCESS) {
        size_t len = val.mv_size < sizeof(state_out->voted_for) ? 
                     val.mv_size : sizeof(state_out->voted_for) - 1;
        memcpy(state_out->voted_for, val.mv_data, len);
        state_out->voted_for[len] = '\0';
    } else if (rc != MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    mdb_txn_abort(txn);
    
    atomic_fetch_add(&context->total_reads, 1);
    
    LOG_DEBUG(context->config.logger, "raft_persistence", "State loaded",
             "term", &(int){state_out->current_term},
             "voted_for", state_out->voted_for);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * LOG PERSISTENCE
 * ========================================================================= */

/**
 * @brief Serialized log entry format
 * 
 * [uint32_t index]
 * [uint32_t term]
 * [uint8_t type]
 * [uint32_t data_len]
 * [uint8_t data[data_len]]
 */

static size_t serialize_log_entry(const raft_log_entry_t* entry, uint8_t** buf_out) {
    size_t total_len = sizeof(uint32_t) * 3 + sizeof(uint8_t) + entry->data_len;
    
    uint8_t* buf = (uint8_t*)malloc(total_len);
    if (!buf) {
        return 0;
    }
    
    size_t offset = 0;
    
    /* Index */
    memcpy(buf + offset, &entry->index, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    /* Term */
    memcpy(buf + offset, &entry->term, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    /* Type */
    memcpy(buf + offset, &entry->type, sizeof(uint8_t));
    offset += sizeof(uint8_t);
    
    /* Data length */
    memcpy(buf + offset, &entry->data_len, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    /* Data */
    if (entry->data && entry->data_len > 0) {
        memcpy(buf + offset, entry->data, entry->data_len);
    }
    
    *buf_out = buf;
    return total_len;
}

static distric_err_t deserialize_log_entry(const uint8_t* buf, size_t len, 
                                           raft_log_entry_t* entry_out) {
    if (len < sizeof(uint32_t) * 3 + sizeof(uint8_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    size_t offset = 0;
    
    /* Index */
    memcpy(&entry_out->index, buf + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    /* Term */
    memcpy(&entry_out->term, buf + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    /* Type */
    memcpy(&entry_out->type, buf + offset, sizeof(uint8_t));
    offset += sizeof(uint8_t);
    
    /* Data length */
    memcpy(&entry_out->data_len, buf + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    /* Data */
    if (entry_out->data_len > 0) {
        if (offset + entry_out->data_len > len) {
            return DISTRIC_ERR_INVALID_FORMAT;
        }
        
        entry_out->data = (uint8_t*)malloc(entry_out->data_len);
        if (!entry_out->data) {
            return DISTRIC_ERR_NO_MEMORY;
        }
        
        memcpy(entry_out->data, buf + offset, entry_out->data_len);
    } else {
        entry_out->data = NULL;
    }
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_append_entry(
    raft_persistence_context_t* context,
    const raft_log_entry_t* entry
) {
    if (!context || !entry) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Serialize entry */
    uint8_t* buf = NULL;
    size_t len = serialize_log_entry(entry, &buf);
    if (len == 0) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, 0, &txn);
    if (rc != MDB_SUCCESS) {
        free(buf);
        return lmdb_err_to_distric(rc);
    }
    
    /* Store with index as key */
    MDB_val key, val;
    key.mv_data = (void*)&entry->index;
    key.mv_size = sizeof(uint32_t);
    val.mv_data = buf;
    val.mv_size = len;
    
    rc = mdb_put(txn, context->log_db, &key, &val, 0);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        free(buf);
        LOG_ERROR(context->config.logger, "raft_persistence",
                 "Failed to append log entry",
                 "index", &(int){entry->index},
                 "error", mdb_strerror(rc));
        return lmdb_err_to_distric(rc);
    }
    
    /* Commit */
    rc = mdb_txn_commit(txn);
    free(buf);
    
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    atomic_fetch_add(&context->total_writes, 1);
    
    LOG_DEBUG(context->config.logger, "raft_persistence", "Entry appended",
             "index", &(int){entry->index},
             "term", &(int){entry->term});
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_load_entry(
    raft_persistence_context_t* context,
    uint32_t index,
    raft_log_entry_t* entry_out
) {
    if (!context || !entry_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    memset(entry_out, 0, sizeof(raft_log_entry_t));
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    MDB_val key, val;
    key.mv_data = &index;
    key.mv_size = sizeof(uint32_t);
    
    rc = mdb_get(txn, context->log_db, &key, &val);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    /* Deserialize */
    distric_err_t err = deserialize_log_entry((const uint8_t*)val.mv_data, 
                                              val.mv_size, entry_out);
    
    mdb_txn_abort(txn);
    
    if (err == DISTRIC_OK) {
        atomic_fetch_add(&context->total_reads, 1);
    }
    
    return err;
}

distric_err_t raft_persistence_load_all_entries(
    raft_persistence_context_t* context,
    raft_log_entry_t** entries_out,
    size_t* count_out
) {
    if (!context || !entries_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    *entries_out = NULL;
    *count_out = 0;
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    /* Count entries first */
    MDB_cursor* cursor = NULL;
    rc = mdb_cursor_open(txn, context->log_db, &cursor);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    MDB_val key, val;
    size_t count = 0;
    
    rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        count++;
        rc = mdb_cursor_get(cursor, &key, &val, MDB_NEXT);
    }
    
    if (count == 0) {
        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);
        return DISTRIC_OK;
    }
    
    /* Allocate array */
    raft_log_entry_t* entries = (raft_log_entry_t*)calloc(count, sizeof(raft_log_entry_t));
    if (!entries) {
        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Load entries */
    size_t i = 0;
    rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST);
    while (rc == MDB_SUCCESS && i < count) {
        distric_err_t err = deserialize_log_entry((const uint8_t*)val.mv_data,
                                                  val.mv_size, &entries[i]);
        if (err != DISTRIC_OK) {
            /* Cleanup on error */
            for (size_t j = 0; j < i; j++) {
                free(entries[j].data);
            }
            free(entries);
            mdb_cursor_close(cursor);
            mdb_txn_abort(txn);
            return err;
        }
        
        i++;
        rc = mdb_cursor_get(cursor, &key, &val, MDB_NEXT);
    }
    
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    
    *entries_out = entries;
    *count_out = count;
    
    atomic_fetch_add(&context->total_reads, count);
    
    LOG_INFO(context->config.logger, "raft_persistence", "All entries loaded",
            "count", &(int){count});
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_truncate_log(
    raft_persistence_context_t* context,
    uint32_t from_index
) {
    if (!context) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, 0, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    /* Open cursor */
    MDB_cursor* cursor = NULL;
    rc = mdb_cursor_open(txn, context->log_db, &cursor);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    /* Delete all entries with index >= from_index */
    MDB_val key, val;
    size_t deleted = 0;
    
    rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        uint32_t index = *(uint32_t*)key.mv_data;
        
        if (index >= from_index) {
            rc = mdb_cursor_del(cursor, 0);
            if (rc != MDB_SUCCESS) {
                mdb_cursor_close(cursor);
                mdb_txn_abort(txn);
                return lmdb_err_to_distric(rc);
            }
            deleted++;
        }
        
        rc = mdb_cursor_get(cursor, &key, &val, MDB_NEXT);
    }
    
    mdb_cursor_close(cursor);
    
    /* Commit */
    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    LOG_INFO(context->config.logger, "raft_persistence", "Log truncated",
            "from_index", &(int){from_index},
            "deleted", &(int){deleted});
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_get_log_bounds(
    raft_persistence_context_t* context,
    uint32_t* first_index_out,
    uint32_t* last_index_out
) {
    if (!context || !first_index_out || !last_index_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    *first_index_out = 0;
    *last_index_out = 0;
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    MDB_cursor* cursor = NULL;
    rc = mdb_cursor_open(txn, context->log_db, &cursor);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    MDB_val key, val;
    
    /* Get first */
    rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST);
    if (rc == MDB_SUCCESS) {
        *first_index_out = *(uint32_t*)key.mv_data;
        
        /* Get last */
        rc = mdb_cursor_get(cursor, &key, &val, MDB_LAST);
        if (rc == MDB_SUCCESS) {
            *last_index_out = *(uint32_t*)key.mv_data;
        }
    }
    
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * SNAPSHOT PERSISTENCE
 * ========================================================================= */

distric_err_t raft_persistence_save_snapshot(
    raft_persistence_context_t* context,
    const raft_snapshot_metadata_t* metadata,
    const uint8_t* data
) {
    if (!context || !metadata || !data) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, 0, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    /* Save metadata */
    MDB_val key, val;
    key.mv_data = (void*)SNAPSHOT_KEY_METADATA;
    key.mv_size = strlen(SNAPSHOT_KEY_METADATA);
    val.mv_data = (void*)metadata;
    val.mv_size = sizeof(raft_snapshot_metadata_t);
    
    rc = mdb_put(txn, context->snapshot_db, &key, &val, 0);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    /* Save data */
    key.mv_data = (void*)SNAPSHOT_KEY_DATA;
    key.mv_size = strlen(SNAPSHOT_KEY_DATA);
    val.mv_data = (void*)data;
    val.mv_size = metadata->data_len;
    
    rc = mdb_put(txn, context->snapshot_db, &key, &val, 0);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    /* Commit */
    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    /* Delete compacted log entries */
    distric_err_t err = raft_persistence_truncate_log(context, metadata->last_included_index + 1);
    if (err != DISTRIC_OK) {
        LOG_WARN(context->config.logger, "raft_persistence",
                "Failed to truncate log after snapshot");
    }
    
    LOG_INFO(context->config.logger, "raft_persistence", "Snapshot saved",
            "last_included_index", &(int){metadata->last_included_index},
            "last_included_term", &(int){metadata->last_included_term},
            "data_len", &(int){metadata->data_len});
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_load_snapshot_metadata(
    raft_persistence_context_t* context,
    raft_snapshot_metadata_t* metadata_out
) {
    if (!context || !metadata_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    memset(metadata_out, 0, sizeof(raft_snapshot_metadata_t));
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    MDB_val key, val;
    key.mv_data = (void*)SNAPSHOT_KEY_METADATA;
    key.mv_size = strlen(SNAPSHOT_KEY_METADATA);
    
    rc = mdb_get(txn, context->snapshot_db, &key, &val);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    memcpy(metadata_out, val.mv_data, sizeof(raft_snapshot_metadata_t));
    
    mdb_txn_abort(txn);
    return DISTRIC_OK;
}

distric_err_t raft_persistence_load_snapshot_data(
    raft_persistence_context_t* context,
    uint8_t** data_out,
    size_t* len_out
) {
    if (!context || !data_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    *data_out = NULL;
    *len_out = 0;
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    MDB_val key, val;
    key.mv_data = (void*)SNAPSHOT_KEY_DATA;
    key.mv_size = strlen(SNAPSHOT_KEY_DATA);
    
    rc = mdb_get(txn, context->snapshot_db, &key, &val);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return lmdb_err_to_distric(rc);
    }
    
    /* Copy data */
    uint8_t* data = (uint8_t*)malloc(val.mv_size);
    if (!data) {
        mdb_txn_abort(txn);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memcpy(data, val.mv_data, val.mv_size);
    *data_out = data;
    *len_out = val.mv_size;
    
    mdb_txn_abort(txn);
    return DISTRIC_OK;
}

/* ============================================================================
 * UTILITIES
 * ========================================================================= */

distric_err_t raft_persistence_compact(raft_persistence_context_t* context) {
    if (!context) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* LMDB auto-compacts on commit, but we can force a sync */
    int rc = mdb_env_sync(context->env, 1);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    LOG_INFO(context->config.logger, "raft_persistence", "Database compacted");
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_get_stats(
    raft_persistence_context_t* context,
    raft_persistence_stats_t* stats_out
) {
    if (!context || !stats_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    memset(stats_out, 0, sizeof(raft_persistence_stats_t));
    
    MDB_txn* txn = NULL;
    int rc = mdb_txn_begin(context->env, NULL, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_err_to_distric(rc);
    }
    
    /* Get environment info */
    MDB_envinfo env_info;
    rc = mdb_env_info(context->env, &env_info);
    if (rc == MDB_SUCCESS) {
        stats_out->total_size_bytes = env_info.me_mapsize;
    }
    
    /* Get log entry count */
    MDB_stat stat;
    rc = mdb_stat(txn, context->log_db, &stat);
    if (rc == MDB_SUCCESS) {
        stats_out->log_entry_count = stat.ms_entries;
    }
    
    /* Check if snapshot exists */
    MDB_val key, val;
    key.mv_data = (void*)SNAPSHOT_KEY_METADATA;
    key.mv_size = strlen(SNAPSHOT_KEY_METADATA);
    
    rc = mdb_get(txn, context->snapshot_db, &key, &val);
    if (rc == MDB_SUCCESS) {
        stats_out->has_snapshot = true;
        raft_snapshot_metadata_t metadata;
        memcpy(&metadata, val.mv_data, sizeof(raft_snapshot_metadata_t));
        stats_out->snapshot_size_bytes = metadata.data_len;
    }
    
    mdb_txn_abort(txn);
    
    return DISTRIC_OK;
}