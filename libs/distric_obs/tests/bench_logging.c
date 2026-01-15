/* Feature test macros must come before any includes */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#define BENCH_LOG_ITERATIONS 100000
#define BENCH_LOG_THREADS 8

static logger_t* bench_logger = NULL;

/* Get high-resolution timestamp in nanoseconds */
static uint64_t get_time_ns() {
    struct timespec ts;
    /* clock_gettime and CLOCK_MONOTONIC require _POSIX_C_SOURCE >= 199309L */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Benchmark synchronous logging */
void bench_sync_logging() {
    printf("Benchmark: Synchronous logging...\n");
    
    char tmpfile[] = "/tmp/distric_bench_sync_XXXXXX";
    int fd = mkstemp(tmpfile); /* Requires _POSIX_C_SOURCE >= 200112L */
    assert(fd >= 0);
    
    logger_t* logger;
    log_init(&logger, fd, LOG_MODE_SYNC);
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < BENCH_LOG_ITERATIONS; i++) {
        LOG_INFO(logger, "benchmark", "Benchmark log message",
                "iteration", "test",
                "value", "42");
    }
    
    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_s = duration_ns / 1e9;
    double logs_per_sec = BENCH_LOG_ITERATIONS / duration_s;
    double us_per_log = (double)duration_ns / (BENCH_LOG_ITERATIONS * 1000);
    
    log_destroy(logger);
    close(fd);
    
    /* Get file size */
    FILE* f = fopen(tmpfile, "r");
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);
    
    printf("  Iterations: %d\n", BENCH_LOG_ITERATIONS);
    printf("  Duration: %.3f seconds\n", duration_s);
    printf("  Throughput: %.2f logs/sec\n", logs_per_sec);
    printf("  Latency: %.2f μs/log\n", us_per_log);
    printf("  Output size: %ld bytes\n", file_size);
    printf("  Avg log size: %ld bytes\n", file_size / BENCH_LOG_ITERATIONS);
    
    unlink(tmpfile);
    printf("\n");
}

/* Benchmark asynchronous logging */
void bench_async_logging() {
    printf("Benchmark: Asynchronous logging...\n");
    
    char tmpfile[] = "/tmp/distric_bench_async_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    
    logger_t* logger;
    log_init(&logger, fd, LOG_MODE_ASYNC);
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < BENCH_LOG_ITERATIONS; i++) {
        LOG_INFO(logger, "benchmark", "Benchmark log message",
                "iteration", "test",
                "value", "42");
    }
    
    uint64_t end = get_time_ns();
    
    /* Destroy will flush all pending logs */
    log_destroy(logger);
    close(fd);
    
    uint64_t duration_ns = end - start;
    double duration_s = duration_ns / 1e9;
    double logs_per_sec = BENCH_LOG_ITERATIONS / duration_s;
    double us_per_log = (double)duration_ns / (BENCH_LOG_ITERATIONS * 1000);
    
    /* Verify all logs written */
    FILE* f = fopen(tmpfile, "r");
    int line_count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        line_count++;
    }
    long file_size = ftell(f);
    fclose(f);
    
    printf("  Iterations: %d\n", BENCH_LOG_ITERATIONS);
    printf("  Logs written: %d\n", line_count);
    printf("  Duration: %.3f seconds\n", duration_s);
    printf("  Throughput: %.2f logs/sec\n", logs_per_sec);
    printf("  Latency: %.2f μs/log\n", us_per_log);
    printf("  Output size: %ld bytes\n", file_size);
    
    assert(line_count == BENCH_LOG_ITERATIONS);
    
    unlink(tmpfile);
    printf("\n");
}

/* Thread worker for logging benchmark */
void* logging_bench_thread(void* arg) {
    int iterations = *(int*)arg;
    
    for (int i = 0; i < iterations; i++) {
        LOG_INFO(bench_logger, "worker", "Concurrent log",
                "thread", "test",
                "iteration", "test");
    }
    
    return NULL;
}

