#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

#define LOG_BUFFER_SIZE 4096
#define RING_BUFFER_SIZE 16384
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

#define LOG_SLOT_EMPTY 0
#define LOG_SLOT_FILLED 1
#define LOG_SLOT_PROCESSING 2

typedef struct {
    _Atomic uint32_t state;
    log_level_t level;
    uint64_t timestamp_ms;
    char message[512];
} log_entry_t;

typedef struct {
    log_entry_t entries[RING_BUFFER_SIZE];
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    _Atomic bool running;
} ring_buffer_t;

struct logger_s {
    int fd;
    log_mode_t mode;
    ring_buffer_t* ring_buffer;
    pthread_t flush_thread;
    _Atomic uint32_t refcount;
    _Atomic bool shutdown;
    
    _Atomic uint64_t messages_logged;
    _Atomic uint64_t messages_dropped;
    _Atomic uint64_t drops_by_level[5];
};

static __thread char tls_buffer[LOG_BUFFER_SIZE];

#if defined(__x86_64__) || defined(__i386__)
    #define CPU_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
    #define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define CPU_PAUSE() do { } while(0)
#endif

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

static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

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

static void* flush_thread_fn(void* arg) {
    logger_t* logger = (logger_t*)arg;
    ring_buffer_t* rb = logger->ring_buffer;
    
    while (atomic_load_explicit(&rb->running, memory_order_acquire) || 
           atomic_load_explicit(&rb->tail, memory_order_acquire) != 
           atomic_load_explicit(&rb->head, memory_order_acquire)) {
        
        uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
        
        if (tail == head) {
            usleep(1000);
            continue;
        }

        log_entry_t* entry = &rb->entries[tail & RING_BUFFER_MASK];
        
        int spin_count = 0;
        const int MAX_SPIN = 100;
        
        while (!atomic_load_explicit(&entry->state, memory_order_acquire) && spin_count < MAX_SPIN) {
            CPU_PAUSE();
            spin_count++;
        }
        
        if (atomic_load_explicit(&entry->state, memory_order_acquire) == LOG_SLOT_FILLED) {
            if (strlen(entry->message) > 0) {
                ssize_t written = 0;
                ssize_t total = 0;
                size_t len = strlen(entry->message);
                
                while (total < (ssize_t)len) {
                    written = write(logger->fd, entry->message + total, len - total);
                    if (written < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    total += written;
                }
            }
            
            atomic_store_explicit(&entry->state, LOG_SLOT_EMPTY, memory_order_release);
        }
        
        atomic_store_explicit(&rb->tail, tail + 1, memory_order_release);
    }
    
    return NULL;
}

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
    atomic_init(&log->refcount, 1);
    atomic_init(&log->shutdown, false);
    atomic_init(&log->messages_logged, 0);
    atomic_init(&log->messages_dropped, 0);
    for (int i = 0; i < 5; i++) {
        atomic_init(&log->drops_by_level[i], 0);
    }
    
    if (mode == LOG_MODE_ASYNC) {
        log->ring_buffer = calloc(1, sizeof(ring_buffer_t));
        if (!log->ring_buffer) {
            free(log);
            return DISTRIC_ERR_ALLOC_FAILURE;
        }
        
        atomic_init(&log->ring_buffer->head, 0);
        atomic_init(&log->ring_buffer->tail, 0);
        atomic_init(&log->ring_buffer->running, true);
        
        for (size_t i = 0; i < RING_BUFFER_SIZE; i++) {
            atomic_init(&log->ring_buffer->entries[i].state, LOG_SLOT_EMPTY);
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

void log_retain(logger_t* logger) {
    if (!logger) return;
    atomic_fetch_add_explicit(&logger->refcount, 1, memory_order_relaxed);
}

void log_release(logger_t* logger) {
    if (!logger) return;
    
    uint32_t old_ref = atomic_fetch_sub_explicit(&logger->refcount, 1, memory_order_acq_rel);
    if (old_ref != 1) return;
    
    atomic_store_explicit(&logger->shutdown, true, memory_order_release);
    
    if (logger->mode == LOG_MODE_ASYNC && logger->ring_buffer) {
        atomic_store_explicit(&logger->ring_buffer->running, false, memory_order_release);
        pthread_join(logger->flush_thread, NULL);
        free(logger->ring_buffer);
    }
    
    free(logger);
}

void log_destroy(logger_t* logger) {
    log_release(logger);
}

distric_err_t log_write(logger_t* logger, log_level_t level,
                       const char* component, const char* message, ...) {
    if (!logger || atomic_load_explicit(&logger->shutdown, memory_order_acquire)) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!component || !message) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    size_t remaining = LOG_BUFFER_SIZE;
    size_t offset = 0;
    int written;

    written = snprintf(tls_buffer + offset, remaining, "{");
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;

    uint64_t ts = get_timestamp_ms();
    written = snprintf(tls_buffer + offset, remaining, "\"timestamp\":%lu,", ts);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;

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

    char escaped_message[1024];
    json_escape(message, escaped_message, sizeof(escaped_message));
    written = snprintf(tls_buffer + offset, remaining,
                      "\"message\":\"%s\"", escaped_message);
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    remaining -= written;

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

    written = snprintf(tls_buffer + offset, remaining, "}\n");
    if (written < 0 || (size_t)written >= remaining) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;

    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire)) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    if (logger->mode == LOG_MODE_SYNC) {
        ssize_t total = 0;
        while (total < (ssize_t)offset) {
            ssize_t n = write(logger->fd, tls_buffer + total, offset - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                return DISTRIC_ERR_IO;
            }
            total += n;
        }
        atomic_fetch_add_explicit(&logger->messages_logged, 1, memory_order_relaxed);
        return DISTRIC_OK;
    } else {
        ring_buffer_t* rb = logger->ring_buffer;
        
        if (!rb || !atomic_load_explicit(&rb->running, memory_order_acquire)) {
            return DISTRIC_ERR_INVALID_ARG;
        }
        
        uint64_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
        uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
        
        if (head - tail >= RING_BUFFER_SIZE - 1) {
            atomic_fetch_add_explicit(&logger->messages_dropped, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&logger->drops_by_level[level], 1, memory_order_relaxed);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        
        uint64_t slot_idx = atomic_fetch_add_explicit(&rb->head, 1, memory_order_acq_rel);
        log_entry_t* entry = &rb->entries[slot_idx & RING_BUFFER_MASK];

        atomic_store_explicit(&entry->state, LOG_SLOT_EMPTY, memory_order_release);
        
        entry->level = level;
        entry->timestamp_ms = ts;
        strncpy(entry->message, tls_buffer, sizeof(entry->message) - 1);
        entry->message[sizeof(entry->message) - 1] = '\0';
        
        atomic_store_explicit(&entry->state, LOG_SLOT_FILLED, memory_order_release);
        
        atomic_fetch_add_explicit(&logger->messages_logged, 1, memory_order_relaxed);
        
        return DISTRIC_OK;
    }
}