#define _DEFAULT_SOURCE
#include "distric_obs.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#define NUM_LOG_THREADS 10
#define LOGS_PER_THREAD 500

static logger_t* shared_logger = NULL;

void* logging_thread(void* arg) {
    int thread_id = *(int*)arg;
    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Log message %d from thread %d", i, thread_id);
        LOG_INFO(shared_logger, "test", msg,
                "thread_id", "1",
                "iteration", "2");
    }
    return NULL;
}

void test_sync_logging() {
    printf("Test: Synchronous logging...\n");

    logger_t* logger;
    distric_err_t err = log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC);
    assert(err == DISTRIC_OK);

    LOG_INFO(logger, "test", "Simple info message", NULL);
    LOG_WARN(logger, "test", "Warning message", "code", "404");
    LOG_ERROR(logger, "test", "Error occurred",
             "error", "File not found",
             "path", "/tmp/missing");

    log_destroy(logger);
    printf("  PASSED\n\n");
}

void test_async_logging() {
    printf("Test: Async logging mode...\n");

    logger_t* logger;
    distric_err_t err = log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
    assert(err == DISTRIC_OK);

    for (int i = 0; i < 100; i++)
        LOG_INFO(logger, "async_test", "Async log message", "iteration", "test");

    log_destroy(logger);
    printf("  PASSED\n\n");
}

void test_concurrent_logging() {
    printf("Test: Concurrent async logging (%d threads x %d logs)...\n",
           NUM_LOG_THREADS, LOGS_PER_THREAD);

    char tmpfile[] = "/tmp/distric_log_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);

    distric_err_t err = log_init(&shared_logger, fd, LOG_MODE_ASYNC);
    assert(err == DISTRIC_OK);

    pthread_t threads[NUM_LOG_THREADS];
    int thread_ids[NUM_LOG_THREADS];

    for (int i = 0; i < NUM_LOG_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, logging_thread, &thread_ids[i]);
    }
    for (int i = 0; i < NUM_LOG_THREADS; i++)
        pthread_join(threads[i], NULL);

    log_destroy(shared_logger);
    close(fd);

    FILE* f = fopen(tmpfile, "r");
    assert(f != NULL);

    int line_count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        line_count++;
        assert(strchr(line, '{') != NULL);
        assert(strchr(line, '}') != NULL);
        assert(strstr(line, "\"timestamp\"") != NULL);
        assert(strstr(line, "\"level\"") != NULL);
        assert(strstr(line, "\"component\"") != NULL);
        assert(strstr(line, "\"message\"") != NULL);
    }
    fclose(f);

    int expected      = NUM_LOG_THREADS * LOGS_PER_THREAD;
    int min_acceptable = expected * 0.99;
    printf("  Expected logs: %d, Actual: %d\n", expected, line_count);
    if (line_count < min_acceptable) {
        printf("  ERROR: Too many logs lost!\n");
        assert(0 && "Too many logs lost");
    }

    unlink(tmpfile);
    printf("  PASSED (%.1f%% delivery rate)\n\n", (line_count * 100.0) / expected);
}

void test_json_format() {
    printf("Test: JSON format and escaping...\n");

    char tmpfile[] = "/tmp/distric_json_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);

    logger_t* logger;
    distric_err_t err = log_init(&logger, fd, LOG_MODE_SYNC);
    assert(err == DISTRIC_OK);

    LOG_INFO(logger, "test", "Message with \"quotes\" and \\backslash\\",
            "key", "value with\nnewline and\ttab");

    log_destroy(logger);
    close(fd);

    FILE* f = fopen(tmpfile, "r");
    assert(f != NULL);
    char line[4096];
    if (fgets(line, sizeof(line), f)) {
        assert(strstr(line, "\\\"quotes\\\"") != NULL);
        assert(strstr(line, "\\\\backslash\\\\") != NULL);
        assert(strstr(line, "\\n") != NULL);
        assert(strstr(line, "\\t") != NULL);
    }
    fclose(f);
    unlink(tmpfile);
    printf("  PASSED\n\n");
}

void test_log_levels() {
    printf("Test: All log levels...\n");

    char tmpfile[] = "/tmp/distric_levels_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);

    logger_t* logger;
    distric_err_t err = log_init(&logger, fd, LOG_MODE_SYNC);
    assert(err == DISTRIC_OK);

    LOG_DEBUG(logger, "test", "Debug message", NULL);
    LOG_INFO(logger, "test", "Info message", NULL);
    LOG_WARN(logger, "test", "Warning message", NULL);
    LOG_ERROR(logger, "test", "Error message", NULL);
    LOG_FATAL(logger, "test", "Fatal message", NULL);

    log_destroy(logger);
    close(fd);

    FILE* f = fopen(tmpfile, "r");
    assert(f != NULL);
    char content[8192];
    size_t read_size = fread(content, 1, sizeof(content) - 1, f);
    content[read_size] = '\0';
    fclose(f);

    assert(strstr(content, "\"level\":\"DEBUG\"") != NULL);
    assert(strstr(content, "\"level\":\"INFO\"") != NULL);
    assert(strstr(content, "\"level\":\"WARN\"") != NULL);
    assert(strstr(content, "\"level\":\"ERROR\"") != NULL);
    assert(strstr(content, "\"level\":\"FATAL\"") != NULL);

    unlink(tmpfile);
    printf("  PASSED\n\n");
}

int main() {
    printf("=== DistriC Logging Tests ===\n\n");
    test_sync_logging();
    test_async_logging();
    test_json_format();
    test_log_levels();
    test_concurrent_logging();
    printf("=== All logging tests passed ===\n");
    return 0;
}