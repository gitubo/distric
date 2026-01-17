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

/* INSTRUMENTATION: Debug logging to stderr (avoid recursion) */
#define DEBUG_LOG(fmt, ...) \
    fprintf(stderr, "[LOGGING_DEBUG] " fmt "\n", ##__VA_ARGS__)

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

/* Background thread for async logging - INSTRUMENTED */
static void* flush_thread_fn(void* arg) {
    logger_t* logger = (logger_t*)arg;
    ring_buffer_t* rb = logger->ring_buffer;
    
    DEBUG_LOG("Flush thread started, ring_buffer=%p", (void*)rb);
    
    size_t iterations = 0;
    size_t logs_flushed = 0;
    size_t timeouts = 0;
    
    while (atomic_load_explicit(&rb->running, memory_order_acquire) || 
           atomic_load_explicit(&rb->read_pos, memory_order_acquire) != 
           atomic_load_explicit(&rb->write_pos, memory_order_acquire)) {
        
        iterations++;
        
        size_t read_pos = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        size_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        
        if (read_pos == write_pos) {
            if (iterations % 1000 == 0) {
                DEBUG_LOG("Flush thread idle, iterations=%zu, read=%zu, write=%zu", 
                         iterations, read_pos, write_pos);
            }
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
                /* Timeout */
                timeouts++;
                
                DEBUG_LOG("Entry timeout at read_pos=%zu, ready=%d, running=%d, timeouts=%zu",
                         read_pos, 
                         atomic_load(&entry->ready),
                         atomic_load(&rb->running),
                         timeouts);
                
                if (!atomic_load_explicit(&rb->running, memory_order_acquire)) {
                    DEBUG_LOG("Shutting down, skipping entry");
                    atomic_store_explicit(&rb->read_pos, read_pos + 1, memory_order_release);
                    break;
                }
                
                DEBUG_LOG("Running but entry stuck, skipping");
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
                        DEBUG_LOG("Write error: %s", strerror(errno));
                        break;
                    }
                    total += written;
                }
                
                logs_flushed++;
                
                if (logs_flushed % 1000 == 0) {
                    DEBUG_LOG("Progress: flushed=%zu, timeouts=%zu, read=%zu, write=%zu",
                             logs_flushed, timeouts, read_pos, write_pos);
                }
            }
            
            atomic_store_explicit(&entry->ready, false, memory_order_release);
        }
        
        atomic_store_explicit(&rb->read_pos, read_pos + 1, memory_order_release);
    }
    
    DEBUG_LOG("Flush thread exiting, logs_flushed=%zu, timeouts=%zu, iterations=%zu",
             logs_flushed, timeouts, iterations);
    
    return NULL;
}

/* Initialize logger with output file descriptor and mode */
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode) {
    DEBUG_LOG("log_init called, fd=%d, mode=%s", fd, mode == LOG_MODE_ASYNC ? "ASYNC" : "SYNC");
    
    if (!logger) {
        DEBUG_LOG("log_init failed: logger is NULL");
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (fd < 0) {
        DEBUG_LOG("log_init failed: invalid fd=%d", fd);
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    logger_t* log = calloc(1, sizeof(logger_t));
    if (!log) {
        DEBUG_LOG("log_init failed: calloc returned NULL");
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    log->fd = fd;
    log->mode = mode;
    atomic_init(&log->shutdown, false);
    
    if (mode == LOG_MODE_ASYNC) {
        DEBUG_LOG("Initializing async mode...");
        
        log->ring_buffer = calloc(1, sizeof(ring_buffer_t));
        if (!log->ring_buffer) {
            DEBUG_LOG("Failed to allocate ring buffer");
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
        
        DEBUG_LOG("Starting flush thread...");
        
        if (pthread_create(&log->flush_thread, NULL, flush_thread_fn, log) != 0) {
            DEBUG_LOG("Failed to create flush thread: %s", strerror(errno));
            free(log->ring_buffer);
            free(log);
            return DISTRIC_ERR_INIT_FAILED;
        }
        
        DEBUG_LOG("Flush thread started successfully");
    }
    
    *logger = log;
    DEBUG_LOG("log_init complete, logger=%p", (void*)log);
    return DISTRIC_OK;
}

/* Destroy logger and flush all pending logs - INSTRUMENTED */
void log_destroy(logger_t* logger) {
    DEBUG_LOG("log_destroy called, logger=%p", (void*)logger);
    
    if (!logger) {
        DEBUG_LOG("log_destroy: logger is NULL, returning");
        return;
    }
    
    if (logger->mode == LOG_MODE_ASYNC && logger->ring_buffer) {
        DEBUG_LOG("Shutting down async logger...");
        
        size_t write_pos = atomic_load_explicit(&logger->ring_buffer->write_pos, memory_order_acquire);
        size_t read_pos = atomic_load_explicit(&logger->ring_buffer->read_pos, memory_order_acquire);
        size_t remaining = write_pos - read_pos;
        
        DEBUG_LOG("Before shutdown: write_pos=%zu, read_pos=%zu, remaining=%zu",
                 write_pos, read_pos, remaining);
        
        atomic_store_explicit(&logger->ring_buffer->running, false, memory_order_release);
        DEBUG_LOG("Set running=false");
        
        if (remaining > 0) {
            DEBUG_LOG("Waiting for %zu remaining entries to flush...", remaining);
            usleep(100000);  // 100ms
        }
        
        DEBUG_LOG("Joining flush thread...");
        pthread_join(logger->flush_thread, NULL);
        DEBUG_LOG("Flush thread joined successfully");
        
        write_pos = atomic_load(&logger->ring_buffer->write_pos);
        read_pos = atomic_load(&logger->ring_buffer->read_pos);
        DEBUG_LOG("After flush: write_pos=%zu, read_pos=%zu, diff=%zu",
                 write_pos, read_pos, write_pos - read_pos);
        
        free(logger->ring_buffer);
        DEBUG_LOG("Ring buffer freed");
    }
    
    free(logger);
    DEBUG_LOG("log_destroy complete");
}

/* Write a log entry - INSTRUMENTED */
distric_err_t log_write(logger_t* logger, log_level_t level, 
                       const char* component, const char* message, ...) {
    static _Atomic size_t total_writes = 0;
    static _Atomic size_t total_overflows = 0;
    
    size_t write_count = atomic_fetch_add(&total_writes, 1) + 1;
    
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
        
        size_t current_write = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        size_t current_read = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        
        size_t used = current_write - current_read;
        
        if (used >= RING_BUFFER_SIZE - 1) {
            size_t overflow_count = atomic_fetch_add(&total_overflows, 1) + 1;
            
            if (overflow_count % 100 == 0) {
                DEBUG_LOG("OVERFLOW #%zu at write_count=%zu, used=%zu, write=%zu, read=%zu",
                         overflow_count, write_count, used, current_write, current_read);
            }
            
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        
        size_t write_pos = atomic_fetch_add_explicit(&rb->write_pos, 1, 
                                                     memory_order_acq_rel);
        log_entry_t* entry = &rb->entries[write_pos & RING_BUFFER_MASK];

        size_t recheck_read = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        if (write_pos - recheck_read >= RING_BUFFER_SIZE) {
            DEBUG_LOG("Post-reservation overflow at write=%zu, read=%zu", write_pos, recheck_read);
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