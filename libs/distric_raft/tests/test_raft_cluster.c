/**
 * @file test_raft_cluster_simple.c
 * @brief Simple 3-Node Raft Cluster Test - FINAL VERSION
 * 
 * Key Features:
 * 1. Single-node control test (verifies election logic works)
 * 2. Explicit startup barrier (wait for all FOLLOWER)
 * 3. RPC traffic sanity check (via metrics)
 * 4. Term explosion detection
 * 5. Vote table diagnostics
 * 
 * This test uses the EXISTING metrics in raft_rpc.c to detect RPC traffic.
 * No library modifications required.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <distric_raft.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>

/* ============================================================================
 * CONFIGURATION
 * ========================================================================= */

#define DATA_DIR "/tmp/raft_simple_test"
#define MAX_NODES 3
#define BASE_PORT 25000

/* Timing */
#define ELECTION_TIMEOUT_MIN_MS 150
#define ELECTION_TIMEOUT_MAX_MS 600
#define HEARTBEAT_INTERVAL_MS 50
#define TICK_INTERVAL_MS 10

/* Test timeouts */
#define BARRIER_TIMEOUT_MS 2000
#define SANITY_CHECK_DELAY_MS 2000  /* Wait before checking RPC traffic */
#define LEADER_WAIT_TIMEOUT_MS 8000
#define STABILITY_CHECK_MS 2000

/* Health */
#define MAX_HEALTHY_TERM 20
#define POLL_INTERVAL_MS 100

/* ============================================================================
 * NODE CONTEXT
 * ========================================================================= */

typedef struct {
    char node_id[64];
    uint16_t port;
    
    raft_node_t* node;
    raft_rpc_context_t* rpc;
    
    pthread_t tick_thread;
    _Atomic bool running;
    
} node_ctx_t;

/* ============================================================================
 * GLOBALS
 * ========================================================================= */

static node_ctx_t nodes[MAX_NODES];
static size_t node_count = 0;
static metrics_registry_t* metrics = NULL;
static logger_t* logger = NULL;

/* Metric pointers for RPC traffic detection */
//static metric_t* request_vote_sent_total = NULL;
//static metric_t* request_vote_received_total = NULL;

/* ============================================================================
 * UTILITIES
 * ========================================================================= */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void* tick_thread_func(void* arg) {
    node_ctx_t* ctx = (node_ctx_t*)arg;
    uint32_t last_broadcast_term = 0;
    
    while (atomic_load(&ctx->running)) {
        raft_tick(ctx->node);
        
        raft_state_t state = raft_get_state(ctx->node);
        uint32_t current_term = raft_get_term(ctx->node);
        
        /* Broadcast votes when new election starts */
        if (state == RAFT_STATE_CANDIDATE && current_term > last_broadcast_term) {
            uint32_t votes = 0;
            raft_rpc_broadcast_request_vote(ctx->rpc, ctx->node, &votes);
            last_broadcast_term = current_term;
            
            /* ✅ FIX: Process election result */
            raft_process_election_result(ctx->node, votes);
        }
        
        /* Send heartbeats as leader */
        if (state == RAFT_STATE_LEADER && raft_should_send_heartbeat(ctx->node)) {
            raft_rpc_broadcast_append_entries(ctx->rpc, ctx->node);
            raft_mark_heartbeat_sent(ctx->node);
        }
        
        usleep(TICK_INTERVAL_MS * 1000);
    }
    return NULL;
}

/* ============================================================================
 * CLUSTER QUERIES
 * ========================================================================= */

static node_ctx_t* find_leader(void) {
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].node && raft_is_leader(nodes[i].node)) {
            return &nodes[i];
        }
    }
    return NULL;
}

static size_t count_leaders(void) {
    size_t count = 0;
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].node && raft_is_leader(nodes[i].node)) {
            count++;
        }
    }
    return count;
}

static uint32_t get_max_term(void) {
    uint32_t max_term = 0;
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].node) {
            uint32_t term = raft_get_term(nodes[i].node);
            if (term > max_term) max_term = term;
        }
    }
    return max_term;
}

/* ============================================================================
 * METRICS-BASED RPC TRAFFIC DETECTION
 * ========================================================================= */

/**
 * Get metric value by searching in Prometheus export
 * This is a workaround since we don't have direct metric access
 */
