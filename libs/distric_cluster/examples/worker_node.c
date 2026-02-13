/**
 * @file worker_node.c
 * @brief Example Worker Node Implementation
 * 
 * Demonstrates how to create and run a worker node that:
 * - Participates in Gossip protocol
 * - Reports health and load to coordinators
 * - Executes tasks (simulated)
 * 
 * Usage:
 *   ./worker_node <node-id> <bind-addr> <gossip-port> <max-tasks>
 * 
 * Example:
 *   ./worker_node worker-1 0.0.0.0 7946 50
 */

#include <distric_cluster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

static volatile bool g_running = true;
static cluster_coordinator_t* g_worker = NULL;
static uint32_t g_current_tasks = 0;

/* ============================================================================
 * SIGNAL HANDLING
 * ========================================================================= */

static void signal_handler(int signum) {
    (void)signum;
    printf("\nReceived shutdown signal...\n");
    g_running = false;
}

/* ============================================================================
 * TASK EXECUTION SIMULATION
 * ========================================================================= */

static void* execute_task(void* arg) {
    int task_id = *(int*)arg;
    free(arg);
    
    printf("[TASK %d] Started execution\n", task_id);
    
    /* Simulate task execution (2-5 seconds) */
    int duration = 2 + (rand() % 4);
    sleep(duration);
    
    __sync_fetch_and_sub(&g_current_tasks, 1);
    
    printf("[TASK %d] Completed (took %d seconds)\n", task_id, duration);
    
    return NULL;
}

static void simulate_task_execution(void) {
    /* Random chance of receiving a task */
    if (rand() % 100 < 20) {  /* 20% chance each tick */
        int* task_id = malloc(sizeof(int));
        *task_id = rand() % 10000;
        
        __sync_fetch_and_add(&g_current_tasks, 1);
        
        pthread_t thread;
        pthread_create(&thread, NULL, execute_task, task_id);
        pthread_detach(thread);
    }
}

static void update_load_metric(void) {
    /* In real implementation, this would be done via RPC or shared state */
    /* For now, we'll update local gossip load metric */
    if (g_worker) {
        /* gossip_update_load(g_worker->gossip, g_current_tasks); */
    }
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <node-id> <bind-addr> <gossip-port> <max-tasks>\n",
                argv[0]);
        fprintf(stderr, "Example: %s worker-1 0.0.0.0 7946 50\n", argv[0]);
        return 1;
    }
    
    const char* node_id = argv[1];
    const char* bind_addr = argv[2];
    uint16_t gossip_port = (uint16_t)atoi(argv[3]);
    uint32_t max_tasks = (uint32_t)atoi(argv[4]);
    
    srand(time(NULL));
    
    printf("=======================================================\n");
    printf("  DistriC Worker Node\n");
    printf("=======================================================\n");
    printf("Node ID:      %s\n", node_id);
    printf("Bind Addr:    %s\n", bind_addr);
    printf("Gossip Port:  %u\n", gossip_port);
    printf("Max Tasks:    %u\n", max_tasks);
    printf("=======================================================\n\n");
    
    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Configure worker */
    cluster_config_t config = {
        .node_type = CLUSTER_NODE_WORKER,
        .gossip_port = gossip_port,
        .max_concurrent_tasks = max_tasks,
        
        /* Gossip configuration */
        .gossip_interval_ms = 1000,
        .gossip_suspect_timeout_ms = 5000,
        
        .metrics = NULL,
        .logger = NULL,
        .tracer = NULL
    };
    
    strncpy(config.node_id, node_id, sizeof(config.node_id) - 1);
    strncpy(config.bind_address, bind_addr, sizeof(config.bind_address) - 1);
    
    /* Gossip seeds (coordinators) */
    snprintf(config.gossip_seeds[0], sizeof(config.gossip_seeds[0]),
             "192.168.1.10:7946");
    snprintf(config.gossip_seeds[1], sizeof(config.gossip_seeds[1]),
             "192.168.1.11:7946");
    snprintf(config.gossip_seeds[2], sizeof(config.gossip_seeds[2]),
             "192.168.1.12:7946");
    config.gossip_seed_count = 3;
    
    /* Create worker */
    printf("Creating worker...\n");
    distric_err_t err = cluster_coordinator_create(&config, &g_worker);
    if (err != DISTRIC_OK) {
        fprintf(stderr, "Failed to create worker: %d\n", err);
        return 1;
    }
    printf("✓ Worker created\n\n");
    
    /* Start worker */
    printf("Starting worker...\n");
    err = cluster_coordinator_start(g_worker);
    if (err != DISTRIC_OK) {
        fprintf(stderr, "Failed to start worker: %d\n", err);
        cluster_coordinator_destroy(g_worker);
        return 1;
    }
    printf("✓ Worker started\n\n");
    
    printf("Worker running. Press Ctrl+C to stop.\n\n");
    
    /* Main loop */
    int tick = 0;
    while (g_running) {
        sleep(2);
        tick++;
        
        /* Simulate task execution */
        simulate_task_execution();
        
        /* Update load metric */
        update_load_metric();
        
        /* Print status */
        double utilization = (double)g_current_tasks / (double)max_tasks * 100.0;
        printf("[Tick %d] Tasks: %u/%u (%.1f%% utilization)\n",
               tick,
               g_current_tasks,
               max_tasks,
               utilization);
    }
    
    /* Cleanup */
    printf("\nShutting down worker...\n");
    cluster_coordinator_stop(g_worker);
    cluster_coordinator_destroy(g_worker);
    printf("✓ Worker stopped\n");
    
    return 0;
}