/**
 * @file raft_persistence.c
 * @brief Raft Persistence Implementation with Snapshots
 * 
 * Complete file-based persistence with snapshot support (Session 3.5).
 * 
 * File Layout:
 *   /data_dir/state.json     - Current term and voted_for
 *   /data_dir/log.dat        - Binary log entries
 *   /data_dir/snapshot.dat   - Latest snapshot
 * 
 * State Format (JSON):
 *   {"term":42,"voted_for":"node-3"}
 * 
 * Log Format (binary):
 *   [4 bytes: magic "RAFT"]
 *   [4 bytes: version = 1]
 *   Repeated records:
 *     [4 bytes: index]
 *     [4 bytes: term]
 *     [1 byte: type]
 *     [4 bytes: data_len]
 *     [data_len bytes: data]
 * 
 * Snapshot Format (binary):
 *   [4 bytes: magic "SNAP"]
 *   [4 bytes: version = 1]
 *   [4 bytes: last_included_index]
 *   [4 bytes: last_included_term]
 *   [4 bytes: data_len]
 *   [4 bytes: crc32]
 *   [data_len bytes: snapshot data]
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_raft/raft_persistence.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdint.h>

/* ============================================================================
 * CONSTANTS
 * ========================================================================= */

#define LOG_FILE_MAGIC 0x52414654  /* "RAFT" */
#define LOG_FILE_VERSION 1

#define SNAPSHOT_MAGIC 0x534E4150  /* "SNAP" */
#define SNAPSHOT_VERSION 1

#define MAX_PATH_LEN 512

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

struct raft_persistence {
    char data_dir[256];
    char state_path[MAX_PATH_LEN];
    char state_tmp_path[MAX_PATH_LEN];
    char log_path[MAX_PATH_LEN];
    char snapshot_path[MAX_PATH_LEN];
    char snapshot_tmp_path[MAX_PATH_LEN];
    
    int log_fd;                    /* File descriptor for log file */
    
    logger_t* logger;
};

/* ============================================================================
 * CRC32 IMPLEMENTATION
 * ========================================================================= */

static uint32_t crc32_compute(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc = crc >> 1;
            }
        }
    }
    
    return ~crc;
}

/* ============================================================================
 * FILE UTILITIES
 * ========================================================================= */