static uint64_t get_metric_value(const char* metric_name) {
    char* prometheus_text = NULL;
    size_t text_size = 0;
    
    if (metrics_export_prometheus(metrics, &prometheus_text, &text_size) != DISTRIC_OK) {
        return 0;
    }
    
    /* Search for metric_name in the text */
    char search_str[256];
    snprintf(search_str, sizeof(search_str), "%s ", metric_name);
    
    char* line = strstr(prometheus_text, search_str);
    uint64_t value = 0;
    
    if (line) {
        /* Parse the value after the metric name */
        while (*line && *line != ' ') line++;
        if (*line == ' ') {
            value = strtoull(line + 1, NULL, 10);
        }
    }
    
    free(prometheus_text);
    return value;
}

/**
 * CRITICAL SANITY CHECK: Verify RPC traffic is happening
 */
static bool sanity_check_rpc_traffic(void) {
    printf("  [SANITY] Checking for RequestVote RPC traffic...\n");
    
    uint64_t sent = get_metric_value("raft_request_vote_sent_total");
    uint64_t received = get_metric_value("raft_request_vote_received_total");
    
    printf("  [SANITY] RequestVote RPCs: sent=%llu, received=%llu\n",
           (unsigned long long)sent, (unsigned long long)received);
    
    if (sent == 0 && received == 0) {
        printf("\n");
        printf("  ╔═══════════════════════════════════════════════════════════╗\n");
        printf("  ║  ✗ CRITICAL FAILURE: No RequestVote traffic observed!    ║\n");
        printf("  ╚═══════════════════════════════════════════════════════════╝\n");
        printf("\n");
        printf("  This indicates one of:\n");
        printf("   1. RPC networking is broken (nodes can't communicate)\n");
        printf("   2. Raft nodes never became CANDIDATE (stuck as FOLLOWER)\n");
        printf("   3. Election timeouts not triggering\n");
        printf("\n");
        printf("  Diagnostic steps:\n");
        printf("   - Check: Are RPC servers listening?\n");
        printf("   - Check: Are nodes reaching CANDIDATE state?\n");
        printf("   - Check: Are election timeouts being triggered?\n");
        printf("\n");
        return false;
    }
    
    if (sent > 0 && received == 0) {
        printf("\n");
        printf("  ╔═══════════════════════════════════════════════════════════╗\n");
        printf("  ║  ✗ FAILURE: RequestVotes sent but none received!         ║\n");
        printf("  ╚═══════════════════════════════════════════════════════════╝\n");
        printf("\n");
        printf("  This indicates:\n");
        printf("   - Nodes can send RPCs but not receive them\n");
        printf("   - Possible firewall or port binding issue\n");
        printf("\n");
        return false;
    }
    
    if (sent == 0 && received > 0) {
        printf("\n");
        printf("  ╔═══════════════════════════════════════════════════════════╗\n");
        printf("  ║  ⚠ WARNING: RequestVotes received but none sent!         ║\n");
        printf("  ╚═══════════════════════════════════════════════════════════╝\n");
        printf("\n");
        printf("  This is unusual but may resolve. Continuing...\n");
        printf("\n");
    }
    
    printf("  [SANITY] ✓ RPC traffic verified\n\n");
    return true;
}

/* ============================================================================
 * BARRIER: Wait for FOLLOWER state
 * ========================================================================= */

static bool wait_until_all_followers(uint64_t timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;
    
    printf("  [BARRIER] Waiting for all nodes to become FOLLOWER...\n");
    
    while (now_ms() < deadline) {
        bool all_followers = true;
        
        for (size_t i = 0; i < node_count; i++) {
            if (!nodes[i].node) {
                all_followers = false;
                break;
            }
            
            raft_state_t state = raft_get_state(nodes[i].node);
            if (state != RAFT_STATE_FOLLOWER) {
                all_followers = false;
                break;
            }
        }
        
        if (all_followers) {
            printf("  [BARRIER] ✓ All nodes are FOLLOWER\n");
            return true;
        }
        
        usleep(POLL_INTERVAL_MS * 1000);
    }
    
    printf("  [BARRIER] ✗ Timeout - not all nodes reached FOLLOWER\n");
    
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].node) {
            raft_state_t state = raft_get_state(nodes[i].node);
            printf("    %s: state=%s\n", nodes[i].node_id, 
                   raft_state_to_string(state));
        }
    }
    
    return false;
}

/* ============================================================================
 * DIAGNOSTICS
 * ========================================================================= */

