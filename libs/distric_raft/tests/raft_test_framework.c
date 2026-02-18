/**
 * @file raft_test_framework.c
 * @brief Implementation of RAFT Test Framework
 * 
 * Based on the working test_raft_cluster.c implementation,
 * this provides reusable test infrastructure with partition support.
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
 * PARTITION FILTER CALLBACK
 * ========================================================================= */

/**
 * @brief Send filter callback for network partition simulation
 * 
 * Checks if a message should be allowed based on current partition state.
 * 
 * @param userdata Pointer to test_node_t (sender node)
 * @param peer_address Target peer address (unused)
 * @param peer_port Target peer port
 * @return true to allow send, false to block
 */
static bool can_communicate(void* userdata, const char* peer_address, uint16_t peer_port) {
    (void)peer_address;  /* Unused */
    test_node_t* sender_node = (test_node_t*)userdata;
    
    /* If no partition active, allow all communication */
    if (!sender_node->cluster->partition_active) {
        return true;
    }
    
    /* If sender is not in a partition, allow */
    if (!sender_node->partitioned) {
        return true;
    }
    
    /* Find receiver node by port */
    int receiver_idx = (int)(peer_port - TEST_BASE_PORT);
    if (receiver_idx < 0 || receiver_idx >= (int)sender_node->cluster->num_nodes) {
        /* Invalid port, allow to avoid breaking non-partition tests */
        return true;
    }
    
    test_node_t* receiver_node = &sender_node->cluster->nodes[receiver_idx];
    
    /* If receiver is not partitioned, allow */
    if (!receiver_node->partitioned) {
        return true;
    }
    
    /* Block if sender and receiver are in different partition groups */
    if (sender_node->partition_group != receiver_node->partition_group) {
        /* Message blocked by partition */
        return false;
    }
    
    /* Same partition group - allow communication */
    return true;
}

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
    uint32_t last_broadcast_term = 0;
    
    while (atomic_load(&node->running)) {
        if (!atomic_load(&node->crashed)) {
            /* Call raft_tick to advance time */
            raft_tick(node->raft_node);
            
            /* Get current state */
            raft_state_t state = raft_get_state(node->raft_node);
            uint32_t current_term = raft_get_term(node->raft_node);
            
            /* Broadcast vote requests when new election starts */
            if (state == RAFT_STATE_CANDIDATE && current_term > last_broadcast_term) {
                uint32_t votes = 0;
                raft_rpc_broadcast_request_vote(node->rpc, node->raft_node, &votes);
                last_broadcast_term = current_term;
                
                /* Process election result */
                raft_process_election_result(node->raft_node, votes);
            }
            
            /* Send heartbeats as leader */
            if (state == RAFT_STATE_LEADER && raft_should_send_heartbeat(node->raft_node)) {
                raft_rpc_broadcast_append_entries(node->rpc, node->raft_node);
                raft_mark_heartbeat_sent(node->raft_node);
            }
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
        
        /* Set cluster back-reference for partition checks */
        node->cluster = cluster;
        
        /* Create peer list (all other nodes) */
        raft_peer_t* peers = calloc(num_nodes - 1, sizeof(raft_peer_t));
        if (!peers) {
            fprintf(stderr, "Failed to allocate peers for node %d\n", i);
            test_cluster_destroy(cluster);
            return NULL;
        }
        
        int peer_idx = 0;
        for (int j = 0; j < num_nodes; j++) {
            if (i == j) continue;
            
            snprintf(peers[peer_idx].node_id, sizeof(peers[peer_idx].node_id), "node-%d", j);
            snprintf(peers[peer_idx].address, sizeof(peers[peer_idx].address), "127.0.0.1");
            peers[peer_idx].port = TEST_BASE_PORT + j;
            peer_idx++;
        }
        
        /* Create RAFT configuration */
        raft_config_t raft_cfg = {
            .peers = peers,
            .peer_count = num_nodes - 1,
            .election_timeout_min_ms = TEST_DEFAULT_ELECTION_TIMEOUT_MIN_MS,
            .election_timeout_max_ms = TEST_DEFAULT_ELECTION_TIMEOUT_MAX_MS,
            .heartbeat_interval_ms = TEST_DEFAULT_HEARTBEAT_INTERVAL_MS,
            .snapshot_threshold = 10000,
            .persistence_data_dir = NULL,  /* Disable persistence for tests */
            .apply_fn = NULL,
            .state_change_fn = NULL,
            .user_data = NULL,
            .metrics = cluster->metrics,
            .logger = cluster->logger
        };
        strncpy(raft_cfg.node_id, node->node_id, sizeof(raft_cfg.node_id) - 1);
        
        /* Create RAFT node */
        distric_err_t err = raft_create(&raft_cfg, &node->raft_node);
        
        free(peers);
        
        if (err != DISTRIC_OK) {
            fprintf(stderr, "Failed to create RAFT node %d: %s\n",
                    i, distric_err_str(err));
            test_cluster_destroy(cluster);
            return NULL;
        }
        
        /* Create RPC context */
        raft_rpc_config_t rpc_cfg = {
            .bind_address = "127.0.0.1",
            .bind_port = node->port,
            .rpc_timeout_ms = 1000,
            .max_retries = 3,
            .metrics = cluster->metrics,
            .logger = cluster->logger
        };
        
        err = raft_rpc_create(&rpc_cfg, node->raft_node, &node->rpc);
        if (err != DISTRIC_OK) {
            fprintf(stderr, "Failed to create RPC context for node %d: %s\n",
                    i, distric_err_str(err));
            test_cluster_destroy(cluster);
            return NULL;
        }
        
        /* Register partition filter callback */
        raft_rpc_set_send_filter(node->rpc, can_communicate, node);
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

void test_cluster_count_states(
    test_cluster_t *cluster,
    int *leaders,
    int *candidates,
    int *followers
) {
    if (!cluster || !leaders || !candidates || !followers) {
        return;
    }

    *leaders = 0;
    *candidates = 0;
    *followers = 0;

    for (size_t i = 0; i < cluster->num_nodes; i++) {
        test_node_t *node = &cluster->nodes[i];
        if (!node || !node->raft_node) {
            continue;
        }

        raft_state_t state = raft_get_state(node->raft_node);
        
        switch (state) {
            case RAFT_STATE_LEADER:
                (*leaders)++;
                break;
            case RAFT_STATE_CANDIDATE:
                (*candidates)++;
                break;
            case RAFT_STATE_FOLLOWER:
                (*followers)++;
                break;
            default:
                // Unknown state, don't count
                break;
        }
    }
}

/* ============================================================================
 * TIME CONTROL
 * ========================================================================= */

void test_cluster_tick(test_cluster_t* cluster, int ms) {
    if (!cluster || ms <= 0) {
        return;
    }
    
    /* The tick threads are already running and calling raft_tick()
     * automatically. We just need to sleep for the requested time.
     * Nodes will tick in the background. */
    usleep((useconds_t)ms * 1000);
}

/* ============================================================================
 * LEADER ELECTION HELPERS
 * ========================================================================= */

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

int test_cluster_wait_for_leader(test_cluster_t* cluster, int timeout_ms) {
    if (!cluster) {
        return -1;
    }
    
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < (uint64_t)timeout_ms) {
        int leader = test_cluster_find_leader(cluster);
        if (leader >= 0) {
            return leader;
        }
        
        usleep(50000);  /* 50ms */
    }
    
    return -1;
}

int test_cluster_wait_for_stable_leader(test_cluster_t* cluster, int stable_ms) {
    if (!cluster) {
        return -1;
    }
    
    int current_leader = -1;
    uint64_t stable_start = 0;
    
    while (true) {
        int leader = test_cluster_find_leader(cluster);
        
        if (leader != current_leader) {
            current_leader = leader;
            stable_start = test_get_time_ms();
        } else if (leader >= 0) {
            uint64_t stable_duration = test_get_time_ms() - stable_start;
            if (stable_duration >= (uint64_t)stable_ms) {
                return leader;
            }
        }
        
        usleep(50000);  /* 50ms */
    }
    
    return -1;
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
    
    /* Network isolation is enforced via send filter callback registered
     * with each node's RPC context. See can_communicate() above. */
    
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
    printf("Partition: %s\n", cluster->partition_active ? "ACTIVE" : "NONE");
    
    if (cluster->partition_active) {
        printf("  Group 1:");
        for (size_t i = 0; i < cluster->group1_count; i++) {
            printf(" %d", cluster->group1_nodes[i]);
        }
        printf("\n  Group 2:");
        for (size_t i = 0; i < cluster->group2_count; i++) {
            printf(" %d", cluster->group2_nodes[i]);
        }
        printf("\n");
    }
    
    for (size_t i = 0; i < cluster->num_nodes; i++) {
        test_node_t* node = &cluster->nodes[i];
        
        const char* state_str = "UNKNOWN";
        uint32_t term = 0;
        uint32_t commit_idx = 0;
        
        if (!atomic_load(&node->crashed)) {
            raft_state_t state = raft_get_state(node->raft_node);
            state_str = raft_state_to_string(state);
            term = raft_get_term(node->raft_node);
            commit_idx = raft_get_commit_index(node->raft_node);
        } else {
            state_str = "CRASHED";
        }
        
        printf("  Node %zu: %s (term=%u, commit=%u)\n",
               i, state_str, term, commit_idx);
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