/* Feature test macros must come before any includes */
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
#include <errno.h>

/* Thread-local buffer for log formatting */
static __thread char tls_buffer[LOG_BUFFER_SIZE];

/* Architecture-specific pause instruction */
#if defined(__x86_64__) || defined(__i386__)
    #define CPU_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
    #define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define CPU_PAUSE() do { } while(0)
#endif

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
static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* Escape JSON string - optimized version */
static void json_escape(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    
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

/* Background thread for async logging - FIXED */
static void* flush_thread_fn(void* arg) {
    logger_t* logger = (logger_t*)arg;
    ring_buffer_t* rb = logger->ring_buffer;
    
    while (atomic_load_explicit(&rb->running, memory_order_acquire) || 
           atomic_load_explicit(&rb->read_pos, memory_order_acquire) != 
           atomic_load_explicit(&rb->write_pos, memory_order_acquire)) {
        
        size_t read_pos = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        size_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        
        if (read_pos == write_pos) {
            usleep(100);
            continue;
        }

        log_entry_t* entry = &rb->entries[read_pos & RING_BUFFER_MASK];
        
        /* CRITICAL FIX: Reduced timeout and proper shutdown handling */
        int spin_count = 0;
        const int MAX_SPIN = 1000;  /* ~1ms max wait */
        
        while (!atomic_load_explicit(&entry->ready, memory_order_acquire)) {
            if (spin_count < 100) {
                CPU_PAUSE();
                spin_count++;
            } else if (spin_count < MAX_SPIN) {
                usleep(1);
                spin_count++;
            } else {
                /* Timeout - entry likely abandoned due to overflow or shutdown */
                if (!atomic_load_explicit(&rb->running, memory_order_acquire)) {
                    /* Shutting down - skip this entry */
                    atomic_store_explicit(&rb->read_pos, read_pos + 1, memory_order_release);
                    break;
                }
                /* Running but stuck - skip and continue */
                atomic_store_explicit(&rb->read_pos, read_pos + 1, memory_order_release);
                break;
            }
        }
        
        /* Only write if entry was marked ready */
        if (atomic_load_explicit(&entry->ready, memory_order_acquire)) {
            if (entry->length > 0) {
                ssize_t written = 0;
                ssize_t total = 0;
                
                /* Handle partial writes */
                while (total < (ssize_t)entry->length) {
                    written = write(logger->fd, entry->data + total, 
                                   entry->length - total);
                    if (written < 0) {
                        if (errno == EINTR) continue;
                        break;  /* Error - drop this log */
                    }
                    total += written;
                }
            }
            
            /* Mark entry as consumed */
            atomic_store_explicit(&entry->ready, false, memory_order_release);
        }
        
        /* Advance read position */
        atomic_store_explicit(&rb->read_pos, read_pos + 1, memory_order_release);
    }
    
    return NULL;
}

/* Initialize logger with output file descriptor and mode */
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode) {
    if (!logger) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (fd < 0) {
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
            log->ring_buffer->entries[i].length = 0;
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
        /* Signal shutdown with proper memory ordering */
        atomic_store_explicit(&logger->ring_buffer->running, false, memory_order_release);
        
        /* Give flush thread time to process remaining entries */
        usleep(50000);  /* 50ms */
        
        /* Wait for flush thread to finish */
        pthread_join(logger->flush_thread, NULL);
        
        free(logger->ring_buffer);
    }
    
    free(logger);
}

/* Write a log entry with key-value pairs (NULL-terminated) */
distric_err_t log_write(logger_t* logger, log_level_t level, 
                       const char* component, const char* message, ...) {
    if (!logger || !component || !message) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    /* Use thread-local buffer */
    size_t remaining = LOG_BUFFER_SIZE;
    size_t offset = 0;
    int written;

    /* Start JSON object */
    written = snprintf(tls_buffer + offset, remaining, "{");
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;

    /* Timestamp */
    uint64_t ts = get_timestamp_ms();
    written = snprintf(tls_buffer + offset, remaining, "\"timestamp\":%lu,", ts);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;

    /* Level and Component - no need to escape level (controlled) */
    char escaped_component[256];
    json_escape(component, escaped_component, sizeof(escaped_component));
    
    written = snprintf(tls_buffer + offset, remaining, 
                      "\"level\":\"%s\",\"component\":\"%s\",", 
                      log_level_str(level), escaped_component);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;

    /* Message */
    char escaped_message[1024];
    json_escape(message, escaped_message, sizeof(escaped_message));
    written = snprintf(tls_buffer + offset, remaining, 
                      "\"message\":\"%s\"", escaped_message);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;

    /* Varargs: Key-Value Pairs */
    va_list args;
    va_start(args, message);
    const char* key;
    while ((key = va_arg(args, const char*)) != NULL) {
        const char* val = va_arg(args, const char*);
        if (!val) break;
        
        char escaped_key[128];
        char escaped_val[512];
        json_escape(key, escaped_key, sizeof(escaped_key));
        json_escape(val, escaped_val, sizeof(escaped_val));
        
        written = snprintf(tls_buffer + offset, remaining, 
                          ",\"%s\":\"%s\"", escaped_key, escaped_val);
        if (written < 0 || (size_t)written >= remaining) {
            break;  /* Truncate extra fields if buffer full */
        }
        offset += written;
        remaining -= written;
    }
    va_end(args);

    /* End JSON object */
    written = snprintf(tls_buffer + offset, remaining, "}\n");
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;

    if (logger->mode == LOG_MODE_SYNC) {
        /* Synchronous write with error handling */
        ssize_t total = 0;
        while (total < (ssize_t)offset) {
            ssize_t n = write(logger->fd, tls_buffer + total, offset - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                return DISTRIC_ERR_INIT_FAILED;  /* Write error */
            }
            total += n;
        }
        return DISTRIC_OK;
    } else {
        /* Async mode - ring buffer */
        ring_buffer_t* rb = logger->ring_buffer;
        
        /* CRITICAL FIX: Check overflow BEFORE incrementing */
        size_t current_write = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        size_t current_read = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        
        if (current_write - current_read >= RING_BUFFER_SIZE) {
            /* Buffer full - drop this log */
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        
        /* Reserve slot */
        size_t write_pos = atomic_fetch_add_explicit(&rb->write_pos, 1, 
                                                     memory_order_acq_rel);
        log_entry_t* entry = &rb->entries[write_pos & RING_BUFFER_MASK];

        /* Double-check after reservation */
        if (write_pos - current_read >= RING_BUFFER_SIZE) {
            /* Still overflowed - mark as ready with zero length to unblock reader */
            entry->length = 0;
            atomic_store_explicit(&entry->ready, true, memory_order_release);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }

        /* Write data to entry - ready flag prevents concurrent reads */
        atomic_store_explicit(&entry->ready, false, memory_order_release);
        
        /* Memory fence before writing data */
        atomic_thread_fence(memory_order_acquire);
        
        memcpy(entry->data, tls_buffer, offset);
        entry->length = offset;
        
        /* Memory fence after writing data */
        atomic_thread_fence(memory_order_release);
        
        /* CRITICAL: Mark as ready LAST */
        atomic_store_explicit(&entry->ready, true, memory_order_release);
        
        return DISTRIC_OK;
    }
}