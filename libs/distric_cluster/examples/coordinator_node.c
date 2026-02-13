/**
 * @file coordinator_node.c
 * @brief Example Coordination Node Implementation
 * 
 * Demonstrates how to create and run a coordination node that:
 * - Participates in Raft consensus
 * - Uses Gossip for failure detection
 * - Manages worker pool
 * - Assigns tasks when leader
 * 
 * Usage:
 *   ./coordinator_node <node-id> <bind-addr> <gossip-port> <raft-port>
 * 
 * Example:
 *   ./coordinator_node coord-1 0.0.0.0 7946 8300
 */

#include <distric_cluster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile bool g_running = true;
static cluster_coordinator_t* g_coordinator = NULL;

/* ============================================================================
 * SIGNAL HANDLING
 * ========================================================================= */

static void signal_handler(int signum) {
    (void)signum;
    printf("\nReceived shutdown signal...\n");
    g_running = false;
}

/* ============================================================================
 * CALLBACKS
 * ========================================================================= */

static void on_became_leader(cluster_coordinator_t* coord, void* user_data) {
    (void)coord;
    (void)user_data;
    
    printf("*** THIS NODE IS NOW THE RAFT LEADER ***\n");
    printf("    Starting task distribution...\n");
}

static void on_lost_leadership(cluster_coordinator_t* coord, void* user_data) {
    (void)coord;
    (void)user_data;
    
    printf("*** THIS NODE LOST RAFT LEADERSHIP ***\n");
    printf("    Stopping task distribution...\n");
}

static void on_worker_joined(cluster_coordinator_t* coord,
                             const cluster_node_t* worker,
                             void* user_data) {
    (void)coord;
    (void)user_data;
    
    printf("[WORKER JOIN] %s (%s:%u)\n",
           worker->node_id,
           worker->address,
           worker->gossip_port);
}

static void on_worker_failed(cluster_coordinator_t* coord,
                             const cluster_node_t* worker,
                             void* user_data) {
    (void)coord;
    (void)user_data;
    
    printf("[WORKER FAIL] %s (%s:%u)\n",
           worker->node_id,
           worker->address,
           worker->gossip_port);
}

/* ============================================================================
 * TASK ASSIGNMENT SIMULATION
 * ========================================================================= */

