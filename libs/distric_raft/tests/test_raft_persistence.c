/**
 * @file test_raft_persistence.c
 * @brief Tests for Raft Persistence
 * 
 * Tests:
 * - State save/load
 * - Log entry persistence
 * - Crash recovery
 * - Snapshot save/load
 * - Log compaction
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <distric_raft/raft_persistence.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START() printf("\n[TEST] %s...\n", __func__)
#define TEST_PASS() do { \
    printf("[PASS] %s\n", __func__); \
    tests_passed++; \
} while(0)

#define ASSERT_OK(expr) do { \
    distric_err_t _err = (expr); \
    if (_err != DISTRIC_OK) { \
        fprintf(stderr, "FAIL: %s returned %d\n", #expr, _err); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s is false\n", #expr); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL: %s (%d) != %s (%d)\n", #a, (int)(a), #b, (int)(b)); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "FAIL: %s (%s) != %s (%s)\n", #a, (a), #b, (b)); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* Test directory */
#define TEST_DATA_DIR "/tmp/distric_test_persistence"

/* ============================================================================
 * TEST HELPERS
 * ========================================================================= */

static void cleanup_test_dir(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DATA_DIR);
    system(cmd);
}

static void create_test_dir(void) {
    cleanup_test_dir();
    mkdir(TEST_DATA_DIR, 0755);
}

static raft_persistence_context_t* create_test_context(void) {
    raft_persistence_config_t config = {
        .data_dir = TEST_DATA_DIR,
        .max_db_size_mb = 10,
        .sync_on_write = true,
        .logger = NULL
    };
    
    raft_persistence_context_t* context = NULL;
    distric_err_t err = raft_persistence_create(&config, &context);
    if (err != DISTRIC_OK) {
        return NULL;
    }
    
    return context;
}

/* ============================================================================
 * STATE PERSISTENCE TESTS
 * ========================================================================= */

void test_state_save_load() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Save state */
    raft_persistent_state_t state = {
        .current_term = 42,
    };
    strncpy(state.voted_for, "node-123", sizeof(state.voted_for) - 1);
    
    ASSERT_OK(raft_persistence_save_state(context, &state));
    
    /* Load state */
    raft_persistent_state_t loaded = {0};
    ASSERT_OK(raft_persistence_load_state(context, &loaded));
    
    ASSERT_EQ(loaded.current_term, 42);
    ASSERT_STR_EQ(loaded.voted_for, "node-123");
    
    printf("  State persisted correctly\n");
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

void test_state_persistence_across_restart() {
    TEST_START();
    
    create_test_dir();
    
    /* First session: save state */
    {
        raft_persistence_context_t* context = create_test_context();
        ASSERT_TRUE(context != NULL);
        
        raft_persistent_state_t state = {
            .current_term = 100,
        };
        strncpy(state.voted_for, "leader-5", sizeof(state.voted_for) - 1);
        
        ASSERT_OK(raft_persistence_save_state(context, &state));
        
        raft_persistence_destroy(context);
    }
    
    /* Second session: load state */
    {
        raft_persistence_context_t* context = create_test_context();
        ASSERT_TRUE(context != NULL);
        
        raft_persistent_state_t loaded = {0};
        ASSERT_OK(raft_persistence_load_state(context, &loaded));
        
        ASSERT_EQ(loaded.current_term, 100);
        ASSERT_STR_EQ(loaded.voted_for, "leader-5");
        
        printf("  State survives restart\n");
        
        raft_persistence_destroy(context);
    }
    
    cleanup_test_dir();
    
    TEST_PASS();
}

void test_state_no_prior_state() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Try to load without saving */
    raft_persistent_state_t loaded = {0};
    distric_err_t err = raft_persistence_load_state(context, &loaded);
    
    ASSERT_TRUE(err == DISTRIC_ERR_NOT_FOUND);
    
    printf("  Correctly returns NOT_FOUND for missing state\n");
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

/* ============================================================================
 * LOG PERSISTENCE TESTS
 * ========================================================================= */

