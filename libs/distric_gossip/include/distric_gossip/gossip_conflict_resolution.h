/**
 * @file gossip_conflict_resolution.h
 * @brief Enhanced Conflict Resolution for Gossip Protocol (Section 4.2)
 * 
 * Provides explicit conflict resolution rules and enhanced refutation
 * mechanisms for the SWIM protocol implementation.
 * 
 * @version 1.1
 * @date 2026-02-12
 */

#ifndef DISTRIC_GOSSIP_CONFLICT_RESOLUTION_H
#define DISTRIC_GOSSIP_CONFLICT_RESOLUTION_H

#include "distric_gossip.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CONFLICT RESOLUTION FUNCTIONS
 * ========================================================================= */

/**
 * @brief Compare two node status updates for conflict resolution
 * 
 * Resolution rules:
 * 1. Higher incarnation always wins
 * 2. If incarnation equal, state priority: ALIVE > SUSPECTED > FAILED > LEFT
 * 
 * @param status1 First status
 * @param incarnation1 First incarnation
 * @param status2 Second status
 * @param incarnation2 Second incarnation
 * @return > 0 if update1 wins, < 0 if update2 wins, = 0 if equivalent
 * 
 * Thread Safety: Safe, no state modification
 */
int gossip_compare_updates(
    gossip_node_status_t status1,
    uint64_t incarnation1,
    gossip_node_status_t status2,
    uint64_t incarnation2
);

/**
 * @brief Determine if an incoming update should replace current state
 * 
 * @param current_status Current node status
 * @param current_incarnation Current incarnation
 * @param update_status Incoming update status
 * @param update_incarnation Incoming update incarnation
 * @return true if update should be applied, false otherwise
 * 
 * Thread Safety: Safe, no state modification
 */
bool gossip_should_apply_update(
    gossip_node_status_t current_status,
    uint64_t current_incarnation,
    gossip_node_status_t update_status,
    uint64_t update_incarnation
);

/**
 * @brief Broadcast refutation immediately to all known nodes
 * 
 * More aggressive than piggybacking - sends immediate refutation to all
 * alive nodes for faster propagation. Use when immediate correction is critical.
 * 
 * Behavior:
 * - Increments incarnation number
 * - Marks self as ALIVE
 * - Broadcasts membership update to all alive nodes
 * 
 * @param state Gossip state
 * @return DISTRIC_OK on success, DISTRIC_ERR_IO if all sends failed
 * 
 * Thread Safety: Safe to call from any thread
 * 
 * Example:
 * @code
 * if (i_am_being_falsely_suspected) {
 *     gossip_broadcast_refutation(state);
 * }
 * @endcode
 */
distric_err_t gossip_broadcast_refutation(gossip_state_t* state);

/**
 * @brief Apply conflict resolution rules when processing membership update
 * 
 * This function encapsulates all conflict resolution logic:
 * - Checks if update should be applied based on incarnation and state priority
 * - Handles automatic refutation for local node
 * - Accepts new nodes unconditionally
 * 
 * @param state Gossip state
 * @param node_id Node being updated
 * @param update_status New status
 * @param update_incarnation New incarnation
 * @return true if update should be applied, false if rejected
 * 
 * Thread Safety: Safe to call from protocol thread
 */
bool gossip_resolve_conflict(
    gossip_state_t* state,
    const char* node_id,
    gossip_node_status_t update_status,
    uint64_t update_incarnation
);

/**
 * @brief Get statistics about conflict resolution events
 * 
 * @param state Gossip state
 * @param refutations_out Number of refutations performed (output)
 * @param conflicts_resolved_out Number of conflicts resolved (output)
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_get_conflict_stats(
    gossip_state_t* state,
    uint64_t* refutations_out,
    uint64_t* conflicts_resolved_out
);

/* ============================================================================
 * CONFLICT RESOLUTION TESTING UTILITIES
 * ========================================================================= */

/**
 * @brief Test scenarios for conflict resolution
 * 
 * These scenarios can be used in unit tests to verify correct behavior:
 * 
 * Scenario 1: Higher incarnation wins
 * - current: (ALIVE, inc=5)
 * - update: (SUSPECTED, inc=6)
 * - result: SUSPECTED wins (higher incarnation)
 * 
 * Scenario 2: State priority when incarnation equal
 * - current: (SUSPECTED, inc=5)
 * - update: (ALIVE, inc=5)
 * - result: ALIVE wins (higher state priority)
 * 
 * Scenario 3: Automatic refutation
 * - local_node: (ALIVE, inc=5)
 * - update: (SUSPECTED, inc=6) targeting local node
 * - result: Auto-increment to inc=7, broadcast ALIVE
 * 
 * Scenario 4: Stale update rejected
 * - current: (ALIVE, inc=10)
 * - update: (FAILED, inc=9)
 * - result: Rejected (lower incarnation)
 */

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_GOSSIP_CONFLICT_RESOLUTION_H */