static void print_vote_table(void) {
    printf("\n  [VOTE TABLE]\n");
    printf("  ┌───────────┬──────┬─────────────┐\n");
    printf("  │ Node      │ Term │ State       │\n");
    printf("  ├───────────┼──────┼─────────────┤\n");
    
    for (size_t i = 0; i < node_count; i++) {
        if (!nodes[i].node) continue;
        
        uint32_t term = raft_get_term(nodes[i].node);
        raft_state_t state = raft_get_state(nodes[i].node);
        
        printf("  │ %-9s │ %4u │ %-11s │\n",
               nodes[i].node_id, term, raft_state_to_string(state));
    }
    
    printf("  └───────────┴──────┴─────────────┘\n\n");
}

/* ============================================================================
 * WAIT FOR LEADER
 * ========================================================================= */

static bool wait_for_leader(uint64_t timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;
    uint32_t last_warn_term = 0;
    
    printf("  [ELECTION] Waiting for leader (timeout: %llu ms)...\n",
           (unsigned long long)timeout_ms);
    
    while (now_ms() < deadline) {
        node_ctx_t* leader = find_leader();
        
        if (leader) {
            size_t leader_count = count_leaders();
            
            if (leader_count == 1) {
                uint32_t term = raft_get_term(leader->node);
                printf("  [ELECTION] ✓ Leader elected: %s (term=%u)\n",
                       leader->node_id, term);
                
                if (term > MAX_HEALTHY_TERM) {
                    printf("  [WARNING] Term=%u is high (threshold=%d)\n",
                           term, MAX_HEALTHY_TERM);
                }
                
                return true;
            }
        }
        
        /* Health check */
        uint32_t max_term = get_max_term();
        
        if (max_term > MAX_HEALTHY_TERM && max_term > last_warn_term) {
            printf("  [WARNING] Term=%u exceeded healthy threshold (%d)\n",
                   max_term, MAX_HEALTHY_TERM);
            printf("  [WARNING] Likely election livelock\n");
            last_warn_term = max_term;
        }
        
        /* Status */
        static uint64_t last_print = 0;
        uint64_t now = now_ms();
        
        if (now - last_print >= 1000) {
            printf("  [ELECTION] ... (");
            for (size_t i = 0; i < node_count; i++) {
                if (nodes[i].node) {
                    raft_state_t state = raft_get_state(nodes[i].node);
                    uint32_t term = raft_get_term(nodes[i].node);
                    printf("%s=%s(t=%u)", nodes[i].node_id,
                           raft_state_to_string(state), term);
                    if (i < node_count - 1) printf(", ");
                }
            }
            printf(")\n");
            last_print = now;
        }
        
        usleep(POLL_INTERVAL_MS * 1000);
    }
    
    printf("  [ELECTION] ✗ Timeout - no leader elected\n\n");
    print_vote_table();
    
    return false;
}

/* ============================================================================
 * STABILITY CHECK
 * ========================================================================= */

static bool verify_leader_stable(const char* leader_id, uint64_t duration_ms) {
    uint64_t deadline = now_ms() + duration_ms;
    
    printf("  [STABILITY] Monitoring leader for %llu ms...\n",
           (unsigned long long)duration_ms);
    
    while (now_ms() < deadline) {
        node_ctx_t* leader = find_leader();
        
        if (!leader || strcmp(leader->node_id, leader_id) != 0) {
            printf("  [STABILITY] ✗ Leader changed or lost!\n");
            return false;
        }
        
        usleep(POLL_INTERVAL_MS * 1000);
    }
    
    printf("  [STABILITY] ✓ Leader stable: %s\n", leader_id);
    return true;
}

/* ============================================================================
 * CONTROL TEST
 * ========================================================================= */

