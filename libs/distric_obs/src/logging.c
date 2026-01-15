#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs/logging.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

/* Thread-local buffer for log formatting */
static __thread char tls_buffer[LOG_BUFFER_SIZE];

/* Convert log level to string */
static const char* log_level_str(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO: return "INFO";
        case LOG_LEVEL_WARN: return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

/* Get current timestamp in milliseconds */
static uint64_t get_timestamp_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* Escape JSON string */
static void json_escape(const char* src, char* dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j < dst_size - 3) dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = 'n';
            }
        } else if (c == '\r') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = 'r';
            }
        } else if (c == '\t') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = 't';
            }
        } else if ((unsigned char)c < 32) {
            /* Skip control characters */
            continue;
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

/* Background thread for async logging */
static void* flush_thread_fn(void* arg) {
    logger_t* logger = (logger_t*)arg;
    ring_buffer_t* rb = logger->ring_buffer;
    
    while (atomic_load(&rb->running) || 
           atomic_load(&rb->read_pos) != atomic_load(&rb->write_pos)) {
        
        size_t read_pos = atomic_load(&rb->read_pos);
        size_t write_pos = atomic_load(&rb->write_pos);
        
        if (read_pos == write_pos) {
            /* Buffer empty, sleep briefly */
            usleep(1000); /* 1ms */
            continue;
        }
        
        /* Get entry pointer */
        log_entry_t* entry = &rb->entries[read_pos & RING_BUFFER_MASK];
        
        /* FIX #2: Wait for writer to commit the entry */
        /* Spin-wait with backoff for the entry to be marked ready */
        int spin_count = 0;
        while (!atomic_load(&entry->ready)) {
            if (spin_count < 100) {
                /* Tight spin for first 100 iterations */
                __asm__ __volatile__("pause" ::: "memory");
                spin_count++;
            } else {
                /* Yield after spinning */
                usleep(1);
            }
            
            /* Double-check we haven't been shut down */
            if (!atomic_load(&rb->running) && 
                atomic_load(&rb->read_pos) == atomic_load(&rb->write_pos)) {
                return NULL;
            }
        }
        
        /* Write to file descriptor */
        write(logger->fd, entry->data, entry->length);
        
        /* Mark entry as consumed (allow reuse) */
        atomic_store(&entry->ready, false);
        
        /* Advance read position */
        atomic_store(&rb->read_pos, read_pos + 1);
    }
    
    return NULL;
}

/* Initialize logger with output file descriptor and mode */
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode) {
    if (!logger || fd < 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    logger_t* log = calloc(1, sizeof(logger_t));
    if (!log) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    log->fd = fd;
    log->mode = mode;
    atomic_init(&log->shutdown, false);
    
    if (mode == LOG_MODE_ASYNC) {
        /* Allocate ring buffer */
        log->ring_buffer = calloc(1, sizeof(ring_buffer_t));
        if (!log->ring_buffer) {
            free(log);
            return DISTRIC_ERR_ALLOC_FAILURE;
        }
        
        atomic_init(&log->ring_buffer->write_pos, 0);
        atomic_init(&log->ring_buffer->read_pos, 0);
        atomic_init(&log->ring_buffer->running, true);
        
        /* Initialize all entries as not ready */
        for (size_t i = 0; i < RING_BUFFER_SIZE; i++) {
            atomic_init(&log->ring_buffer->entries[i].ready, false);
        }
        
        /* Start flush thread */
        if (pthread_create(&log->flush_thread, NULL, flush_thread_fn, log) != 0) {
            free(log->ring_buffer);
            free(log);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    *logger = log;
    return DISTRIC_OK;
}

/* Destroy logger and flush all pending logs */
void log_destroy(logger_t* logger) {
    if (!logger) {
        return;
    }
    
    if (logger->mode == LOG_MODE_ASYNC && logger->ring_buffer) {
        /* Signal shutdown */
        atomic_store(&logger->ring_buffer->running, false);
        
        /* Wait for flush thread to finish */
        pthread_join(logger->flush_thread, NULL);
        
        free(logger->ring_buffer);
    }
    
    free(logger);
}

/* Write a log entry with key-value pairs (NULL-terminated) */
distric_err_t log_write(
    logger_t* logger,
    log_level_t level,
    const char* component,
    const char* message,
    ...
) {
    if (!logger || !component || !message) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Use thread-local buffer */
    char* buffer = tls_buffer;
    size_t offset = 0;
    size_t remaining = LOG_BUFFER_SIZE;
    
    /* Start JSON object */
    int written = snprintf(buffer + offset, remaining, "{");
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;
    
    /* Timestamp */
    uint64_t ts = get_timestamp_ms();
    written = snprintf(buffer + offset, remaining, "\"timestamp\":%lu,", ts);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;
    
    /* Level */
    written = snprintf(buffer + offset, remaining, "\"level\":\"%s\",",
                      log_level_str(level));
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;
    
    /* Component */
    char escaped_component[256];
    json_escape(component, escaped_component, sizeof(escaped_component));
    written = snprintf(buffer + offset, remaining, "\"component\":\"%s\",",
                      escaped_component);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;
    
    /* Message */
    char escaped_message[1024];
    json_escape(message, escaped_message, sizeof(escaped_message));
    written = snprintf(buffer + offset, remaining, "\"message\":\"%s\"",
                      escaped_message);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;
    
    /* Parse key-value pairs */
    va_list args;
    va_start(args, message);
    
    while (1) {
        const char* key = va_arg(args, const char*);
        if (!key) break;
        
        const char* value = va_arg(args, const char*);
        if (!value) break;
        
        char escaped_key[128];
        char escaped_value[512];
        json_escape(key, escaped_key, sizeof(escaped_key));
        json_escape(value, escaped_value, sizeof(escaped_value));
        
        written = snprintf(buffer + offset, remaining, ",\"%s\":\"%s\"",
                          escaped_key, escaped_value);
        if (written < 0 || (size_t)written >= remaining) {
            va_end(args);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
        remaining -= written;
    }
    
    va_end(args);
    
    /* Close JSON object */
    written = snprintf(buffer + offset, remaining, "}\n");
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    
    /* Write to output */
    if (logger->mode == LOG_MODE_SYNC) {
        /* Direct write to file descriptor */
        ssize_t result = write(logger->fd, buffer, offset);
        if (result < 0 || (size_t)result != offset) {
            return DISTRIC_ERR_INIT_FAILED;
        }
    } else {
        /* Write to ring buffer */
        ring_buffer_t* rb = logger->ring_buffer;
        
        /* FIX #1: Atomically reserve a slot */
        size_t write_pos = atomic_fetch_add(&rb->write_pos, 1);
        size_t read_pos = atomic_load(&rb->read_pos);
        
        /* FIX #6: Check if buffer is full AFTER reservation */
        if (write_pos - read_pos >= RING_BUFFER_SIZE) {
            /* Buffer full - this creates a "hole" but prevents deadlock */
            /* The flush thread will spin-wait on the ready flag forever if we don't handle this */
            /* We need to mark this slot as skipped */
            log_entry_t* entry = &rb->entries[write_pos & RING_BUFFER_MASK];
            entry->length = 0; /* Mark as empty/skipped */
            atomic_store(&entry->ready, true); /* Let flush thread skip it */
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        
        /* Copy to ring buffer entry */
        log_entry_t* entry = &rb->entries[write_pos & RING_BUFFER_MASK];
        
        /* Ensure ready flag is false before writing (should already be from previous use) */
        atomic_store(&entry->ready, false);
        
        /* Copy data */
        memcpy(entry->data, buffer, offset);
        entry->length = offset;
        
        /* FIX #2: Commit the entry - signal to flush thread that data is ready */
        atomic_store(&entry->ready, true);
    }
    
    return DISTRIC_OK;
}