static distric_err_t ensure_directory(const char* path, logger_t* logger) {
    struct stat st;
    
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return DISTRIC_OK;
        }
        LOG_ERROR(logger, "persistence", "Path exists but is not a directory", "path", path, NULL);
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (mkdir(path, 0755) != 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(logger, "persistence", "Failed to create directory",
                 "path", path, "errno", errno_str, NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    LOG_INFO(logger, "persistence", "Created directory", "path", path, NULL);
    return DISTRIC_OK;
}

static distric_err_t write_file_atomic(const char* path, const char* tmp_path,
                                       const void* data, size_t len, logger_t* logger) {
    /* Write to temporary file */
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(logger, "persistence", "Failed to create tmp file",
                 "path", tmp_path, "errno", errno_str, NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    ssize_t written = write(fd, data, len);
    if (written != (ssize_t)len) {
        char expected_str[32], written_str[32];
        snprintf(expected_str, sizeof(expected_str), "%zu", len);
        snprintf(written_str, sizeof(written_str), "%zd", written);
        LOG_ERROR(logger, "persistence", "Failed to write tmp file",
                 "expected", expected_str, "written", written_str, NULL);
        close(fd);
        unlink(tmp_path);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Sync to disk */
    if (fsync(fd) != 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(logger, "persistence", "Failed to fsync tmp file", "errno", errno_str, NULL);
        close(fd);
        unlink(tmp_path);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    close(fd);
    
    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(logger, "persistence", "Failed to rename tmp file", "errno", errno_str, NULL);
        unlink(tmp_path);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    return DISTRIC_OK;
}

static distric_err_t read_file(const char* path, char** data_out, size_t* len_out, logger_t* logger) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            *data_out = NULL;
            *len_out = 0;
            return DISTRIC_OK;  /* File doesn't exist yet */
        }
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(logger, "persistence", "Failed to open file", "path", path, "errno", errno_str, NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(logger, "persistence", "Failed to stat file", "errno", errno_str, NULL);
        close(fd);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    size_t size = st.st_size;
    if (size == 0) {
        close(fd);
        *data_out = NULL;
        *len_out = 0;
        return DISTRIC_OK;
    }
    
    /* Allocate buffer */
    char* buffer = (char*)malloc(size + 1);  /* +1 for null terminator */
    if (!buffer) {
        close(fd);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Read file */
    ssize_t bytes_read = read(fd, buffer, size);
    close(fd);
    
    if (bytes_read != (ssize_t)size) {
        LOG_ERROR(logger, "persistence", "Failed to read complete file", NULL);
        free(buffer);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    buffer[size] = '\0';  /* Null terminate for text parsing */
    
    *data_out = buffer;
    *len_out = size;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * STATE FILE OPERATIONS (Simple JSON-like format)
 * ========================================================================= */

static distric_err_t parse_state_file(const char* content, uint32_t* term_out, char* voted_for_out) {
    /* Very simple parser for: {"term":42,"voted_for":"node-3"} */
    *term_out = 0;
    voted_for_out[0] = '\0';
    
    if (!content || strlen(content) == 0) {
        return DISTRIC_OK;  /* Empty state */
    }
    
    /* Parse term */
    const char* term_str = strstr(content, "\"term\":");
    if (term_str) {
        term_str += 7;  /* Skip "term": */
        *term_out = (uint32_t)atoi(term_str);
    }
    
    /* Parse voted_for */
    const char* vote_str = strstr(content, "\"voted_for\":\"");
    if (vote_str) {
        vote_str += 13;  /* Skip "voted_for":" */
        const char* end = strchr(vote_str, '"');
        if (end) {
            size_t len = end - vote_str;
            if (len > 0 && len < 64) {
                strncpy(voted_for_out, vote_str, len);
                voted_for_out[len] = '\0';
            }
        }
    }
    
    return DISTRIC_OK;
}

static distric_err_t format_state_file(uint32_t term, const char* voted_for,
                                       char** content_out, size_t* len_out) {
    char buffer[256];
    int written;
    
    if (!voted_for || voted_for[0] == '\0') {
        written = snprintf(buffer, sizeof(buffer), "{\"term\":%u,\"voted_for\":\"\"}", term);
    } else {
        written = snprintf(buffer, sizeof(buffer), "{\"term\":%u,\"voted_for\":\"%s\"}", term, voted_for);
    }
    
    if (written < 0 || written >= (int)sizeof(buffer)) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    
    char* content = (char*)malloc(written + 1);
    if (!content) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memcpy(content, buffer, written + 1);
    *content_out = content;
    *len_out = written;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * LOG FILE OPERATIONS
 * ========================================================================= */

static distric_err_t write_log_header(int fd, logger_t* logger) {
    uint32_t magic = LOG_FILE_MAGIC;
    uint32_t version = LOG_FILE_VERSION;
    
    if (write(fd, &magic, 4) != 4 || write(fd, &version, 4) != 4) {
        LOG_ERROR(logger, "persistence", "Failed to write log header", NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    return DISTRIC_OK;
}

static distric_err_t read_log_header(int fd, logger_t* logger) {
    uint32_t magic, version;
    
    if (read(fd, &magic, 4) != 4 || read(fd, &version, 4) != 4) {
        /* Empty file - write header */
        lseek(fd, 0, SEEK_SET);
        return write_log_header(fd, logger);
    }
    
    if (magic != LOG_FILE_MAGIC) {
        LOG_ERROR(logger, "persistence", "Invalid log file magic", NULL);
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    if (version != LOG_FILE_VERSION) {
        LOG_ERROR(logger, "persistence", "Unsupported log file version", NULL);
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    return DISTRIC_OK;
}

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

distric_err_t raft_persistence_init(
    const raft_persistence_config_t* config,
    raft_persistence_t** persistence_out
) {
    if (!config || !config->data_dir || !persistence_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    raft_persistence_t* p = (raft_persistence_t*)calloc(1, sizeof(raft_persistence_t));
    if (!p) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    strncpy(p->data_dir, config->data_dir, sizeof(p->data_dir) - 1);
    p->logger = config->logger;
    p->log_fd = -1;
    
    /* Build file paths */
    snprintf(p->state_path, sizeof(p->state_path), "%s/state.json", p->data_dir);
    snprintf(p->state_tmp_path, sizeof(p->state_tmp_path), "%s/state.json.tmp", p->data_dir);
    snprintf(p->log_path, sizeof(p->log_path), "%s/log.dat", p->data_dir);
    snprintf(p->snapshot_path, sizeof(p->snapshot_path), "%s/snapshot.dat", p->data_dir);
    snprintf(p->snapshot_tmp_path, sizeof(p->snapshot_tmp_path), "%s/snapshot.dat.tmp", p->data_dir);
    
    /* Create directory */
    distric_err_t err = ensure_directory(p->data_dir, p->logger);
    if (err != DISTRIC_OK) {
        free(p);
        return err;
    }
    
    /* Open/create log file */
    p->log_fd = open(p->log_path, O_RDWR | O_CREAT, 0644);
    if (p->log_fd < 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(p->logger, "persistence", "Failed to open log file",
                 "path", p->log_path, "errno", errno_str, NULL);
        free(p);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Read/write log header */
    err = read_log_header(p->log_fd, p->logger);
    if (err != DISTRIC_OK) {
        close(p->log_fd);
        free(p);
        return err;
    }
    
    LOG_INFO(p->logger, "persistence", "Initialized",
            "data_dir", p->data_dir, NULL);
    
    *persistence_out = p;
    return DISTRIC_OK;
}

void raft_persistence_destroy(raft_persistence_t* persistence) {
    if (!persistence) return;
    
    if (persistence->log_fd >= 0) {
        close(persistence->log_fd);
    }
    
    free(persistence);
}

/* ============================================================================
 * STATE OPERATIONS
 * ========================================================================= */

distric_err_t raft_persistence_save_term(
    raft_persistence_t* persistence,
    uint32_t term
) {
    if (!persistence) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Load current voted_for to preserve it */
    uint32_t old_term;
    char voted_for[64];
    distric_err_t err = raft_persistence_load_state(persistence, &old_term, voted_for);
    if (err != DISTRIC_OK && err != DISTRIC_ERR_NOT_FOUND) {
        return err;
    }
    
    return raft_persistence_save_state(persistence, term, voted_for);
}

distric_err_t raft_persistence_save_vote(
    raft_persistence_t* persistence,
    const char* voted_for
) {
    if (!persistence) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Load current term to preserve it */
    uint32_t term;
    char old_voted_for[64];
    distric_err_t err = raft_persistence_load_state(persistence, &term, old_voted_for);
    if (err != DISTRIC_OK && err != DISTRIC_ERR_NOT_FOUND) {
        return err;
    }
    
    return raft_persistence_save_state(persistence, term, voted_for);
}

distric_err_t raft_persistence_save_state(
    raft_persistence_t* persistence,
    uint32_t term,
    const char* voted_for
) {
    if (!persistence) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Format state */
    char* content = NULL;
    size_t len = 0;
    distric_err_t err = format_state_file(term, voted_for, &content, &len);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Write atomically */
    err = write_file_atomic(persistence->state_path, persistence->state_tmp_path,
                           content, len, persistence->logger);
    free(content);
    
    if (err == DISTRIC_OK) {
        char term_str[32];
        snprintf(term_str, sizeof(term_str), "%u", term);
        LOG_DEBUG(persistence->logger, "persistence", "Saved state",
                 "term", term_str,
                 "voted_for", voted_for ? voted_for : "", NULL);
    }
    
    return err;
}

distric_err_t raft_persistence_load_state(
    raft_persistence_t* persistence,
    uint32_t* term_out,
    char* voted_for_out
) {
    if (!persistence || !term_out || !voted_for_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Read state file */
    char* content = NULL;
    size_t len = 0;
    distric_err_t err = read_file(persistence->state_path, &content, &len, persistence->logger);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Parse */
    if (content) {
        err = parse_state_file(content, term_out, voted_for_out);
        free(content);
    } else {
        *term_out = 0;
        voted_for_out[0] = '\0';
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    char term_str[32];
    snprintf(term_str, sizeof(term_str), "%u", *term_out);
    LOG_DEBUG(persistence->logger, "persistence", "Loaded state",
             "term", term_str,
             "voted_for", voted_for_out, NULL);
    
    return err;
}

/* ============================================================================
 * LOG OPERATIONS
 * ========================================================================= */

distric_err_t raft_persistence_append_log(
    raft_persistence_t* persistence,
    const raft_log_entry_t* entry
) {
    if (!persistence || !entry) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Seek to end */
    if (lseek(persistence->log_fd, 0, SEEK_END) < 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(persistence->logger, "persistence", "Failed to seek log", "errno", errno_str, NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Write record */
    uint32_t index = entry->index;
    uint32_t term = entry->term;
    uint8_t type = (uint8_t)entry->type;
    uint32_t data_len = entry->data_len;
    
    if (write(persistence->log_fd, &index, 4) != 4 ||
        write(persistence->log_fd, &term, 4) != 4 ||
        write(persistence->log_fd, &type, 1) != 1 ||
        write(persistence->log_fd, &data_len, 4) != 4) {
        LOG_ERROR(persistence->logger, "persistence", "Failed to write log entry header", NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    if (data_len > 0 && entry->data) {
        if (write(persistence->log_fd, entry->data, data_len) != (ssize_t)data_len) {
            LOG_ERROR(persistence->logger, "persistence", "Failed to write log entry data", NULL);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    /* Sync */
    if (fsync(persistence->log_fd) != 0) {
        LOG_ERROR(persistence->logger, "persistence", "Failed to fsync log", NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    char idx_str[32], term_str[32];
    snprintf(idx_str, sizeof(idx_str), "%u", index);
    snprintf(term_str, sizeof(term_str), "%u", term);
    LOG_DEBUG(persistence->logger, "persistence", "Appended log entry",
             "index", idx_str, "term", term_str, NULL);
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_load_log(
    raft_persistence_t* persistence,
    raft_log_entry_t** entries_out,
    size_t* count_out
) {
    if (!persistence || !entries_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    *entries_out = NULL;
    *count_out = 0;
    
    /* Seek past header */
    if (lseek(persistence->log_fd, 8, SEEK_SET) < 0) {
        LOG_ERROR(persistence->logger, "persistence", "Failed to seek log", NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Count entries first */
    size_t count = 0;
    while (1) {
        uint32_t index, term, data_len;
        uint8_t type;
        
        ssize_t r = read(persistence->log_fd, &index, 4);
        if (r == 0) break;  /* EOF */
        if (r != 4) goto read_error;
        
        if (read(persistence->log_fd, &term, 4) != 4 ||
            read(persistence->log_fd, &type, 1) != 1 ||
            read(persistence->log_fd, &data_len, 4) != 4) {
            goto read_error;
        }
        
        /* Skip data */
        if (data_len > 0) {
            if (lseek(persistence->log_fd, data_len, SEEK_CUR) < 0) {
                goto read_error;
            }
        }
        
        count++;
    }
    
    if (count == 0) {
        return DISTRIC_OK;  /* Empty log */
    }
    
    /* Allocate entries */
    raft_log_entry_t* entries = (raft_log_entry_t*)calloc(count, sizeof(raft_log_entry_t));
    if (!entries) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Seek back and read entries */
    if (lseek(persistence->log_fd, 8, SEEK_SET) < 0) {
        free(entries);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    for (size_t i = 0; i < count; i++) {
        uint32_t index, term, data_len;
        uint8_t type;
        
        if (read(persistence->log_fd, &index, 4) != 4 ||
            read(persistence->log_fd, &term, 4) != 4 ||
            read(persistence->log_fd, &type, 1) != 1 ||
            read(persistence->log_fd, &data_len, 4) != 4) {
            raft_persistence_free_log(entries, i);
            return DISTRIC_ERR_INIT_FAILED;
        }
        
        entries[i].index = index;
        entries[i].term = term;
        entries[i].type = (raft_entry_type_t)type;
        entries[i].data_len = data_len;
        
        if (data_len > 0) {
            entries[i].data = (uint8_t*)malloc(data_len);
            if (!entries[i].data) {
                raft_persistence_free_log(entries, i);
                return DISTRIC_ERR_NO_MEMORY;
            }
            
            if (read(persistence->log_fd, entries[i].data, data_len) != (ssize_t)data_len) {
                raft_persistence_free_log(entries, i + 1);
                return DISTRIC_ERR_INIT_FAILED;
            }
        } else {
            entries[i].data = NULL;
        }
    }
    
    *entries_out = entries;
    *count_out = count;
    
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%zu", count);
    LOG_INFO(persistence->logger, "persistence", "Loaded log",
            "entry_count", count_str, NULL);
    
    return DISTRIC_OK;

read_error:
    LOG_ERROR(persistence->logger, "persistence", "Failed to read log file", NULL);
    return DISTRIC_ERR_INIT_FAILED;
}

distric_err_t raft_persistence_truncate_log(
    raft_persistence_t* persistence,
    uint32_t from_index
) {
    if (!persistence) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Load log, filter, rewrite */
    raft_log_entry_t* entries = NULL;
    size_t count = 0;
    
    distric_err_t err = raft_persistence_load_log(persistence, &entries, &count);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Close current log */
    close(persistence->log_fd);
    
    /* Reopen and truncate */
    persistence->log_fd = open(persistence->log_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (persistence->log_fd < 0) {
        raft_persistence_free_log(entries, count);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Write header */
    err = write_log_header(persistence->log_fd, persistence->logger);
    if (err != DISTRIC_OK) {
        raft_persistence_free_log(entries, count);
        return err;
    }
    
    /* Rewrite entries before from_index */
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].index < from_index) {
            err = raft_persistence_append_log(persistence, &entries[i]);
            if (err != DISTRIC_OK) {
                raft_persistence_free_log(entries, count);
                return err;
            }
            kept++;
        }
    }
    
    raft_persistence_free_log(entries, count);
    
    char from_idx_str[32], kept_str[32];
    snprintf(from_idx_str, sizeof(from_idx_str), "%u", from_index);
    snprintf(kept_str, sizeof(kept_str), "%zu", kept);
    LOG_INFO(persistence->logger, "persistence", "Truncated log",
            "from_index", from_idx_str,
            "kept", kept_str, NULL);
    
    return DISTRIC_OK;
}

void raft_persistence_free_log(raft_log_entry_t* entries, size_t count) {
    if (!entries) return;
    
    for (size_t i = 0; i < count; i++) {
        free(entries[i].data);
    }
    
    free(entries);
}

/* ============================================================================
 * SNAPSHOT OPERATIONS (Session 3.5)
 * ========================================================================= */

distric_err_t raft_persistence_save_snapshot(
    raft_persistence_t* persistence,
    uint32_t last_included_index,
    uint32_t last_included_term,
    const uint8_t* snapshot_data,
    size_t snapshot_len
) {
    if (!persistence || !snapshot_data) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Open temporary snapshot file */
    int fd = open(persistence->snapshot_tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(persistence->logger, "persistence", "Failed to create snapshot tmp file",
                 "errno", errno_str, NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Compute CRC32 of snapshot data */
    uint32_t crc32 = crc32_compute(snapshot_data, snapshot_len);
    
    /* Write header */
    uint32_t magic = SNAPSHOT_MAGIC;
    uint32_t version = SNAPSHOT_VERSION;
    uint32_t data_len = (uint32_t)snapshot_len;
    
    if (write(fd, &magic, 4) != 4 ||
        write(fd, &version, 4) != 4 ||
        write(fd, &last_included_index, 4) != 4 ||
        write(fd, &last_included_term, 4) != 4 ||
        write(fd, &data_len, 4) != 4 ||
        write(fd, &crc32, 4) != 4) {
        LOG_ERROR(persistence->logger, "persistence", "Failed to write snapshot header", NULL);
        close(fd);
        unlink(persistence->snapshot_tmp_path);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Write snapshot data */
    ssize_t written = write(fd, snapshot_data, snapshot_len);
    if (written != (ssize_t)snapshot_len) {
        char expected_str[32], written_str[32];
        snprintf(expected_str, sizeof(expected_str), "%zu", snapshot_len);
        snprintf(written_str, sizeof(written_str), "%zd", written);
        LOG_ERROR(persistence->logger, "persistence", "Failed to write snapshot data",
                 "expected", expected_str, "written", written_str, NULL);
        close(fd);
        unlink(persistence->snapshot_tmp_path);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Sync to disk */
    if (fsync(fd) != 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(persistence->logger, "persistence", "Failed to fsync snapshot", "errno", errno_str, NULL);
        close(fd);
        unlink(persistence->snapshot_tmp_path);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    close(fd);
    
    /* Atomic rename */
    if (rename(persistence->snapshot_tmp_path, persistence->snapshot_path) != 0) {
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(persistence->logger, "persistence", "Failed to rename snapshot", "errno", errno_str, NULL);
        unlink(persistence->snapshot_tmp_path);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    char idx_str[32], term_str[32], size_str[32];
    snprintf(idx_str, sizeof(idx_str), "%u", last_included_index);
    snprintf(term_str, sizeof(term_str), "%u", last_included_term);
    snprintf(size_str, sizeof(size_str), "%u", (uint32_t)snapshot_len);
    LOG_INFO(persistence->logger, "persistence", "Saved snapshot",
            "last_included_index", idx_str,
            "last_included_term", term_str,
            "size", size_str, NULL);
    
    return DISTRIC_OK;
}

distric_err_t raft_persistence_load_snapshot(
    raft_persistence_t* persistence,
    uint32_t* last_included_index_out,
    uint32_t* last_included_term_out,
    uint8_t** snapshot_data_out,
    size_t* snapshot_len_out
) {
    if (!persistence || !last_included_index_out || !last_included_term_out ||
        !snapshot_data_out || !snapshot_len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Open snapshot file */
    int fd = open(persistence->snapshot_path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return DISTRIC_ERR_NOT_FOUND;  /* No snapshot exists */
        }
        char errno_str[32];
        snprintf(errno_str, sizeof(errno_str), "%d", errno);
        LOG_ERROR(persistence->logger, "persistence", "Failed to open snapshot", "errno", errno_str, NULL);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Read header */
    uint32_t magic, version, last_included_index, last_included_term, data_len, stored_crc32;
    
    if (read(fd, &magic, 4) != 4 ||
        read(fd, &version, 4) != 4 ||
        read(fd, &last_included_index, 4) != 4 ||
        read(fd, &last_included_term, 4) != 4 ||
        read(fd, &data_len, 4) != 4 ||
        read(fd, &stored_crc32, 4) != 4) {
        LOG_ERROR(persistence->logger, "persistence", "Failed to read snapshot header", NULL);
        close(fd);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Validate header */
    if (magic != SNAPSHOT_MAGIC) {
        LOG_ERROR(persistence->logger, "persistence", "Invalid snapshot magic", NULL);
        close(fd);
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    if (version != SNAPSHOT_VERSION) {
        LOG_ERROR(persistence->logger, "persistence", "Unsupported snapshot version", NULL);
        close(fd);
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    /* Allocate buffer for snapshot data */
    uint8_t* data = (uint8_t*)malloc(data_len);
    if (!data) {
        close(fd);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Read snapshot data */
    ssize_t bytes_read = read(fd, data, data_len);
    close(fd);
    
    if (bytes_read != (ssize_t)data_len) {
        LOG_ERROR(persistence->logger, "persistence", "Failed to read snapshot data", NULL);
        free(data);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Verify CRC32 */
    uint32_t computed_crc32 = crc32_compute(data, data_len);
    if (computed_crc32 != stored_crc32) {
        char expected_str[32], computed_str[32];
        snprintf(expected_str, sizeof(expected_str), "%u", stored_crc32);
        snprintf(computed_str, sizeof(computed_str), "%u", computed_crc32);
        LOG_ERROR(persistence->logger, "persistence", "Snapshot CRC32 mismatch",
                 "expected", expected_str,
                 "computed", computed_str, NULL);
        free(data);
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    *last_included_index_out = last_included_index;
    *last_included_term_out = last_included_term;
    *snapshot_data_out = data;
    *snapshot_len_out = data_len;
    
    char idx_str[32], term_str[32], size_str[32];
    snprintf(idx_str, sizeof(idx_str), "%u", last_included_index);
    snprintf(term_str, sizeof(term_str), "%u", last_included_term);
    snprintf(size_str, sizeof(size_str), "%u", data_len);
    LOG_INFO(persistence->logger, "persistence", "Loaded snapshot",
            "last_included_index", idx_str,
            "last_included_term", term_str,
            "size", size_str, NULL);
    
    return DISTRIC_OK;
}