static bool test_single_node_election(void) {
    printf("\n═══ CONTROL TEST: Single Node Self-Election ═══\n");
    
    node_ctx_t solo;
    memset(&solo, 0, sizeof(solo));
    
    snprintf(solo.node_id, sizeof(solo.node_id), "node-solo");
    solo.port = BASE_PORT;
    
    raft_config_t config = {
        .peers = NULL,
        .peer_count = 0,
        .election_timeout_min_ms = ELECTION_TIMEOUT_MIN_MS,
        .election_timeout_max_ms = ELECTION_TIMEOUT_MAX_MS,
        .heartbeat_interval_ms = HEARTBEAT_INTERVAL_MS,
        .snapshot_threshold = 100,
        .persistence_data_dir = NULL,
        .apply_fn = NULL,
        .user_data = &solo,
        .metrics = metrics,
        .logger = logger
    };
    strncpy(config.node_id, solo.node_id, sizeof(config.node_id) - 1);
    
    if (raft_create(&config, &solo.node) != DISTRIC_OK) {
        printf("  ✗ Failed to create node\n");
        return false;
    }
    
    if (raft_start(solo.node) != DISTRIC_OK) {
        printf("  ✗ Failed to start node\n");
        raft_destroy(solo.node);
        return false;
    }
    
    atomic_store(&solo.running, true);
    pthread_create(&solo.tick_thread, NULL, tick_thread_func, &solo);
    
    printf("  Waiting for self-election...\n");
    
    uint64_t deadline = now_ms() + 1000;
    bool elected = false;
    
    while (now_ms() < deadline) {
        if (raft_is_leader(solo.node)) {
            uint32_t term = raft_get_term(solo.node);
            printf("  ✓ Self-elected as leader (term=%u)\n", term);
            elected = true;
            break;
        }
        usleep(10000);
    }
    
    atomic_store(&solo.running, false);
    pthread_join(solo.tick_thread, NULL);
    raft_stop(solo.node);
    raft_destroy(solo.node);
    
    if (!elected) {
        printf("\n");
        printf("  ╔═══════════════════════════════════════════════════════════╗\n");
        printf("  ║  ✗ CRITICAL: Single node did NOT self-elect!             ║\n");
        printf("  ╚═══════════════════════════════════════════════════════════╝\n");
        printf("\n");
        printf("  This indicates BROKEN election logic in raft_core.c!\n");
        printf("  No point testing 3-node cluster.\n");
        printf("\n");
        return false;
    }
    
    printf("  ✓ Control test passed\n");
    return true;
}

/* ============================================================================
 * SETUP
 * ========================================================================= */

static bool setup_environment(void) {
    printf("\n═══ SETUP ═══\n");
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    srand((unsigned int)(ts.tv_sec ^ ts.tv_nsec));
    
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", DATA_DIR);
    system(cmd);
    mkdir(DATA_DIR, 0755);
    
    if (metrics_init(&metrics) != DISTRIC_OK) {
        printf("  ✗ Failed to initialize metrics\n");
        return false;
    }
    
    if (log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC) != DISTRIC_OK) {
        printf("  ✗ Failed to initialize logger\n");
        metrics_destroy(metrics);
        return false;
    }
    
    printf("  ✓ Environment ready\n");
    return true;
}

/* ============================================================================
 * NODE MANAGEMENT
 * ========================================================================= */

static bool create_node(size_t index, size_t total) {
    node_ctx_t* ctx = &nodes[index];
    memset(ctx, 0, sizeof(*ctx));
    
    snprintf(ctx->node_id, sizeof(ctx->node_id), "node-%zu", index);
    ctx->port = BASE_PORT + index;
    
    raft_peer_t* peers = NULL;
    size_t peer_count = total - 1;
    
    if (peer_count > 0) {
        peers = calloc(peer_count, sizeof(raft_peer_t));
        if (!peers) return false;
        
        size_t p = 0;
        for (size_t i = 0; i < total; i++) {
            if (i == index) continue;
            snprintf(peers[p].node_id, sizeof(peers[p].node_id), "node-%zu", i);
            strcpy(peers[p].address, "127.0.0.1");
            peers[p].port = BASE_PORT + i;
            p++;
        }
    }
    
    raft_config_t config = {
        .peers = peers,
        .peer_count = peer_count,
        .election_timeout_min_ms = ELECTION_TIMEOUT_MIN_MS,
        .election_timeout_max_ms = ELECTION_TIMEOUT_MAX_MS,
        .heartbeat_interval_ms = HEARTBEAT_INTERVAL_MS,
        .snapshot_threshold = 100,
        .persistence_data_dir = NULL,
        .apply_fn = NULL,
        .user_data = ctx,
        .metrics = metrics,
        .logger = logger
    };
    strncpy(config.node_id, ctx->node_id, sizeof(config.node_id) - 1);
    
    distric_err_t err = raft_create(&config, &ctx->node);
    free(peers);
    if (err != DISTRIC_OK) return false;
    
    raft_rpc_config_t rpc_cfg = {
        .bind_address = "127.0.0.1",
        .bind_port = ctx->port,
        .rpc_timeout_ms = 3000,
        .max_retries = 3,
        .metrics = metrics,
        .logger = logger
    };
    
    err = raft_rpc_create(&rpc_cfg, ctx->node, &ctx->rpc);
    if (err != DISTRIC_OK) {
        raft_destroy(ctx->node);
        return false;
    }
    
    return true;
}

static void stop_node(size_t i) {
    if (!nodes[i].node) return;
    
    if (atomic_load(&nodes[i].running)) {
        atomic_store(&nodes[i].running, false);
        pthread_join(nodes[i].tick_thread, NULL);
    }
    
    raft_stop(nodes[i].node);
    raft_rpc_stop(nodes[i].rpc);
}

