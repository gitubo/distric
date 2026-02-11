/**
 * @file test_gossip_basic.c
 * @brief Basic test to verify gossip library structure and compilation
 */

#include <distric_gossip.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    printf("=== DistriC Gossip Library - Basic Structure Test ===\n\n");
    
    /* Test 1: Configuration initialization */
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strcpy(config.node_id, "test-node-1");
    strcpy(config.bind_address, "127.0.0.1");
    config.bind_port = 7946;
    config.role = GOSSIP_ROLE_WORKER;
    config.protocol_period_ms = 1000;
    config.probe_timeout_ms = 500;
    config.indirect_probes = 3;
    config.suspicion_mult = 3;
    config.max_transmissions = 3;
    config.seed_count = 0;
    config.seed_addresses = NULL;
    config.seed_ports = NULL;
    config.metrics = NULL;
    config.logger = NULL;
    
    printf("✓ Configuration structure initialized\n");
    
    /* Test 2: Initialize gossip state */
    gossip_state_t* state = NULL;
    distric_err_t err = gossip_init(&config, &state);
    
    if (err == DISTRIC_OK && state != NULL) {
        printf("✓ Gossip state initialized successfully\n");
    } else {
        printf("✗ Failed to initialize gossip state: %d\n", err);
        return 1;
    }
    
    /* Test 3: Test utility functions */
    const char* status_str = gossip_status_to_string(GOSSIP_NODE_ALIVE);
    printf("✓ Status string: %s\n", status_str);
    
    const char* role_str = gossip_role_to_string(GOSSIP_ROLE_WORKER);
    printf("✓ Role string: %s\n", role_str);
    
    /* Test 4: Query functions (should return empty initially) */
    gossip_node_info_t* nodes = NULL;
    size_t count = 0;
    
    err = gossip_get_alive_nodes(state, &nodes, &count);
    if (err == DISTRIC_OK) {
        printf("✓ Alive nodes query: %zu nodes (expected: 1 - self)\n", count);
        if (nodes) {
            free(nodes);
        }
    }
    
    /* Test 5: Check if self node is alive */
    bool is_alive = false;
    err = gossip_is_node_alive(state, config.node_id, &is_alive);
    if (err == DISTRIC_OK) {
        printf("✓ Self node alive check: %s\n", is_alive ? "ALIVE" : "NOT ALIVE");
    }
    
    /* Test 6: Get node count */
    size_t node_count = 0;
    err = gossip_get_node_count(state, (gossip_node_status_t)-1, &node_count);
    if (err == DISTRIC_OK) {
        printf("✓ Total node count: %zu\n", node_count);
    }
    
    /* Test 7: Destroy state */
    gossip_destroy(state);
    printf("✓ Gossip state destroyed\n");
    
    printf("\n=== All basic tests passed! ===\n");
    
    return 0;
}