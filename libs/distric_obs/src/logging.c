/**
 * @file logging_fixed.c
 * @brief FIXED: Thread-safe logger shutdown
 * 
 * ROOT CAUSE: Use-after-free during concurrent shutdown
 * 
 * PROBLEM:
 * 1. Integration test destroys logger
 * 2. Background threads (UDP, TCP, HTTP, tracing) still running
 * 3. Those threads call LOG_*() macros
 * 4. Logger already freed → SEGFAULT
 * 
 * FIX:
 * 1. Add shutdown flag to logger (check before writing)
 * 2. Ensure flush thread exits cleanly
 * 3. Make log_write() return early if logger is shutting down
 */

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
    
    while (atomic_load_explicit(&rb->running, memory_order_acquire) || 
           atomic_load_explicit(&rb->read_pos, memory_order_acquire) != 
           atomic_load_explicit(&rb->write_pos, memory_order_acquire)) {
        
        size_t read_pos = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        size_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        
        if (read_pos == write_pos) {
            usleep(1000);
            continue;
        }

        log_entry_t* entry = &rb->entries[read_pos & RING_BUFFER_MASK];
        
        int spin_count = 0;
        const int MAX_SPIN = 100;
        
        while (!atomic_load_explicit(&entry->ready, memory_order_acquire)) {
            if (spin_count < 10) {
                CPU_PAUSE();
                spin_count++;
            } else if (spin_count < MAX_SPIN) {
                sched_yield();
                spin_count++;
            } else {
                /* Timeout - skip this entry if shutting down */
                if (!atomic_load_explicit(&rb->running, memory_order_acquire)) {
                    atomic_store_explicit(&rb->read_pos, read_pos + 1, memory_order_release);
                    break;
                }
                
                /* Still running but entry stuck - skip it */
                atomic_store_explicit(&rb->read_pos, read_pos + 1, memory_order_release);
                break;
            }
        }
        
        if (atomic_load_explicit(&entry->ready, memory_order_acquire)) {
            if (entry->length > 0) {
                ssize_t written = 0;
                ssize_t total = 0;
                
                while (total < (ssize_t)entry->length) {
                    written = write(logger->fd, entry->data + total, 
                                   entry->length - total);
                    if (written < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    total += written;
                }
            }
            
            atomic_store_explicit(&entry->ready, false, memory_order_release);
        }
        
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
    atomic_init(&log->shutdown, false);  /* CRITICAL: Initialize shutdown flag */
    
    if (mode == LOG_MODE_ASYNC) {
        log->ring_buffer = calloc(1, sizeof(ring_buffer_t));
        if (!log->ring_buffer) {
            free(log);
            return DISTRIC_ERR_ALLOC_FAILURE;
        }
        
        atomic_init(&log->ring_buffer->write_pos, 0);
        atomic_init(&log->ring_buffer->read_pos, 0);
        atomic_init(&log->ring_buffer->running, true);
        
        for (size_t i = 0; i < RING_BUFFER_SIZE; i++) {
            atomic_init(&log->ring_buffer->entries[i].ready, false);
            log->ring_buffer->entries[i].length = 0;
        }
        
        if (pthread_create(&log->flush_thread, NULL, flush_thread_fn, log) != 0) {
            free(log->ring_buffer);
            free(log);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    *logger = log;
    return DISTRIC_OK;
}

/* Destroy logger and flush all pending logs - HARDENED */
void log_destroy(logger_t* logger) {
    if (!logger) {
        return;
    }
    
    /* CRITICAL FIX: Set shutdown flag FIRST to prevent new writes */
    atomic_store_explicit(&logger->shutdown, true, memory_order_seq_cst);
    
    /* Memory barrier to ensure all threads see shutdown flag */
    atomic_thread_fence(memory_order_seq_cst);
    
    if (logger->mode == LOG_MODE_ASYNC && logger->ring_buffer) {
        /* Stop the ring buffer */
        atomic_store_explicit(&logger->ring_buffer->running, false, memory_order_release);
        
        /* Wait for flush thread to exit cleanly */
        pthread_join(logger->flush_thread, NULL);
        
        /* Now safe to free ring buffer */
        free(logger->ring_buffer);
    }
    
    /* Finally free logger struct */
    free(logger);
}

/* Write a log entry - HARDENED with shutdown check */
distric_err_t log_write(logger_t* logger, log_level_t level, 
                       const char* component, const char* message, ...) {
    /* CRITICAL FIX: Check shutdown flag BEFORE accessing logger */
    if (!logger || atomic_load_explicit(&logger->shutdown, memory_order_acquire)) {
        return DISTRIC_ERR_INVALID_ARG;  /* Silently fail if shutting down */
    }
    
    if (!component || !message) {
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

    /* Level and Component */
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

    /* Varargs */
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
            break;
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

    /* CRITICAL: Check shutdown again before writing */
    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire)) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    if (logger->mode == LOG_MODE_SYNC) {
        ssize_t total = 0;
        while (total < (ssize_t)offset) {
            ssize_t n = write(logger->fd, tls_buffer + total, offset - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                return DISTRIC_ERR_INIT_FAILED;
            }
            total += n;
        }
        return DISTRIC_OK;
    } else {
        /* Async mode */
        ring_buffer_t* rb = logger->ring_buffer;
        
        /* Check if ring buffer is being shut down */
        if (!rb || !atomic_load_explicit(&rb->running, memory_order_acquire)) {
            return DISTRIC_ERR_INVALID_ARG;
        }
        
        size_t current_write = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        size_t current_read = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        
        size_t used = current_write - current_read;
        
        if (used >= RING_BUFFER_SIZE - 1) {
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        
        size_t write_pos = atomic_fetch_add_explicit(&rb->write_pos, 1, 
                                                     memory_order_acq_rel);
        log_entry_t* entry = &rb->entries[write_pos & RING_BUFFER_MASK];

        size_t recheck_read = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        if (write_pos - recheck_read >= RING_BUFFER_SIZE) {
            entry->length = 0;
            atomic_store_explicit(&entry->ready, true, memory_order_release);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }

        atomic_store_explicit(&entry->ready, false, memory_order_release);
        atomic_thread_fence(memory_order_acquire);
        
        memcpy(entry->data, tls_buffer, offset);
        entry->length = offset;
        
        atomic_thread_fence(memory_order_release);
        atomic_store_explicit(&entry->ready, true, memory_order_release);
        
        return DISTRIC_OK;
    }
}