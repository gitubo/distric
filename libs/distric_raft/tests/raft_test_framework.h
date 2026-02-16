/**
 * @file raft_test_framework.h
 * @brief Reusable Test Framework for RAFT Consensus Tests
 * 
 * This framework provides cluster management, network simulation, and
 * utilities for testing the distric_raft library. It's based on the
 * working test_raft_cluster.c implementation.
 * 
 * Features:
 * - Multi-node cluster creation and management
 * - Network partition simulation
 * - Message delay and loss simulation
 * - Time control and synchronization
 * - Leader election helpers
 * - Log replication helpers
 * - Node crash and restart simulation
 * 
 * @version 1.0.0
 * @author DistriC Development Team
 */

#ifndef RAFT_TEST_FRAMEWORK_H
#define RAFT_TEST_FRAMEWORK_H

#include <distric_raft.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CONFIGURATION
 * ========================================================================= */

#define TEST_MAX_NODES 10
#define TEST_BASE_PORT 25000
#define TEST_DATA_DIR_PREFIX "/tmp/raft_test_"

/* Default timing */
#define TEST_DEFAULT_ELECTION_TIMEOUT_MIN_MS 150
#define TEST_DEFAULT_ELECTION_TIMEOUT_MAX_MS 300
#define TEST_DEFAULT_HEARTBEAT_INTERVAL_MS 50
#define TEST_DEFAULT_TICK_INTERVAL_MS 10

/* ============================================================================
 * TEST NODE STRUCTURE
 * ========================================================================= */

/**
 * @brief Test node context
 */
typedef struct test_node {
    char node_id[64];           /**< Node identifier */
    uint16_t port;              /**< RPC server port */
    
    raft_node_t* raft_node;     /**< RAFT node instance */
    raft_rpc_context_t* rpc;    /**< RPC context */
    
    pthread_t tick_thread;      /**< Tick thread handle */
    _Atomic bool running;       /**< Thread running flag */
    _Atomic bool crashed;       /**< Simulated crash state */
    
    /* Network simulation */
    _Atomic double message_loss_rate;  /**< 0.0-1.0, probability of dropping messages */
    uint32_t message_delay_min_ms;     /**< Minimum message delay */
    uint32_t message_delay_max_ms;     /**< Maximum message delay */
    
    /* Partition state */
    bool partitioned;           /**< Is this node partitioned? */
    int partition_group;        /**< Which partition group (0 or 1) */
    
    /* Back-reference to cluster for partition checking */
    struct test_cluster* cluster;  /**< Parent cluster (for partition checks) */
    
} test_node_t;

/* ============================================================================
 * TEST CLUSTER STRUCTURE
 * ========================================================================= */

/**
 * @brief Test cluster context
 */
typedef struct test_cluster {
    test_node_t nodes[TEST_MAX_NODES];  /**< Array of test nodes */
    size_t num_nodes;                   /**< Number of nodes in cluster */
    
    char data_dir[256];                 /**< Base data directory */
    
    metrics_registry_t* metrics;        /**< Shared metrics registry */
    logger_t* logger;                   /**< Shared logger */
    
    /* Network partition state */
    bool partition_active;              /**< Is a partition currently active? */
    int group1_nodes[TEST_MAX_NODES];   /**< Node IDs in partition group 1 */
    size_t group1_count;
    int group2_nodes[TEST_MAX_NODES];   /**< Node IDs in partition group 2 */
    size_t group2_count;
    
    /* Global timing */
    uint32_t tick_interval_ms;          /**< How often to tick nodes */
    
} test_cluster_t;

/* ============================================================================
 * FRAMEWORK INITIALIZATION
 * ========================================================================= */

/**
 * @brief Initialize the test framework
 * 
 * Must be called before any other framework functions.
 * 
 * @return true on success, false on failure
 */
bool test_framework_init(void);

/**
 * @brief Cleanup the test framework
 * 
 * Should be called when done with all tests.
 */
void test_framework_cleanup(void);

/* ============================================================================
 * CLUSTER MANAGEMENT
 * ========================================================================= */

/**
 * @brief Create a test cluster with specified number of nodes
 * 
 * @param num_nodes Number of nodes (1-TEST_MAX_NODES)
 * @return Cluster handle, or NULL on failure
 */
test_cluster_t* test_cluster_create(int num_nodes);

/**
 * @brief Destroy a test cluster and cleanup all resources
 * 
 * @param cluster Cluster to destroy
 */
void test_cluster_destroy(test_cluster_t* cluster);

/**
 * @brief Start all nodes in the cluster
 * 
 * Starts RAFT nodes and RPC servers, begins tick threads.
 * 
 * @param cluster Cluster to start
 * @return true on success
 */
bool test_cluster_start(test_cluster_t* cluster);

/**
 * @brief Stop all nodes in the cluster
 * 
 * Stops tick threads and RPC servers.
 * 
 * @param cluster Cluster to stop
 */
void test_cluster_stop(test_cluster_t* cluster);

/**
 * @brief Get RAFT node by ID
 * 
 * @param cluster Cluster
 * @param node_id Node ID (0-based)
 * @return RAFT node, or NULL if not found
 */
raft_node_t* test_cluster_get_node(test_cluster_t* cluster, int node_id);

