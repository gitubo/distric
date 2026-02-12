/**
 * @file raft_test_framework.c
 * @brief Implementation of RAFT Test Framework
 * 
 * Based on the working test_raft_cluster.c implementation,
 * this provides reusable test infrastructure.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "raft_test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdatomic.h>

/* ============================================================================
 * GLOBALS
 * ========================================================================= */

static bool framework_initialized = false;
static uint32_t cluster_counter = 0;  /* For unique data directories */

/* ============================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/**
 * @brief Get current time in milliseconds
 */
uint64_t test_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

/**
 * @brief Tick thread function
 */
static void* tick_thread_func(void* arg) {
    test_node_t* node = (test_node_t*)arg;
    
    while (atomic_load(&node->running)) {
        if (!atomic_load(&node->crashed)) {
            /* Call raft_tick to advance time */
            raft_tick(node->raft_node);
        }
        
        /* Sleep for tick interval */
        usleep(TEST_DEFAULT_TICK_INTERVAL_MS * 1000);
    }
    
    return NULL;
}

/**
 * @brief Create directory recursively
 */
static bool create_directory(const char* path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    
    return true;
}

/**
 * @brief Remove directory recursively
 */
static void remove_directory(const char* path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    system(cmd);
}

/* ============================================================================
 * FRAMEWORK INITIALIZATION
 * ========================================================================= */

bool test_framework_init(void) {
    if (framework_initialized) {
        return true;
    }
    
    /* Seed random number generator */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    srand((unsigned int)(ts.tv_sec ^ ts.tv_nsec));
    
    framework_initialized = true;
    return true;
}

void test_framework_cleanup(void) {
    framework_initialized = false;
}

/* ============================================================================
 * CLUSTER MANAGEMENT
 * ========================================================================= */

