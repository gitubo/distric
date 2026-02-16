/**
 * @file config_manager_example.c
 * @brief Example demonstrating Configuration Manager usage
 * 
 * This example shows how to:
 * 1. Create and start a configuration manager
 * 2. Add and manage node configurations
 * 3. Set and retrieve system parameters
 * 4. Query configuration state
 */

#include <distric_cluster/config_manager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Simple logging helper */
#define LOG(fmt, ...) \
    printf("[CONFIG_EXAMPLE] " fmt "\n", ##__VA_ARGS__)

/**
 * @brief Print configuration state
 */
static void print_config_state(config_state_t* state) {
    printf("\n");
    printf("=======================================================\n");
    printf("  Configuration State\n");
    printf("=======================================================\n");
    printf("Version:    %lu\n", state->version);
    printf("Timestamp:  %lu\n", state->timestamp_ms);
    printf("\n");
    
    printf("Nodes (%zu):\n", state->node_count);
    printf("-------------------------------------------------------\n");
    for (size_t i = 0; i < state->node_count; i++) {
        printf("  [%zu] %s\n", i, state->nodes[i].node_id);
        printf("      Role:    %s\n",
               config_node_role_to_string(state->nodes[i].role));
        printf("      Address: %s:%u\n",
               state->nodes[i].address, state->nodes[i].port);
        printf("      Enabled: %s\n",
               state->nodes[i].enabled ? "yes" : "no");
    }
    printf("\n");
    
    printf("Parameters (%zu):\n", state->param_count);
    printf("-------------------------------------------------------\n");
    for (size_t i = 0; i < state->param_count; i++) {
        printf("  %s = %s\n",
               state->params[i].key,
               state->params[i].value);
        if (state->params[i].description[0] != '\0') {
            printf("    (%s)\n", state->params[i].description);
        }
    }
    printf("\n");
    
    printf("Default System Parameters:\n");
    printf("-------------------------------------------------------\n");
    printf("  Gossip interval:          %u ms\n", state->gossip_interval_ms);
    printf("  Gossip suspect timeout:   %u ms\n", state->gossip_suspect_timeout_ms);
    printf("  Raft election timeout:    %u-%u ms\n",
           state->raft_election_timeout_min_ms,
           state->raft_election_timeout_max_ms);
    printf("  Raft heartbeat interval:  %u ms\n", state->raft_heartbeat_interval_ms);
    printf("  Task assignment timeout:  %u ms\n", state->task_assignment_timeout_ms);
    printf("  Task execution timeout:   %u ms\n", state->task_execution_timeout_ms);
    printf("  Max tasks per worker:     %u\n", state->max_tasks_per_worker);
    printf("=======================================================\n\n");
}

/**
 * @brief Main example function
 */
int main(int argc, char* argv[]) {
    printf("\n");
    printf("=======================================================\n");
    printf("  DistriC Configuration Manager Example\n");
    printf("=======================================================\n\n");
    
    /* For this example, we'll use a mock Raft node */
    /* In a real application, you would create a real Raft node */
    LOG("Note: This example uses a simplified mock for demonstration");
    LOG("In production, integrate with a real Raft node\n");
    
    /* Step 1: Create Raft node (simplified mock) */
    raft_node_t* raft = NULL;  /* Would be created with raft_create() */
    
    /* For demonstration, we'll skip actual Raft setup */
    LOG("Step 1: Create and start configuration manager");
    LOG("  (Skipping Raft setup for this example)\n");
    
    /* Normally you would:
     * 1. Create Raft configuration
     * 2. Create Raft node: raft_create(&config, &raft);
     * 3. Start Raft node: raft_start(raft);
     */
    
    /* Step 2: Create configuration manager */
    /* config_manager_t* manager = config_manager_create(raft, NULL, NULL); */
    LOG("Step 2: Configuration manager would be created here");
    LOG("  config_manager_t* manager = config_manager_create(raft, NULL, NULL);\n");
    
    /* Step 3: Start configuration manager */
    /* config_manager_start(manager); */
    LOG("Step 3: Start configuration manager");
    LOG("  config_manager_start(manager);\n");
    
    /* Step 4: Add coordinator nodes */
    LOG("Step 4: Add coordinator nodes (3-node Raft cluster)");
    printf("  Adding coordinator nodes:\n");
    
    const char* coord_ids[] = {"coord-1", "coord-2", "coord-3"};
    const char* coord_addrs[] = {"192.168.1.10", "192.168.1.11", "192.168.1.12"};
    
    for (int i = 0; i < 3; i++) {
        printf("    - %s @ %s:9090\n", coord_ids[i], coord_addrs[i]);
        
        /* In real code:
        config_change_t change = {
            .operation = CONFIG_OP_ADD_NODE,
            .node = {
                .role = CONFIG_NODE_ROLE_COORDINATOR,
                .port = 9090,
                .enabled = true
            }
        };
        strncpy(change.node.node_id, coord_ids[i], 
                sizeof(change.node.node_id) - 1);
        strncpy(change.node.address, coord_addrs[i],
                sizeof(change.node.address) - 1);
        
        uint64_t log_index;
        if (config_manager_propose_change(manager, &change, &log_index) == 0) {
            config_manager_wait_committed(manager, log_index, 5000);
            LOG("    Added %s", coord_ids[i]);
        }
        */
    }
    printf("\n");
    
    /* Step 5: Add worker nodes */
    LOG("Step 5: Add worker nodes");
    printf("  Adding worker nodes:\n");
    
    for (int i = 0; i < 10; i++) {
        printf("    - worker-%d @ 192.168.2.%d:8080\n", i + 1, 10 + i);
        
        /* In real code:
        config_change_t change = {
            .operation = CONFIG_OP_ADD_NODE,
            .node = {
                .role = CONFIG_NODE_ROLE_WORKER,
                .port = 8080,
                .enabled = true
            }
        };
        snprintf(change.node.node_id, sizeof(change.node.node_id),
                "worker-%d", i + 1);
        snprintf(change.node.address, sizeof(change.node.address),
                "192.168.2.%d", 10 + i);
        
        config_manager_propose_change(manager, &change, NULL);
        */
    }
    printf("\n");
    
    /* Step 6: Configure system parameters */
    LOG("Step 6: Configure system parameters");
    
    const char* param_keys[] = {
        "max_message_size",
        "worker_timeout_ms",
        "retry_attempts",
        "enable_compression"
    };
    const char* param_values[] = {
        "10485760",  /* 10 MB */
        "30000",     /* 30 seconds */
        "3",
        "true"
    };
    const char* param_descs[] = {
        "Maximum message size in bytes",
        "Worker health check timeout",
        "Number of retry attempts for failed tasks",
        "Enable data compression"
    };
    
    for (int i = 0; i < 4; i++) {
        printf("  Setting: %s = %s\n", param_keys[i], param_values[i]);
        printf("           (%s)\n", param_descs[i]);
        
        /* In real code:
        config_change_t change = {
            .operation = CONFIG_OP_SET_PARAM
        };
        strncpy(change.param.key, param_keys[i],
                sizeof(change.param.key) - 1);
        strncpy(change.param.value, param_values[i],
                sizeof(change.param.value) - 1);
        strncpy(change.param.description, param_descs[i],
                sizeof(change.param.description) - 1);
        
        config_manager_propose_change(manager, &change, NULL);
        */
    }
    printf("\n");
    
    /* Step 7: Query configuration */
    LOG("Step 7: Query configuration state");
    
    /* In real code:
    config_state_t* state = NULL;
    if (config_manager_get_state(manager, &state) == 0) {
        print_config_state(state);
        config_state_free(state);
    }
    */
    
    printf("  (Configuration state would be printed here)\n\n");
    
    /* Step 8: Query specific nodes */
    LOG("Step 8: Query specific node information");
    
    /* In real code:
    config_node_entry_t node;
    if (config_manager_get_node(manager, "worker-1", &node) == 0) {
        printf("  Found node: %s\n", node.node_id);
        printf("    Role:    %s\n", config_node_role_to_string(node.role));
        printf("    Address: %s:%u\n", node.address, node.port);
        printf("    Enabled: %s\n", node.enabled ? "yes" : "no");
    }
    */
    
    printf("  (Node details would be printed here)\n\n");
    
    /* Step 9: Query workers only */
    LOG("Step 9: Get all worker nodes");
    
    /* In real code:
    config_node_entry_t* workers = NULL;
    size_t worker_count = 0;
    if (config_manager_get_nodes_by_role(manager, CONFIG_NODE_ROLE_WORKER,
                                        &workers, &worker_count) == 0) {
        printf("  Found %zu worker nodes\n", worker_count);
        for (size_t i = 0; i < worker_count; i++) {
            printf("    [%zu] %s @ %s:%u\n", i, workers[i].node_id,
                   workers[i].address, workers[i].port);
        }
        free(workers);
    }
    */
    
    printf("  (Worker list would be printed here)\n\n");
    
    /* Step 10: Get parameter values */
    LOG("Step 10: Retrieve parameter values");
    
    /* In real code:
    char value[256];
    if (config_manager_get_param(manager, "max_message_size",
                                value, sizeof(value)) == 0) {
        printf("  max_message_size = %s\n", value);
    }
    
    int64_t timeout = config_manager_get_param_int(manager, "worker_timeout_ms", 0);
    printf("  worker_timeout_ms = %ld\n", timeout);
    */
    
    printf("  (Parameter values would be printed here)\n\n");
    
    /* Step 11: Update a node */
    LOG("Step 11: Update worker-5 (disable it for maintenance)");
    
    /* In real code:
    config_change_t change = {
        .operation = CONFIG_OP_UPDATE_NODE,
        .node = {
            .role = CONFIG_NODE_ROLE_WORKER,
            .port = 8080,
            .enabled = false
        }
    };
    strncpy(change.node.node_id, "worker-5", sizeof(change.node.node_id) - 1);
    strncpy(change.node.address, "192.168.2.14",
            sizeof(change.node.address) - 1);
    
    if (config_manager_propose_change(manager, &change, NULL) == 0) {
        LOG("  Worker-5 disabled successfully");
    }
    */
    
    printf("  (Worker update would happen here)\n\n");
    
    /* Step 12: Remove a node */
    LOG("Step 12: Remove worker-10 from cluster");
    
    /* In real code:
    config_change_t remove_change = {
        .operation = CONFIG_OP_REMOVE_NODE
    };
    strncpy(remove_change.node.node_id, "worker-10",
            sizeof(remove_change.node.node_id) - 1);
    
    if (config_manager_propose_change(manager, &remove_change, NULL) == 0) {
        LOG("  Worker-10 removed successfully");
    }
    */
    
    printf("  (Worker removal would happen here)\n\n");
    
    /* Step 13: Monitor version changes */
    LOG("Step 13: Monitor configuration version");
    
    /* In real code:
    uint64_t version = config_manager_get_version(manager);
    printf("  Current configuration version: %lu\n", version);
    */
    
    printf("  (Version monitoring would happen here)\n\n");
    
    /* Cleanup */
    LOG("Cleanup: Stop and destroy configuration manager");
    
    /* In real code:
    config_manager_stop(manager);
    config_manager_destroy(manager);
    raft_stop(raft);
    raft_destroy(raft);
    */
    
    printf("\n");
    printf("=======================================================\n");
    printf("  Example Complete\n");
    printf("=======================================================\n");
    printf("\n");
    printf("Key Takeaways:\n");
    printf("  1. Configuration manager replicates config via Raft\n");
    printf("  2. Only leader can propose configuration changes\n");
    printf("  3. All nodes can read configuration (consistent view)\n");
    printf("  4. Configuration changes are versioned and timestamped\n");
    printf("  5. Thread-safe for concurrent reads\n");
    printf("\n");
    printf("For a working example, integrate with:\n");
    printf("  - distric_raft library (Raft consensus)\n");
    printf("  - distric_gossip library (failure detection)\n");
    printf("  - distric_cluster library (cluster coordination)\n");
    printf("\n");
    
    return 0;
}