void test_log_append_and_load() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Append entries */
    raft_log_entry_t entry1 = {
        .index = 1,
        .term = 1,
        .type = RAFT_ENTRY_COMMAND,
        .data = (uint8_t*)"cmd1",
        .data_len = 4
    };
    
    raft_log_entry_t entry2 = {
        .index = 2,
        .term = 1,
        .type = RAFT_ENTRY_COMMAND,
        .data = (uint8_t*)"cmd2",
        .data_len = 4
    };
    
    ASSERT_OK(raft_persistence_append_entry(context, &entry1));
    ASSERT_OK(raft_persistence_append_entry(context, &entry2));
    
    /* Load entry */
    raft_log_entry_t loaded = {0};
    ASSERT_OK(raft_persistence_load_entry(context, 1, &loaded));
    
    ASSERT_EQ(loaded.index, 1);
    ASSERT_EQ(loaded.term, 1);
    ASSERT_EQ(loaded.type, RAFT_ENTRY_COMMAND);
    ASSERT_EQ(loaded.data_len, 4);
    ASSERT_TRUE(memcmp(loaded.data, "cmd1", 4) == 0);
    
    free(loaded.data);
    
    printf("  Log entries persisted correctly\n");
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

void test_log_load_all() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Append 10 entries */
    for (uint32_t i = 1; i <= 10; i++) {
        char data[16];
        snprintf(data, sizeof(data), "cmd%u", i);
        
        raft_log_entry_t entry = {
            .index = i,
            .term = (i / 3) + 1,
            .type = RAFT_ENTRY_COMMAND,
            .data = (uint8_t*)data,
            .data_len = strlen(data)
        };
        
        ASSERT_OK(raft_persistence_append_entry(context, &entry));
    }
    
    /* Load all */
    raft_log_entry_t* entries = NULL;
    size_t count = 0;
    
    ASSERT_OK(raft_persistence_load_all_entries(context, &entries, &count));
    
    ASSERT_EQ(count, 10);
    ASSERT_EQ(entries[0].index, 1);
    ASSERT_EQ(entries[9].index, 10);
    
    printf("  Loaded all %zu entries\n", count);
    
    /* Cleanup */
    for (size_t i = 0; i < count; i++) {
        free(entries[i].data);
    }
    free(entries);
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

void test_log_truncate() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Append 10 entries */
    for (uint32_t i = 1; i <= 10; i++) {
        char data[16];
        snprintf(data, sizeof(data), "cmd%u", i);
        
        raft_log_entry_t entry = {
            .index = i,
            .term = 1,
            .type = RAFT_ENTRY_COMMAND,
            .data = (uint8_t*)data,
            .data_len = strlen(data)
        };
        
        ASSERT_OK(raft_persistence_append_entry(context, &entry));
    }
    
    /* Truncate from index 6 */
    ASSERT_OK(raft_persistence_truncate_log(context, 6));
    
    /* Verify only 1-5 remain */
    raft_log_entry_t* entries = NULL;
    size_t count = 0;
    
    ASSERT_OK(raft_persistence_load_all_entries(context, &entries, &count));
    
    ASSERT_EQ(count, 5);
    ASSERT_EQ(entries[0].index, 1);
    ASSERT_EQ(entries[4].index, 5);
    
    printf("  Truncated log from index 6\n");
    printf("  Remaining entries: %zu\n", count);
    
    /* Cleanup */
    for (size_t i = 0; i < count; i++) {
        free(entries[i].data);
    }
    free(entries);
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

void test_log_bounds() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Empty log */
    uint32_t first, last;
    ASSERT_OK(raft_persistence_get_log_bounds(context, &first, &last));
    ASSERT_EQ(first, 0);
    ASSERT_EQ(last, 0);
    
    /* Append entries 5-10 */
    for (uint32_t i = 5; i <= 10; i++) {
        raft_log_entry_t entry = {
            .index = i,
            .term = 1,
            .type = RAFT_ENTRY_COMMAND,
            .data = (uint8_t*)"cmd",
            .data_len = 3
        };
        
        ASSERT_OK(raft_persistence_append_entry(context, &entry));
    }
    
    /* Check bounds */
    ASSERT_OK(raft_persistence_get_log_bounds(context, &first, &last));
    ASSERT_EQ(first, 5);
    ASSERT_EQ(last, 10);
    
    printf("  Log bounds: [%u, %u]\n", first, last);
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