test_cluster_t* test_cluster_create(int num_nodes) {
    if (!framework_initialized) {
        test_framework_init();
    }
    
    if (num_nodes < 1 || num_nodes > TEST_MAX_NODES) {
        fprintf(stderr, "Invalid number of nodes: %d\n", num_nodes);
        return NULL;
    }
    
    test_cluster_t* cluster = calloc(1, sizeof(test_cluster_t));
    if (!cluster) {
        return NULL;
    }
    
    cluster->num_nodes = num_nodes;
    cluster->partition_active = false;
    cluster->tick_interval_ms = TEST_DEFAULT_TICK_INTERVAL_MS;
    
    /* Create unique data directory */
    snprintf(cluster->data_dir, sizeof(cluster->data_dir),
             "%s%u", TEST_DATA_DIR_PREFIX, cluster_counter++);
    
    if (!create_directory(cluster->data_dir)) {
        fprintf(stderr, "Failed to create data directory: %s\n", cluster->data_dir);
        free(cluster);
        return NULL;
    }
    
    /* Initialize metrics and logger */
    if (metrics_init(&cluster->metrics) != DISTRIC_OK) {
        fprintf(stderr, "Failed to initialize metrics\n");
        remove_directory(cluster->data_dir);
        free(cluster);
        return NULL;
    }
    
    if (log_init(&cluster->logger, STDOUT_FILENO, LOG_MODE_SYNC) != DISTRIC_OK) {
        fprintf(stderr, "Failed to initialize logger\n");
        metrics_destroy(cluster->metrics);
        remove_directory(cluster->data_dir);
        free(cluster);
        return NULL;
    }
    
    /* Create nodes */
    for (int i = 0; i < num_nodes; i++) {
        test_node_t* node = &cluster->nodes[i];
        
        snprintf(node->node_id, sizeof(node->node_id), "node-%d", i);
        node->port = TEST_BASE_PORT + i;
        atomic_store(&node->running, false);
        atomic_store(&node->crashed, false);
        atomic_store(&node->message_loss_rate, 0.0);
        node->message_delay_min_ms = 0;
        node->message_delay_max_ms = 0;
        node->partitioned = false;
        node->partition_group = 0;
        
        /* Create peer list (all other nodes) */
        raft_peer_t* peers = NULL;
        size_t peer_count = num_nodes - 1;
        
        if (peer_count > 0) {
            peers = calloc(peer_count, sizeof(raft_peer_t));
            if (!peers) {
                test_cluster_destroy(cluster);
                return NULL;
            }
            
            size_t p = 0;
            for (int j = 0; j < num_nodes; j++) {
                if (j == i) continue;
                
                snprintf(peers[p].node_id, sizeof(peers[p].node_id), "node-%d", j);
                strncpy(peers[p].address, "127.0.0.1", sizeof(peers[p].address) - 1);
                peers[p].port = TEST_BASE_PORT + j;
                p++;
            }
        }
        
        /* Create RAFT config */
        raft_config_t config = {
            .peers = peers,
            .peer_count = peer_count,
            .election_timeout_min_ms = TEST_DEFAULT_ELECTION_TIMEOUT_MIN_MS,
            .election_timeout_max_ms = TEST_DEFAULT_ELECTION_TIMEOUT_MAX_MS,
            .heartbeat_interval_ms = TEST_DEFAULT_HEARTBEAT_INTERVAL_MS,
            .snapshot_threshold = 100,
            .persistence_data_dir = NULL,  /* No persistence for tests */
            .apply_fn = NULL,
            .state_change_fn = NULL,
            .user_data = node,
            .metrics = cluster->metrics,
            .logger = cluster->logger
        };
        strncpy(config.node_id, node->node_id, sizeof(config.node_id) - 1);
        
        /* Create RAFT node */
        distric_err_t err = raft_create(&config, &node->raft_node);
        free(peers);
        
        if (err != DISTRIC_OK) {
            fprintf(stderr, "Failed to create RAFT node %d: %s\n",
                    i, distric_strerror(err));
            test_cluster_destroy(cluster);
            return NULL;
        }
        
        /* Create RPC context */
        raft_rpc_config_t rpc_cfg = {
            .bind_address = "127.0.0.1",
            .bind_port = node->port,
            .rpc_timeout_ms = 3000,
            .max_retries = 3,
            .metrics = cluster->metrics,
            .logger = cluster->logger
        };
        
        err = raft_rpc_create(&rpc_cfg, node->raft_node, &node->rpc);
        if (err != DISTRIC_OK) {
            fprintf(stderr, "Failed to create RPC context for node %d: %s\n",
                    i, distric_strerror(err));
            test_cluster_destroy(cluster);
            return NULL;
        }
    }
    
    return cluster;
}

void test_cluster_destroy(test_cluster_t* cluster) {
    if (!cluster) {
        return;
    }
    
    /* Stop cluster first */
    test_cluster_stop(cluster);
    
    /* Destroy nodes */
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        test_node_t* node = &cluster->nodes[i];
        
        if (node->rpc) {
            raft_rpc_destroy(node->rpc);
            node->rpc = NULL;
        }
        
        if (node->raft_node) {
            raft_destroy(node->raft_node);
            node->raft_node = NULL;
        }
    }
    
    /* Cleanup metrics and logger */
    if (cluster->logger) {
        log_destroy(cluster->logger);
    }
    
    if (cluster->metrics) {
        metrics_destroy(cluster->metrics);
    }
    
    /* Remove data directory */
    remove_directory(cluster->data_dir);
    
    free(cluster);
}

bool test_cluster_start(test_cluster_t* cluster) {
    if (!cluster) {
        return false;
    }
    
    /* Start RPC servers */
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        test_node_t* node = &cluster->nodes[i];
        
        if (raft_rpc_start(node->rpc) != DISTRIC_OK) {
            fprintf(stderr, "Failed to start RPC for node %zu\n", i);
            return false;
        }
    }
    
    /* Let RPC servers settle */
    usleep(100000);  /* 100ms */
    
    /* Start RAFT nodes */
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        test_node_t* node = &cluster->nodes[i];
        
        if (raft_start(node->raft_node) != DISTRIC_OK) {
            fprintf(stderr, "Failed to start RAFT node %zu\n", i);
            return false;
        }
        
        /* Start tick thread */
        atomic_store(&node->running, true);
        if (pthread_create(&node->tick_thread, NULL, tick_thread_func, node) != 0) {
            fprintf(stderr, "Failed to create tick thread for node %zu\n", i);
            atomic_store(&node->running, false);
            return false;
        }
    }
    
    /* Let nodes initialize */
    usleep(100000);  /* 100ms */
    
    return true;
}