static void cleanup(void) {
    printf("\n═══ CLEANUP ═══\n");
    
    for (size_t i = 0; i < node_count; i++) {
        stop_node(i);
        if (nodes[i].rpc) raft_rpc_destroy(nodes[i].rpc);
        if (nodes[i].node) raft_destroy(nodes[i].node);
    }
    
    if (logger) log_destroy(logger);
    if (metrics) metrics_destroy(metrics);
    
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", DATA_DIR);
    system(cmd);
    
    printf("  ✓ Cleanup complete\n");
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║  Raft Cluster Test - FINAL (with RPC traffic detection)       ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    
    bool success = false;
    
    if (!setup_environment()) return 1;
    
    /* CRITICAL: Control test first */
    if (!test_single_node_election()) {
        cleanup();
        return 1;
    }
    
    /* 3-node cluster test */
    printf("\n═══ CREATING 3-NODE CLUSTER ═══\n");
    
    node_count = 3;
    for (size_t i = 0; i < node_count; i++) {
        if (!create_node(i, node_count)) {
            printf("  ✗ Failed to create node-%zu\n", i);
            goto cleanup;
        }
        printf("  ✓ Created: node-%zu (port %u)\n", i, nodes[i].port);
    }
    
    /* Start RPC servers */
    printf("\n═══ STARTING RPC SERVERS ═══\n");
    for (size_t i = 0; i < node_count; i++) {
        if (raft_rpc_start(nodes[i].rpc) != DISTRIC_OK) {
            printf("  ✗ Failed to start RPC for node-%zu\n", i);
            goto cleanup;
        }
        printf("  ✓ RPC started: node-%zu\n", i);
    }
    
    printf("\n  Waiting for RPC servers to settle (1s)...\n");
    usleep(1000000);
    
    /* Start Raft nodes */
    printf("\n═══ STARTING RAFT NODES ═══\n");
    for (size_t i = 0; i < node_count; i++) {
        if (raft_start(nodes[i].node) != DISTRIC_OK) {
            printf("  ✗ Failed to start node-%zu\n", i);
            goto cleanup;
        }
        
        atomic_store(&nodes[i].running, true);
        pthread_create(&nodes[i].tick_thread, NULL, tick_thread_func, &nodes[i]);
        
        printf("  ✓ Started: node-%zu\n", i);
        usleep(250000);
    }
    
    /* BARRIER */
    printf("\n═══ STARTUP BARRIER ═══\n");
    if (!wait_until_all_followers(BARRIER_TIMEOUT_MS)) {
        printf("\n✗ BARRIER FAILED\n");
        goto cleanup;
    }
    
    printf("\n  Settling (300ms)...\n");
    usleep(300000);
    
    /* CRITICAL SANITY CHECK */
    printf("\n═══ RPC TRAFFIC SANITY CHECK ═══\n");
    printf("  Waiting %d ms for election activity...\n", SANITY_CHECK_DELAY_MS);
    usleep(SANITY_CHECK_DELAY_MS * 1000);
    
    if (!sanity_check_rpc_traffic()) {
        printf("\n✗ SANITY CHECK FAILED - NO RPC TRAFFIC\n");
        goto cleanup;
    }
    
    /* Wait for leader */
    printf("\n═══ LEADER ELECTION ═══\n");
    if (!wait_for_leader(LEADER_WAIT_TIMEOUT_MS)) {
        printf("\n✗ LEADER ELECTION FAILED\n");
        goto cleanup;
    }
    
    /* Verify */
    printf("\n═══ VERIFICATION ═══\n");
    
    if (count_leaders() != 1) {
        printf("  ✗ Expected 1 leader, found %zu\n", count_leaders());
        goto cleanup;
    }
    
    node_ctx_t* leader = find_leader();
    printf("  ✓ Exactly 1 leader: %s\n", leader->node_id);
    
    /* Stability */
    printf("\n═══ STABILITY CHECK ═══\n");
    if (!verify_leader_stable(leader->node_id, STABILITY_CHECK_MS)) {
        printf("\n✗ STABILITY FAILED\n");
        goto cleanup;
    }
    
    success = true;

cleanup:
    cleanup();
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    if (success) {
        printf("║  ✓✓✓ TEST PASSED - Leader election successful ✓✓✓            ║\n");
    } else {
        printf("║  ✗✗✗ TEST FAILED - See diagnostics above ✗✗✗                ║\n");
    }
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    return success ? 0 : 1;
}