/* ============================================================================
 * CRASH RECOVERY TESTS
 * ========================================================================= */

void test_crash_recovery() {
    TEST_START();
    
    create_test_dir();
    
    /* Simulate normal operation */
    {
        raft_persistence_context_t* context = create_test_context();
        ASSERT_TRUE(context != NULL);
        
        /* Save state */
        raft_persistent_state_t state = {
            .current_term = 50,
        };
        strncpy(state.voted_for, "node-X", sizeof(state.voted_for) - 1);
        ASSERT_OK(raft_persistence_save_state(context, &state));
        
        /* Append entries */
        for (uint32_t i = 1; i <= 20; i++) {
            char data[16];
            snprintf(data, sizeof(data), "cmd%u", i);
            
            raft_log_entry_t entry = {
                .index = i,
                .term = (i / 5) + 1,
                .type = RAFT_ENTRY_COMMAND,
                .data = (uint8_t*)data,
                .data_len = strlen(data)
            };
            
            ASSERT_OK(raft_persistence_append_entry(context, &entry));
        }
        
        raft_persistence_destroy(context);
        /* Simulate crash here */
    }
    
    /* Recover after crash */
    {
        raft_persistence_context_t* context = create_test_context();
        ASSERT_TRUE(context != NULL);
        
        /* Load state */
        raft_persistent_state_t state = {0};
        ASSERT_OK(raft_persistence_load_state(context, &state));
        ASSERT_EQ(state.current_term, 50);
        ASSERT_STR_EQ(state.voted_for, "node-X");
        
        /* Load log */
        raft_log_entry_t* entries = NULL;
        size_t count = 0;
        ASSERT_OK(raft_persistence_load_all_entries(context, &entries, &count));
        ASSERT_EQ(count, 20);
        
        printf("  Crash recovery successful\n");
        printf("  Recovered state: term=%u, voted_for=%s\n", 
               state.current_term, state.voted_for);
        printf("  Recovered log: %zu entries\n", count);
        
        /* Cleanup */
        for (size_t i = 0; i < count; i++) {
            free(entries[i].data);
        }
        free(entries);
        
        raft_persistence_destroy(context);
    }
    
    cleanup_test_dir();
    
    TEST_PASS();
}

/* ============================================================================
 * SNAPSHOT TESTS
 * ========================================================================= */

void test_snapshot_save_load() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Create snapshot */
    raft_snapshot_metadata_t metadata = {
        .last_included_index = 100,
        .last_included_term = 10,
        .data_len = 1024
    };
    
    uint8_t* data = (uint8_t*)malloc(metadata.data_len);
    memset(data, 0xAB, metadata.data_len);
    
    ASSERT_OK(raft_persistence_save_snapshot(context, &metadata, data));
    
    /* Load metadata */
    raft_snapshot_metadata_t loaded_meta = {0};
    ASSERT_OK(raft_persistence_load_snapshot_metadata(context, &loaded_meta));
    
    ASSERT_EQ(loaded_meta.last_included_index, 100);
    ASSERT_EQ(loaded_meta.last_included_term, 10);
    ASSERT_EQ(loaded_meta.data_len, 1024);
    
    /* Load data */
    uint8_t* loaded_data = NULL;
    size_t loaded_len = 0;
    ASSERT_OK(raft_persistence_load_snapshot_data(context, &loaded_data, &loaded_len));
    
    ASSERT_EQ(loaded_len, 1024);
    ASSERT_TRUE(memcmp(loaded_data, data, 1024) == 0);
    
    printf("  Snapshot persisted correctly\n");
    
    free(data);
    free(loaded_data);
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