/**
 * @brief Get test node by ID
 * 
 * @param cluster Cluster
 * @param node_id Node ID (0-based)
 * @return Test node, or NULL if not found
 */
test_node_t* test_cluster_get_test_node(test_cluster_t* cluster, int node_id);

/* ============================================================================
 * TIME CONTROL
 * ========================================================================= */

/**
 * @brief Get current time in milliseconds
 * 
 * @return Current time in milliseconds
 */
uint64_t test_get_time_ms(void);

/**
 * @brief Advance cluster time
 * 
 * Sleeps for the specified amount of time, allowing nodes to tick.
 * 
 * @param cluster Cluster
 * @param ms Milliseconds to advance
 */
void test_cluster_tick(test_cluster_t* cluster, int ms);

/* ============================================================================
 * LEADER ELECTION HELPERS
 * ========================================================================= */

/**
 * @brief Find current leader in cluster
 * 
 * @param cluster Cluster to check
 * @return Leader node ID, or -1 if no leader
 */
int test_cluster_find_leader(test_cluster_t* cluster);

/**
 * @brief Wait for leader to be elected
 * 
 * @param cluster Cluster to wait on
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return Leader node ID, or -1 if timeout
 */
int test_cluster_wait_for_leader(test_cluster_t* cluster, int timeout_ms);

/**
 * @brief Wait for stable leader
 * 
 * Waits for a leader to be elected and remain stable for a period.
 * 
 * @param cluster Cluster to wait on
 * @param stable_ms How long leader must be stable (milliseconds)
 * @return Leader node ID, or -1 if timeout
 */
int test_cluster_wait_for_stable_leader(test_cluster_t* cluster, int stable_ms);

/* ============================================================================
 * LOG REPLICATION HELPERS
 * ========================================================================= */

/**
 * @brief Replicate logs to followers
 * 
 * Waits for log replication to complete across the cluster.
 * 
 * @param cluster Cluster
 * @param num_entries Number of entries to replicate
 * @return true if replication succeeded
 */
bool test_cluster_replicate_logs(test_cluster_t* cluster, int num_entries);

/**
 * @brief Replicate and commit a single entry
 * 
 * Helper that replicates one entry and waits for commit.
 * 
 * @param cluster Cluster
 * @return true on success
 */
bool test_cluster_replicate_and_commit(test_cluster_t* cluster);

/**
 * @brief Verify log consistency across cluster
 * 
 * Checks that all nodes have consistent logs.
 * 
 * @param cluster Cluster to check
 * @return true if logs are consistent
 */
bool test_cluster_verify_log_consistency(test_cluster_t* cluster);

/* ============================================================================
 * NETWORK SIMULATION
 * ========================================================================= */

/**
 * @brief Create a network partition
 * 
 * Divides the cluster into two groups that cannot communicate.
 * 
 * @param cluster Cluster
 * @param group1 Array of node IDs in first group
 * @param group1_size Number of nodes in first group
 * @param group2 Array of node IDs in second group
 * @param group2_size Number of nodes in second group
 * @return true on success
 */
bool test_cluster_partition(test_cluster_t* cluster,
                            int* group1, int group1_size,
                            int* group2, int group2_size);

/**
 * @brief Heal a network partition
 * 
 * Restores communication between all nodes.
 * 
 * @param cluster Cluster
 */
void test_cluster_heal_partition(test_cluster_t* cluster);

/**
 * @brief Set message delay range for a node
 * 
 * @param cluster Cluster
 * @param node_id Node to configure
 * @param min_ms Minimum delay
 * @param max_ms Maximum delay
 */
void test_cluster_set_message_delay(test_cluster_t* cluster,
                                   int node_id,
                                   uint32_t min_ms,
                                   uint32_t max_ms);

/**
 * @brief Set message loss rate for a node
 * 
 * @param cluster Cluster
 * @param node_id Node to configure
 * @param rate Loss rate (0.0 = no loss, 1.0 = drop all)
 */
void test_cluster_set_message_loss_rate(test_cluster_t* cluster,
                                       int node_id,
                                       double rate);

/* ============================================================================
 * NODE CRASH/RESTART SIMULATION
 * ========================================================================= */

/**
 * @brief Simulate a node crash
 * 
 * Stops the node and marks it as crashed. It will not participate
 * in the cluster until restarted.
 * 
 * @param cluster Cluster
 * @param node_id Node to crash
 * @return true on success
 */
bool test_cluster_crash_node(test_cluster_t* cluster, int node_id);

/**
 * @brief Restart a crashed node
 * 
 * Restarts the node with its persistent state intact.
 * 
 * @param cluster Cluster
 * @param node_id Node to restart
 * @return true on success
 */
bool test_cluster_restart_node(test_cluster_t* cluster, int node_id);

/* ============================================================================
 * UTILITIES
 * ========================================================================= */

/**
 * @brief Print cluster state for debugging
 * 
 * @param cluster Cluster to print
 */
void test_cluster_print_state(test_cluster_t* cluster);

/**
 * @brief Verify a condition and print error if false
 * 
 * @param condition Condition to check
 * @param message Error message to print if false
 * @return true if condition is true
 */
bool test_assert(bool condition, const char* message);

#ifdef __cplusplus
}
#endif

#endif /* RAFT_TEST_FRAMEWORK_H */