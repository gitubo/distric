#ifndef DISTRIC_OBS_LOGGING_H
#define DISTRIC_OBS_LOGGING_H

#include "distric_obs.h"
#include <pthread.h>
#include <stdatomic.h>

/* Internal logging structures */

#define LOG_BUFFER_SIZE 4096
#define RING_BUFFER_SIZE 8192
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

typedef struct {
    char data[LOG_BUFFER_SIZE];
    size_t length;
    _Atomic bool ready;  /* Commit flag for safe concurrent access */
} log_entry_t;

typedef struct {
    log_entry_t entries[RING_BUFFER_SIZE];
    _Atomic size_t write_pos;
    _Atomic size_t read_pos;
    _Atomic bool running;
} ring_buffer_t;

struct logger_s {
    int fd;
    log_mode_t mode;
    ring_buffer_t* ring_buffer;
    pthread_t flush_thread;
    _Atomic bool shutdown;
};

#endif /* DISTRIC_OBS_LOGGING_H */