static void assign_dummy_task(cluster_coordinator_t* coord) {
    /* Select an available worker */
    char worker_id[64];
    distric_err_t err = cluster_coordinator_select_worker(coord, worker_id,
                                                          sizeof(worker_id));
    
    if (err == DISTRIC_OK) {
        printf("[TASK] Assigned dummy task to worker: %s\n", worker_id);
        
        /* Update load (simulate task assignment) */
        cluster_coordinator_update_worker_load(coord, worker_id, +1);
        
        /* In real implementation, send task via RPC and update load on completion */
    } else {
        printf("[TASK] No workers available for task assignment\n");
    }
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <node-id> <bind-addr> <gossip-port> <raft-port>\n",
                argv[0]);
        fprintf(stderr, "Example: %s coord-1 0.0.0.0 7946 8300\n", argv[0]);
        return 1;
    }
    
    const char* node_id = argv[1];
    const char* bind_addr = argv[2];
    uint16_t gossip_port = (uint16_t)atoi(argv[3]);
    uint16_t raft_port = (uint16_t)atoi(argv[4]);
    
    printf("=======================================================\n");
    printf("  DistriC Coordination Node\n");
    printf("=======================================================\n");
    printf("Node ID:     %s\n", node_id);
    printf("Bind Addr:   %s\n", bind_addr);
    printf("Gossip Port: %u\n", gossip_port);
    printf("Raft Port:   %u\n", raft_port);
    printf("=======================================================\n\n");
    
    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Configure coordinator */
    cluster_config_t config = {
        .node_type = CLUSTER_NODE_COORDINATOR,
        .gossip_port = gossip_port,
        .raft_port = raft_port,
        
        /* Gossip configuration */
        .gossip_interval_ms = 1000,
        .gossip_suspect_timeout_ms = 5000,
        
        /* Raft configuration */
        .raft_election_timeout_min_ms = 150,
        .raft_election_timeout_max_ms = 300,
        .raft_heartbeat_interval_ms = 50,
        
        /* For demo, assume 3-node cluster */
        .raft_peer_count = 3,
        
        .metrics = NULL,
        .logger = NULL,
        .tracer = NULL
    };
    
    strncpy(config.node_id, node_id, sizeof(config.node_id) - 1);
    strncpy(config.bind_address, bind_addr, sizeof(config.bind_address) - 1);
    
    /* Configure Raft peers (example with 3 coordinators) */
    snprintf(config.raft_peers[0], sizeof(config.raft_peers[0]),
             "coord-1@192.168.1.10:%u", raft_port);
    snprintf(config.raft_peers[1], sizeof(config.raft_peers[1]),
             "coord-2@192.168.1.11:%u", raft_port);
    snprintf(config.raft_peers[2], sizeof(config.raft_peers[2]),
             "coord-3@192.168.1.12:%u", raft_port);
    
    /* Gossip seeds (other coordinators) */
    snprintf(config.gossip_seeds[0], sizeof(config.gossip_seeds[0]),
             "192.168.1.10:%u", gossip_port);
    snprintf(config.gossip_seeds[1], sizeof(config.gossip_seeds[1]),
             "192.168.1.11:%u", gossip_port);
    config.gossip_seed_count = 2;
    
    snprintf(config.storage_path, sizeof(config.storage_path),
             "/tmp/distric-%s", node_id);
    
    /* Create coordinator */
    printf("Creating coordinator...\n");
    distric_err_t err = cluster_coordinator_create(&config, &g_coordinator);
    if (err != DISTRIC_OK) {
        fprintf(stderr, "Failed to create coordinator: %d\n", err);
        return 1;
    }
    printf("✓ Coordinator created\n\n");
    
    /* Set callbacks */
    cluster_coordinator_set_leadership_callbacks(g_coordinator,
                                                 on_became_leader,
                                                 on_lost_leadership,
                                                 NULL);
    
    cluster_coordinator_set_worker_callbacks(g_coordinator,
                                             on_worker_joined,
                                             on_worker_failed,
                                             NULL);
    
    /* Start coordinator */
    printf("Starting coordinator...\n");
    err = cluster_coordinator_start(g_coordinator);
    if (err != DISTRIC_OK) {
        fprintf(stderr, "Failed to start coordinator: %d\n", err);
        cluster_coordinator_destroy(g_coordinator);
        return 1;
    }
    printf("✓ Coordinator started\n\n");
    
    printf("Coordinator running. Press Ctrl+C to stop.\n\n");
    
    /* Main loop */
    int tick = 0;
    while (g_running) {
        sleep(5);
        tick++;
        
        /* Check if we're the leader */
        bool is_leader = false;
        cluster_coordinator_is_leader(g_coordinator, &is_leader);
        
        /* Get cluster stats */
        size_t coordinator_count = 0;
        size_t worker_count = 0;
        cluster_coordinator_get_node_count(g_coordinator,
                                          CLUSTER_NODE_COORDINATOR,
                                          &coordinator_count);
        cluster_coordinator_get_node_count(g_coordinator,
                                          CLUSTER_NODE_WORKER,
                                          &worker_count);
        
        printf("[Tick %d] Leader: %s | Coordinators: %zu | Workers: %zu\n",
               tick,
               is_leader ? "YES" : "NO",
               coordinator_count,
               worker_count);
        
        /* If leader, simulate task assignment every 10 seconds */
        if (is_leader && tick % 2 == 0) {
            assign_dummy_task(g_coordinator);
        }
    }
    
    /* Cleanup */
    printf("\nShutting down coordinator...\n");
    cluster_coordinator_stop(g_coordinator);
    cluster_coordinator_destroy(g_coordinator);
    printf("✓ Coordinator stopped\n");
    
    return 0;
}