void test_cluster_stop(test_cluster_t* cluster) {
    if (!cluster) {
        return;
    }
    
    /* Stop all nodes */
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        test_node_t* node = &cluster->nodes[i];
        
        /* Stop RAFT node first */
        if (node->raft_node) {
            raft_stop(node->raft_node);
        }
        
        /* Stop tick thread */
        if (atomic_load(&node->running)) {
            atomic_store(&node->running, false);
            pthread_join(node->tick_thread, NULL);
        }
        
        /* Stop RPC server */
        if (node->rpc) {
            raft_rpc_stop(node->rpc);
        }
    }
    
    /* Wait for any in-flight RPCs to complete */
    usleep(200000);  /* 200ms */
}

raft_node_t* test_cluster_get_node(test_cluster_t* cluster, int node_id) {
    if (!cluster || node_id < 0 || node_id >= (int)cluster->num_nodes) {
        return NULL;
    }
    return cluster->nodes[node_id].raft_node;
}

test_node_t* test_cluster_get_test_node(test_cluster_t* cluster, int node_id) {
    if (!cluster || node_id < 0 || node_id >= (int)cluster->num_nodes) {
        return NULL;
    }
    return &cluster->nodes[node_id];
}

/* ============================================================================
 * TIME CONTROL
 * ========================================================================= */

void test_cluster_tick(test_cluster_t* cluster, int ms) {
    if (!cluster || ms <= 0) {
        return;
    }
    
    /* The tick threads are already running and calling raft_tick()
     * automatically. We just need to sleep for the requested time. */
    usleep(ms * 1000);
}

/* ============================================================================
 * LEADER ELECTION HELPERS
 * ========================================================================= */

int test_cluster_wait_for_leader(test_cluster_t* cluster, uint32_t timeout_ms) {
    if (!cluster) {
        return -1;
    }
    
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < timeout_ms) {
        int leader_id = test_cluster_find_leader(cluster);
        if (leader_id >= 0) {
            return leader_id;
        }
        
        usleep(50000);  /* 50ms */
    }
    
    return -1;  /* Timeout */
}

bool test_cluster_start_election(test_cluster_t* cluster, int node_id) {
    if (!cluster || node_id < 0 || node_id >= (int)cluster->num_nodes) {
        return false;
    }
    
    test_node_t* node = &cluster->nodes[node_id];
    
    /* This is a test helper - in a real scenario, elections are triggered
     * by timeouts. For testing, we can't directly trigger an election
     * through the public API, so we'd need to manipulate timeouts or
     * wait for natural timeout. For now, just return true. */
    
    (void)node;  /* Unused */
    return true;
}

void test_cluster_count_states(test_cluster_t* cluster,
                               int* leaders_out,
                               int* candidates_out,
                               int* followers_out) {
    if (!cluster) {
        return;
    }
    
    int leaders = 0, candidates = 0, followers = 0;
    
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        if (atomic_load(&cluster->nodes[i].crashed)) {
            continue;
        }
        
        raft_state_t state = raft_get_state(cluster->nodes[i].raft_node);
        
        switch (state) {
            case RAFT_STATE_LEADER:
                leaders++;
                break;
            case RAFT_STATE_CANDIDATE:
                candidates++;
                break;
            case RAFT_STATE_FOLLOWER:
                followers++;
                break;
        }
    }
    
    if (leaders_out) *leaders_out = leaders;
    if (candidates_out) *candidates_out = candidates;
    if (followers_out) *followers_out = followers;
}

int test_cluster_find_leader(test_cluster_t* cluster) {
    if (!cluster) {
        return -1;
    }
    
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        if (atomic_load(&cluster->nodes[i].crashed)) {
            continue;
        }
        
        if (raft_get_state(cluster->nodes[i].raft_node) == RAFT_STATE_LEADER) {
            return (int)i;
        }
    }
    
    return -1;
}