/* Benchmark multi-threaded async logging */
void bench_async_logging_multithread() {
    printf("Benchmark: Multi-threaded async logging (%d threads)...\n",
           BENCH_LOG_THREADS);
    
    char tmpfile[] = "/tmp/distric_bench_async_mt_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    
    log_init(&bench_logger, fd, LOG_MODE_ASYNC);
    
    pthread_t threads[BENCH_LOG_THREADS];
    int iterations_per_thread = BENCH_LOG_ITERATIONS / BENCH_LOG_THREADS;
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < BENCH_LOG_THREADS; i++) {
        pthread_create(&threads[i], NULL, logging_bench_thread,
                      &iterations_per_thread);
    }
    
    for (int i = 0; i < BENCH_LOG_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    uint64_t end = get_time_ns();
    
    /* Destroy and flush */
    log_destroy(bench_logger);
    close(fd);
    
    uint64_t duration_ns = end - start;
    double duration_s = duration_ns / 1e9;
    double logs_per_sec = BENCH_LOG_ITERATIONS / duration_s;
    
    /* Verify all logs written */
    FILE* f = fopen(tmpfile, "r");
    int line_count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        line_count++;
    }
    fclose(f);
    
    printf("  Iterations: %d\n", BENCH_LOG_ITERATIONS);
    printf("  Logs written: %d\n", line_count);
    printf("  Duration: %.3f seconds\n", duration_s);
    printf("  Throughput: %.2f logs/sec\n", logs_per_sec);
    printf("  Per-thread: %.2f logs/sec\n", logs_per_sec / BENCH_LOG_THREADS);
    
    assert(line_count == BENCH_LOG_ITERATIONS);
    
    unlink(tmpfile);
    printf("\n");
}

/* Benchmark CPU overhead */
void bench_cpu_overhead() {
    printf("Benchmark: CPU overhead measurement...\n");
    
    char tmpfile[] = "/tmp/distric_bench_overhead_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);
    
    logger_t* logger;
    log_init(&logger, fd, LOG_MODE_ASYNC);
    
    /* Baseline: do nothing */
    uint64_t baseline_start = get_time_ns();
    for (int i = 0; i < BENCH_LOG_ITERATIONS; i++) {
        __asm__ __volatile__("" ::: "memory");
    }
    uint64_t baseline_end = get_time_ns();
    uint64_t baseline_ns = baseline_end - baseline_start;
    
    /* With logging */
    uint64_t logging_start = get_time_ns();
    for (int i = 0; i < BENCH_LOG_ITERATIONS; i++) {
        /* Added dummy key-value pair to satisfy variadic macro requirements 
           under strict compiler settings */
        LOG_INFO(logger, "test", "Message", "bench", "overhead");
    }
    uint64_t logging_end = get_time_ns();
    uint64_t logging_ns = logging_end - logging_start;
    
    log_destroy(logger);
    close(fd);
    unlink(tmpfile);
    
    double overhead_ns = (double)(logging_ns - baseline_ns) / BENCH_LOG_ITERATIONS;
    double overhead_pct = ((double)(logging_ns - baseline_ns) / (double)baseline_ns) * 100.0;
    
    printf("  Baseline: %.3f seconds\n", (double)baseline_ns / 1e9);
    printf("  With logging: %.3f seconds\n", (double)logging_ns / 1e9);
    printf("  Overhead per log: %.2f ns\n", overhead_ns);
    printf("  Relative overhead: %.2f%%\n", overhead_pct);
    printf("\n");
}

int main() {
    printf("=== DistriC Logging Performance Benchmarks ===\n\n");
    
    bench_sync_logging();
    bench_async_logging();
    bench_async_logging_multithread();
    bench_cpu_overhead();
    
    printf("=== Benchmarks complete ===\n");
    return 0;
}