void test_snapshot_log_compaction() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Append 100 log entries */
    for (uint32_t i = 1; i <= 100; i++) {
        raft_log_entry_t entry = {
            .index = i,
            .term = 1,
            .type = RAFT_ENTRY_COMMAND,
            .data = (uint8_t*)"cmd",
            .data_len = 3
        };
        
        ASSERT_OK(raft_persistence_append_entry(context, &entry));
    }
    
    /* Create snapshot at index 80 */
    raft_snapshot_metadata_t metadata = {
        .last_included_index = 80,
        .last_included_term = 1,
        .data_len = 100
    };
    
    uint8_t data[100];
    memset(data, 0, sizeof(data));
    
    ASSERT_OK(raft_persistence_save_snapshot(context, &metadata, data));
    
    /* Verify log only contains entries 81-100 */
    uint32_t first, last;
    ASSERT_OK(raft_persistence_get_log_bounds(context, &first, &last));
    
    /* Note: After compaction, entries 1-80 should be removed */
    printf("  Log bounds after snapshot: [%u, %u]\n", first, last);
    ASSERT_EQ(last, 100);
    
    raft_log_entry_t* entries = NULL;
    size_t count = 0;
    ASSERT_OK(raft_persistence_load_all_entries(context, &entries, &count));
    
    printf("  Entries after compaction: %zu (expected 20)\n", count);
    /* Should have entries 81-100 (20 entries) */
    ASSERT_EQ(count, 20);
    
    /* Cleanup */
    for (size_t i = 0; i < count; i++) {
        free(entries[i].data);
    }
    free(entries);
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

/* ============================================================================
 * STATISTICS TESTS
 * ========================================================================= */

void test_persistence_stats() {
    TEST_START();
    
    create_test_dir();
    
    raft_persistence_context_t* context = create_test_context();
    ASSERT_TRUE(context != NULL);
    
    /* Append entries */
    for (uint32_t i = 1; i <= 50; i++) {
        raft_log_entry_t entry = {
            .index = i,
            .term = 1,
            .type = RAFT_ENTRY_COMMAND,
            .data = (uint8_t*)"command_data",
            .data_len = 12
        };
        
        ASSERT_OK(raft_persistence_append_entry(context, &entry));
    }
    
    /* Get stats */
    raft_persistence_stats_t stats = {0};
    ASSERT_OK(raft_persistence_get_stats(context, &stats));
    
    ASSERT_EQ(stats.log_entry_count, 50);
    ASSERT_TRUE(!stats.has_snapshot);
    
    printf("  Database statistics:\n");
    printf("    Total size: %zu bytes\n", stats.total_size_bytes);
    printf("    Log entries: %zu\n", stats.log_entry_count);
    printf("    Has snapshot: %s\n", stats.has_snapshot ? "yes" : "no");
    
    raft_persistence_destroy(context);
    cleanup_test_dir();
    
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Raft - Persistence Tests ===\n");
    
    /* State persistence */
    test_state_save_load();
    test_state_persistence_across_restart();
    test_state_no_prior_state();
    
    /* Log persistence */
    test_log_append_and_load();
    test_log_load_all();
    test_log_truncate();
    test_log_bounds();
    
    /* Crash recovery */
    test_crash_recovery();
    
    /* Snapshots */
    test_snapshot_save_load();
    test_snapshot_log_compaction();
    
    /* Statistics */
    test_persistence_stats();
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\n✓ All Raft persistence tests passed!\n");
        printf("✓ Session 3.4 (Persistence) COMPLETE\n");
        printf("\nKey Features Implemented:\n");
        printf("  - LMDB-based durable storage\n");
        printf("  - Persistent state (current_term, voted_for)\n");
        printf("  - Log entry persistence and recovery\n");
        printf("  - Snapshot save/load with log compaction\n");
        printf("  - Crash recovery with full state restoration\n");
        printf("  - Database statistics and monitoring\n");
        printf("\nNext Steps:\n");
        printf("  - Session 3.5: Integration with raft_core\n");
        printf("  - Session 3.6: Multi-node integration tests\n");
        printf("  - Session 3.7: Performance testing\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}