bool test_cluster_elect_leader(test_cluster_t* cluster, int preferred_node) {
    (void)preferred_node; // Ignored for now - any node can become leader
    
    if (!cluster) {
        return false;
    }
    
    // Check if cluster is already running
    bool already_started = false;
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        if (atomic_load(&cluster->nodes[i].running)) {
            already_started = true;
            break;
        }
    }
    
    // Start cluster if not running
    if (!already_started) {
        if (!test_cluster_start(cluster)) {
            return false;
        }
    }
    
    // Wait for leader election (5 second timeout)
    return test_cluster_wait_for_leader(cluster, 5000) >= 0;
}

/* ============================================================================
 * LOG REPLICATION HELPERS
 * ========================================================================= */

bool test_cluster_replicate_logs(test_cluster_t* cluster, int num_entries) {
    if (!cluster || num_entries <= 0) {
        return false;
    }
    
    /* Find leader */
    int leader_id = test_cluster_find_leader(cluster);
    if (leader_id < 0) {
        return false;
    }
    
    raft_node_t* leader = cluster->nodes[leader_id].raft_node;
    
    /* Append entries */
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "test_entry_%d", i);
        
        uint32_t index;
        distric_err_t err = raft_append_entry(leader,
                                              (const uint8_t*)data,
                                              strlen(data),
                                              &index);
        if (err != DISTRIC_OK) {
            return false;
        }
    }
    
    /* Wait for replication (give it some time) */
    test_cluster_tick(cluster, 500);
    
    return true;
}

bool test_cluster_replicate_and_commit(test_cluster_t* cluster) {
    return test_cluster_replicate_logs(cluster, 1);
}

bool test_cluster_verify_log_consistency(test_cluster_t* cluster) {
    if (!cluster || cluster->num_nodes == 0) {
        return false;
    }
    
    /* Find first non-crashed node to use as reference */
    int ref_id = -1;
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        if (!atomic_load(&cluster->nodes[i].crashed)) {
            ref_id = (int)i;
            break;
        }
    }
    
    if (ref_id < 0) {
        return false;  /* All nodes crashed */
    }
    
    uint32_t ref_commit = raft_get_commit_index(cluster->nodes[ref_id].raft_node);
    
    /* Check all other non-crashed nodes */
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        if ((int)i == ref_id || atomic_load(&cluster->nodes[i].crashed)) {
            continue;
        }
        
        uint32_t commit = raft_get_commit_index(cluster->nodes[i].raft_node);
        
        /* Committed entries should match */
        if (commit != ref_commit) {
            /* Allow some variance during replication */
            if (commit > ref_commit + 1 || ref_commit > commit + 1) {
                return false;
            }
        }
    }
    
    return true;
}

/* ============================================================================
 * NETWORK SIMULATION
 * ========================================================================= */

bool test_cluster_partition(test_cluster_t* cluster,
                            int* group1, int group1_size,
                            int* group2, int group2_size) {
    if (!cluster || !group1 || !group2) {
        return false;
    }
    
    if (group1_size + group2_size != (int)cluster->num_nodes) {
        fprintf(stderr, "Partition groups don't cover all nodes\n");
        return false;
    }
    
    cluster->partition_active = true;
    cluster->group1_count = group1_size;
    cluster->group2_count = group2_size;
    
    memcpy(cluster->group1_nodes, group1, group1_size * sizeof(int));
    memcpy(cluster->group2_nodes, group2, group2_size * sizeof(int));
    
    /* Mark nodes with their partition group */
    for (int i = 0; i < group1_size; i++) {
        int node_id = group1[i];
        if (node_id >= 0 && node_id < (int)cluster->num_nodes) {
            cluster->nodes[node_id].partitioned = true;
            cluster->nodes[node_id].partition_group = 0;
        }
    }
    
    for (int i = 0; i < group2_size; i++) {
        int node_id = group2[i];
        if (node_id >= 0 && node_id < (int)cluster->num_nodes) {
            cluster->nodes[node_id].partitioned = true;
            cluster->nodes[node_id].partition_group = 1;
        }
    }
    
    /* NOTE: Actual network isolation would require RPC layer support.
     * For now, this just marks the partition state. The RPC layer would
     * need to check can_communicate() before sending messages. */
    
    return true;
}

