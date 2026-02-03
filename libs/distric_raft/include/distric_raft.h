/**
 * @file distric_raft.h
 * @brief DistriC Raft Consensus - Single Public Header
 * 
 * This is the ONLY header file that users of the distric_raft library
 * need to include. It provides the Raft consensus algorithm for distributed
 * coordination in DistriC.
 * 
 * Features:
 * - Leader election with randomized timeouts
 * - Log replication with majority quorum
 * - Log compaction via snapshotting
 * - Dynamic membership changes (future)
 * - Integrated observability
 * 
 * Usage:
 * @code
 * #include <distric_raft.h>
 * 
 * // Create Raft configuration
 * raft_config_t config = {
 *     .node_id = "node-1",
 *     .peers = peers,
 *     .peer_count = 4,
 *     .election_timeout_min_ms = 150,
 *     .election_timeout_max_ms = 300,
 *     .heartbeat_interval_ms = 50,
 *     .apply_fn = apply_callback,
 *     .metrics = metrics,
 *     .logger = logger
 * };
 * 
 * // Create and start Raft node
 * raft_node_t* node;
 * raft_create(&config, &node);
 * raft_start(node);
 * 
 * // Main loop
 * while (running) {
 *     raft_tick(node);  // Drive timers and replication
 *     usleep(10000);    // 10ms
 * }
 * 
 * // Append entry (if leader)
 * uint32_t index;
 * raft_append_entry(node, data, len, &index);
 * 
 * // Wait for commit
 * raft_wait_committed(node, index, 5000);
 * 
 * // Cleanup
 * raft_stop(node);
 * raft_destroy(node);
 * @endcode
 * 
 * @version 1.0.0
 * @author DistriC Development Team
 */

#ifndef DISTRIC_RAFT_H
#define DISTRIC_RAFT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * RAFT MODULES
 * ========================================================================= */

/**
 * @defgroup raft_core Raft Core
 * @brief Core Raft consensus algorithm
 * 
 * Provides leader election, log replication, and safety guarantees.
 * @{
 */
#include "distric_raft/raft_core.h"
/** @} */

/* Future modules will be added here:
 * - raft_persistence: Durable storage for log and state
 * - raft_snapshot: Log compaction
 * - raft_membership: Dynamic cluster configuration
 * - raft_rpc: RPC integration layer
 */

/* ============================================================================
 * VERSION INFORMATION
 * ========================================================================= */

#define DISTRIC_RAFT_VERSION_MAJOR 1
#define DISTRIC_RAFT_VERSION_MINOR 0
#define DISTRIC_RAFT_VERSION_PATCH 0

/**
 * @brief Get library version string
 * 
 * @return Version string (e.g., "1.0.0")
 */
static inline const char* distric_raft_version(void) {
    return "1.0.0";
}

/* ============================================================================
 * TYPICAL USAGE WORKFLOW
 * ========================================================================= */

/**
 * @page raft_workflow Raft Usage Workflow
 * 
 * @section raft_init Initialization
 * 
 * 1. Create raft_config_t with node ID, peers, and callbacks
 * 2. Call raft_create() to initialize node
 * 3. Call raft_start() to begin election timer
 * 
 * @section raft_operation Operation
 * 
 * Main thread loop:
 * 1. Call raft_tick() periodically (every 10-50ms)
 * 2. Handle RPC requests (RequestVote, AppendEntries)
 * 3. Apply committed entries to state machine
 * 
 * Leader operations:
 * 1. Call raft_append_entry() to propose new commands
 * 2. Wait for commit with raft_wait_committed()
 * 3. Send heartbeats automatically via raft_tick()
 * 
 * @section raft_shutdown Shutdown
 * 
 * 1. Call raft_stop() to stop timers
 * 2. Call raft_destroy() to free resources
 */

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_H */