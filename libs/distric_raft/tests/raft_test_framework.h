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
 * @return true on success, false on failure
 */
bool test_cluster_start(test_cluster_t* cluster);

/**
 * @brief Stop all nodes in the cluster
 * 
 * @param cluster Cluster to stop
 */
void test_cluster_stop(test_cluster_t* cluster);

/**
 * @brief Get a node by index
 * 
 * @param cluster Cluster
 * @param node_id Node index (0-based)
 * @return RAFT node handle, or NULL if invalid
 */
raft_node_t* test_cluster_get_node(test_cluster_t* cluster, int node_id);

/**
 * @brief Get test node context by index
 * 
 * @param cluster Cluster
 * @param node_id Node index (0-based)
 * @return Test node context, or NULL if invalid
 */
test_node_t* test_cluster_get_test_node(test_cluster_t* cluster, int node_id);

/* ============================================================================
 * TIME CONTROL
 * ========================================================================= */

/**
 * @brief Advance time by ticking all nodes
 * 
 * Simulates the passage of time by calling raft_tick() on all
 * non-crashed nodes.
 * 
 * @param cluster Cluster to tick
 * @param ms Milliseconds to advance (will sleep this amount)
 */
void test_cluster_tick(test_cluster_t* cluster, int ms);

/**
 * @brief Get current monotonic time in milliseconds
 * 
 * @return Current time in ms
 */
uint64_t test_get_time_ms(void);

/* ============================================================================
 * LEADER ELECTION HELPERS
 * ========================================================================= */

/**
 * @brief Wait for a leader to be elected
 * 
 * Polls the cluster until exactly one leader is detected or timeout.
 * 
 * @param cluster Cluster to check
 * @param timeout_ms Maximum time to wait
 * @return Node ID of leader, or -1 on timeout
 */
int test_cluster_wait_for_leader(test_cluster_t* cluster, uint32_t timeout_ms);

/**
 * @brief Force a specific node to start an election
 * 
 * This is a helper for testing - directly triggers election logic.
 * 
 * @param cluster Cluster
 * @param node_id Node to start election
 * @return true on success
 */
bool test_cluster_start_election(test_cluster_t* cluster, int node_id);

/**
 * @brief Count number of nodes in each state
 * 
 * @param cluster Cluster to check
 * @param leaders_out Number of leaders (can be NULL)
 * @param candidates_out Number of candidates (can be NULL)
 * @param followers_out Number of followers (can be NULL)
 */
void test_cluster_count_states(test_cluster_t* cluster,
                               int* leaders_out,
                               int* candidates_out,
                               int* followers_out);

/**
 * @brief Find the current leader
 * 
 * @param cluster Cluster to check
 * @return Node ID of leader, or -1 if none
 */
int test_cluster_find_leader(test_cluster_t* cluster);

/* ============================================================================
 * LOG REPLICATION HELPERS
 * ========================================================================= */

/**
 * @brief Append entries to the leader and wait for replication
 * 
 * Finds the leader, appends test entries, and waits for them to commit.
 * 
 * @param cluster Cluster
 * @param num_entries Number of entries to append
 * @return true if replicated successfully
 */
bool test_cluster_replicate_logs(test_cluster_t* cluster, int num_entries);

/**
 * @brief Append entries and wait for commit on majority
 * 
 * @param cluster Cluster
 * @return true on success
 */
bool test_cluster_replicate_and_commit(test_cluster_t* cluster);

/**
 * @brief Check if all nodes agree on committed entries
 * 
 * Verifies that all non-crashed, non-partitioned nodes have the
 * same committed log prefix.
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