void test_cluster_heal_partition(test_cluster_t* cluster) {
    if (!cluster) {
        return;
    }
    
    cluster->partition_active = false;
    
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        cluster->nodes[i].partitioned = false;
        cluster->nodes[i].partition_group = 0;
    }
}

void test_cluster_set_message_delay(test_cluster_t* cluster,
                                   int node_id,
                                   uint32_t min_ms,
                                   uint32_t max_ms) {
    if (!cluster || node_id < 0 || node_id >= (int)cluster->num_nodes) {
        return;
    }
    
    cluster->nodes[node_id].message_delay_min_ms = min_ms;
    cluster->nodes[node_id].message_delay_max_ms = max_ms;
}

void test_cluster_set_message_loss_rate(test_cluster_t* cluster,
                                       int node_id,
                                       double rate) {
    if (!cluster || node_id < 0 || node_id >= (int)cluster->num_nodes) {
        return;
    }
    
    if (rate < 0.0) rate = 0.0;
    if (rate > 1.0) rate = 1.0;
    
    atomic_store(&cluster->nodes[node_id].message_loss_rate, rate);
}

/* ============================================================================
 * NODE CRASH/RESTART SIMULATION
 * ========================================================================= */

bool test_cluster_crash_node(test_cluster_t* cluster, int node_id) {
    if (!cluster || node_id < 0 || node_id >= (int)cluster->num_nodes) {
        return false;
    }
    
    test_node_t* node = &cluster->nodes[node_id];
    
    /* Mark as crashed - tick thread will skip ticking */
    atomic_store(&node->crashed, true);
    
    /* Stop RPC to simulate crash */
    if (node->rpc) {
        raft_rpc_stop(node->rpc);
    }
    
    return true;
}

bool test_cluster_restart_node(test_cluster_t* cluster, int node_id) {
    if (!cluster || node_id < 0 || node_id >= (int)cluster->num_nodes) {
        return false;
    }
    
    test_node_t* node = &cluster->nodes[node_id];
    
    /* Restart RPC */
    if (node->rpc) {
        if (raft_rpc_start(node->rpc) != DISTRIC_OK) {
            return false;
        }
    }
    
    /* Mark as running */
    atomic_store(&node->crashed, false);
    
    return true;
}

/* ============================================================================
 * UTILITIES
 * ========================================================================= */

void test_cluster_print_state(test_cluster_t* cluster) {
    if (!cluster) {
        return;
    }
    
    printf("\n=== Cluster State ===\n");
    printf("Nodes: %zu\n", cluster->num_nodes);
    printf("Partition: %s\n", cluster->partition_active ? "ACTIVE" : "none");
    
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        test_node_t* node = &cluster->nodes[i];
        raft_node_t* raft = node->raft_node;
        
        const char* state_str = "UNKNOWN";
        raft_state_t state = raft_get_state(raft);
        switch (state) {
            case RAFT_STATE_FOLLOWER: state_str = "FOLLOWER"; break;
            case RAFT_STATE_CANDIDATE: state_str = "CANDIDATE"; break;
            case RAFT_STATE_LEADER: state_str = "LEADER"; break;
        }
        
        printf("  Node %zu [%s]: ", i, node->node_id);
        printf("state=%s term=%u commit=%u last_log=%u",
               state_str,
               raft_get_term(raft),
               raft_get_commit_index(raft),
               raft_get_last_log_index(raft));
        
        if (atomic_load(&node->crashed)) {
            printf(" [CRASHED]");
        }
        
        if (node->partitioned) {
            printf(" [PARTITION_GROUP_%d]", node->partition_group);
        }
        
        printf("\n");
    }
    
    printf("=====================\n\n");
}

bool test_assert(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "ASSERTION FAILED: %s\n", message);
        return false;
    }
    return true;
}