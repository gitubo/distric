/**
 * @file gossip_core.c
 * @brief SWIM Protocol Core Implementation
 * 
 * Implements the core SWIM (Scalable Weakly-consistent Infection-style
 * Process Group Membership) protocol with:
 * - Direct probing
 * - Indirect probing through K random nodes
 * - Suspicion mechanism
 * - Infection-style dissemination
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#include "distric_gossip.h"
#include "distric_gossip/gossip_internal.h"
#include "distric_gossip/messages.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

/* ============================================================================
 * HELPER FUNCTIONS
 * ========================================================================= */

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static uint32_t get_random_seed(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec ^ tv.tv_usec ^ getpid());
}

/* ============================================================================
 * MESSAGE SERIALIZATION (Simple Binary Format)
 * ========================================================================= */

#define GOSSIP_MAGIC 0x47535350  /* "GSSP" */
#define GOSSIP_VERSION 1

static size_t serialize_message(const gossip_message_t* msg, uint8_t* buffer, size_t buffer_size) {
    if (buffer_size < 512) {
        return 0;  /* Buffer too small */
    }
    
    uint8_t* ptr = buffer;
    
    /* Header */
    *(uint32_t*)ptr = GOSSIP_MAGIC;
    ptr += 4;
    *(uint32_t*)ptr = GOSSIP_VERSION;
    ptr += 4;
    *(uint32_t*)ptr = (uint32_t)msg->type;
    ptr += 4;
    *(uint32_t*)ptr = msg->sequence;
    ptr += 4;
    *(uint64_t*)ptr = msg->incarnation;
    ptr += 8;
    
    /* Sender ID */
    strncpy((char*)ptr, msg->sender_id, 64);
    ptr += 64;
    
    /* Update count */
    *(uint32_t*)ptr = msg->update_count;
    ptr += 4;
    
    /* Updates (if any) */
    for (uint32_t i = 0; i < msg->update_count && i < 10; i++) {  /* Max 10 updates per message */
        const gossip_node_info_t* node = &msg->updates[i];
        
        strncpy((char*)ptr, node->node_id, 64);
        ptr += 64;
        strncpy((char*)ptr, node->address, 256);
        ptr += 256;
        *(uint16_t*)ptr = node->port;
        ptr += 2;
        *(uint32_t*)ptr = (uint32_t)node->status;
        ptr += 4;
        *(uint32_t*)ptr = (uint32_t)node->role;
        ptr += 4;
        *(uint64_t*)ptr = node->incarnation;
        ptr += 8;
        *(uint64_t*)ptr = node->load;
        ptr += 8;
    }
    
    return (size_t)(ptr - buffer);
}

static int deserialize_message(const uint8_t* buffer, size_t buffer_size, gossip_message_t* msg) {
    if (buffer_size < 96) {
        return -1;  /* Too small for header */
    }
    
    const uint8_t* ptr = buffer;
    
    /* Verify magic and version */
    if (*(uint32_t*)ptr != GOSSIP_MAGIC) {
        return -1;
    }
    ptr += 4;
    
    uint32_t version = *(uint32_t*)ptr;
    ptr += 4;
    if (version != GOSSIP_VERSION) {
        return -1;
    }
    
    /* Parse header */
    msg->type = (gossip_msg_type_t)*(uint32_t*)ptr;
    ptr += 4;
    msg->sequence = *(uint32_t*)ptr;
    ptr += 4;
    msg->incarnation = *(uint64_t*)ptr;
    ptr += 8;
    
    /* Sender ID */
    strncpy(msg->sender_id, (const char*)ptr, sizeof(msg->sender_id) - 1);
    msg->sender_id[sizeof(msg->sender_id) - 1] = '\0';
    ptr += 64;
    
    /* Update count */
    msg->update_count = *(uint32_t*)ptr;
    ptr += 4;
    
    /* Allocate and parse updates */
    if (msg->update_count > 0) {
        msg->update_count = (msg->update_count > 10) ? 10 : msg->update_count;
        msg->updates = (gossip_node_info_t*)calloc(msg->update_count, sizeof(gossip_node_info_t));
        if (!msg->updates) {
            return -1;
        }
        
        for (uint32_t i = 0; i < msg->update_count; i++) {
            if ((size_t)(ptr - buffer) + 346 > buffer_size) {
                free(msg->updates);
                msg->updates = NULL;
                return -1;
            }
            
            gossip_node_info_t* node = &msg->updates[i];
            strncpy(node->node_id, (const char*)ptr, sizeof(node->node_id) - 1);
            node->node_id[sizeof(node->node_id) - 1] = '\0';
            ptr += 64;
            
            strncpy(node->address, (const char*)ptr, sizeof(node->address) - 1);
            node->address[sizeof(node->address) - 1] = '\0';
            ptr += 256;
            
            node->port = *(uint16_t*)ptr;
            ptr += 2;
            node->status = (gossip_node_status_t)*(uint32_t*)ptr;
            ptr += 4;
            node->role = (gossip_node_role_t)*(uint32_t*)ptr;
            ptr += 4;
            node->incarnation = *(uint64_t*)ptr;
            ptr += 8;
            node->load = *(uint64_t*)ptr;
            ptr += 8;
        }
    } else {
        msg->updates = NULL;
    }
    
    return 0;
}

static void free_message(gossip_message_t* msg) {
    if (msg && msg->updates) {
        free(msg->updates);
        msg->updates = NULL;
    }
}

/* ============================================================================
 * NETWORK I/O
 * ========================================================================= */

static int send_message(
    gossip_state_t* state,
    const gossip_message_t* msg,
    const char* dest_addr,
    uint16_t dest_port
) {
    uint8_t buffer[2048];
    size_t len = serialize_message(msg, buffer, sizeof(buffer));
    if (len == 0) {
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_addr, &addr.sin_addr) <= 0) {
        return -1;
    }
    
    ssize_t sent = sendto(state->udp_socket, buffer, len, 0,
                          (struct sockaddr*)&addr, sizeof(addr));
    
    return (sent == (ssize_t)len) ? 0 : -1;
}

static int receive_message(
    gossip_state_t* state,
    gossip_message_t* msg,
    char* src_addr,
    uint16_t* src_port,
    int timeout_ms
) {
    uint8_t buffer[2048];
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    /* Set socket timeout */
    if (timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(state->udp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    
    ssize_t received = recvfrom(state->udp_socket, buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&addr, &addr_len);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;  /* Timeout */
        }
        return -1;  /* Error */
    }
    
    /* Parse source address */
    inet_ntop(AF_INET, &addr.sin_addr, src_addr, INET_ADDRSTRLEN);
    *src_port = ntohs(addr.sin_port);
    
    /* Deserialize message */
    if (deserialize_message(buffer, received, msg) != 0) {
        return -1;
    }
    
    return 0;  /* Success */
}

/* ============================================================================
 * PROTOCOL LOGIC
 * ========================================================================= */

/**
 * @brief Select a random alive node to probe
 * @return Pointer to membership entry or NULL if no nodes available
 */
static membership_entry_t* select_probe_target(gossip_state_t* state) {
    pthread_mutex_lock(&state->membership_lock);
    
    /* Count alive non-local nodes */
    size_t alive_count = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE &&
            !state->membership[i].local_node) {
            alive_count++;
        }
    }
    
    if (alive_count == 0) {
        pthread_mutex_unlock(&state->membership_lock);
        return NULL;
    }
    
    /* Select random target */
    size_t target_idx = (size_t)rand() % alive_count;
    membership_entry_t* target = NULL;
    
    size_t current = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE &&
            !state->membership[i].local_node) {
            if (current == target_idx) {
                target = &state->membership[i];
                break;
            }
            current++;
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
    
    return target;
}

/**
 * @brief Select K random nodes for indirect probing
 */
static void select_indirect_probers(
    gossip_state_t* state,
    const char* exclude_id,
    membership_entry_t*** probers_out,
    size_t* count_out
) {
    pthread_mutex_lock(&state->membership_lock);
    
    /* Collect candidates (alive, non-local, not the target) */
    size_t candidate_count = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE &&
            !state->membership[i].local_node &&
            strcmp(state->membership[i].info.node_id, exclude_id) != 0) {
            candidate_count++;
        }
    }
    
    if (candidate_count == 0) {
        pthread_mutex_unlock(&state->membership_lock);
        *probers_out = NULL;
        *count_out = 0;
        return;
    }
    
    /* Allocate candidate array */
    membership_entry_t** candidates = (membership_entry_t**)malloc(
        candidate_count * sizeof(membership_entry_t*)
    );
    
    size_t idx = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE &&
            !state->membership[i].local_node &&
            strcmp(state->membership[i].info.node_id, exclude_id) != 0) {
            candidates[idx++] = &state->membership[i];
        }
    }
    
    /* Select min(K, candidate_count) probers */
    size_t k = (state->config.indirect_probes < candidate_count) ?
               state->config.indirect_probes : candidate_count;
    
    membership_entry_t** probers = (membership_entry_t**)malloc(
        k * sizeof(membership_entry_t*)
    );
    
    /* Fisher-Yates shuffle for random selection */
    for (size_t i = 0; i < k; i++) {
        size_t j = i + ((size_t)rand() % (candidate_count - i));
        probers[i] = candidates[j];
        candidates[j] = candidates[i];
    }
    
    free(candidates);
    pthread_mutex_unlock(&state->membership_lock);
    
    *probers_out = probers;
    *count_out = k;
}

/**
 * @brief Perform direct probe of a node
 * @return 0 if ACK received, 1 if timeout, -1 on error
 */
static int probe_node_direct(
    gossip_state_t* state,
    const membership_entry_t* target
) {
    /* Send PING */
    gossip_message_t ping_msg;
    memset(&ping_msg, 0, sizeof(ping_msg));
    ping_msg.type = GOSSIP_MSG_PING;
    strncpy(ping_msg.sender_id, state->config.node_id, sizeof(ping_msg.sender_id) - 1);
    ping_msg.incarnation = state->self_incarnation;
    ping_msg.sequence = __atomic_fetch_add(&state->sequence_counter, 1, __ATOMIC_SEQ_CST);
    
    /* Add piggybacked updates */
    gossip_get_updates_to_broadcast(state, &ping_msg.updates, &ping_msg.update_count);
    
    int result = send_message(state, &ping_msg, target->info.address, target->info.port);
    free_message(&ping_msg);
    
    if (result != 0) {
        return -1;
    }
    
    /* Wait for ACK */
    uint64_t start_time = get_time_ms();
    while (get_time_ms() - start_time < state->config.probe_timeout_ms) {
        gossip_message_t response;
        char src_addr[INET_ADDRSTRLEN];
        uint16_t src_port;
        
        int recv_result = receive_message(state, &response, src_addr, &src_port,
                                          state->config.probe_timeout_ms - (get_time_ms() - start_time));
        
        if (recv_result == 0) {
            if (response.type == GOSSIP_MSG_ACK &&
                strcmp(response.sender_id, target->info.node_id) == 0) {
                free_message(&response);
                return 0;  /* ACK received */
            }
            free_message(&response);
        } else if (recv_result == 1) {
            break;  /* Timeout */
        }
    }
    
    return 1;  /* Timeout */
}

/**
 * @brief Perform indirect probe through K intermediaries
 * @return 0 if any ACK received, 1 if all timeout, -1 on error
 */
static int probe_node_indirect(
    gossip_state_t* state,
    const membership_entry_t* target
) {
    membership_entry_t** probers;
    size_t prober_count;
    
    select_indirect_probers(state, target->info.node_id, &probers, &prober_count);
    
    if (prober_count == 0) {
        return -1;  /* No intermediaries available */
    }
    
    /* Send PING-REQ to each prober */
    gossip_message_t ping_req;
    memset(&ping_req, 0, sizeof(ping_req));
    ping_req.type = GOSSIP_MSG_PING_REQ;
    strncpy(ping_req.sender_id, state->config.node_id, sizeof(ping_req.sender_id) - 1);
    ping_req.incarnation = state->self_incarnation;
    ping_req.sequence = __atomic_fetch_add(&state->sequence_counter, 1, __ATOMIC_SEQ_CST);
    
    /* Include target in updates */
    ping_req.update_count = 1;
    ping_req.updates = (gossip_node_info_t*)malloc(sizeof(gossip_node_info_t));
    memcpy(ping_req.updates, &target->info, sizeof(gossip_node_info_t));
    
    for (size_t i = 0; i < prober_count; i++) {
        send_message(state, &ping_req, probers[i]->info.address, probers[i]->info.port);
    }
    
    free_message(&ping_req);
    
    /* Wait for any ACK */
    uint64_t start_time = get_time_ms();
    int got_ack = 0;
    
    while (get_time_ms() - start_time < state->config.probe_timeout_ms) {
        gossip_message_t response;
        char src_addr[INET_ADDRSTRLEN];
        uint16_t src_port;
        
        int recv_result = receive_message(state, &response, src_addr, &src_port,
                                          state->config.probe_timeout_ms - (get_time_ms() - start_time));
        
        if (recv_result == 0) {
            if (response.type == GOSSIP_MSG_ACK &&
                strcmp(response.sender_id, target->info.node_id) == 0) {
                got_ack = 1;
                free_message(&response);
                break;
            }
            free_message(&response);
        } else if (recv_result == 1) {
            break;  /* Timeout */
        }
    }
    
    free(probers);
    
    return got_ack ? 0 : 1;
}

/**
 * @brief Main gossip protocol round
 */
void* gossip_protocol_thread(void* arg) {
    gossip_state_t* state = (gossip_state_t*)arg;
    
    srand(get_random_seed());
    
    while (__atomic_load_n(&state->running, __ATOMIC_SEQ_CST)) {
        uint64_t round_start = get_time_ms();
        
        /* Select a random node to probe */
        membership_entry_t* target = select_probe_target(state);
        
        if (target) {
            /* Direct probe */
            int direct_result = probe_node_direct(state, target);
            
            if (direct_result != 0) {
                /* Direct probe failed, try indirect */
                int indirect_result = probe_node_indirect(state, target);
                
                if (indirect_result != 0) {
                    /* Both failed - mark as suspected */
                    gossip_mark_node_suspected(state, target->info.node_id);
                }
            } else {
                /* Direct probe succeeded - ensure node is alive */
                gossip_mark_node_alive(state, target->info.node_id);
            }
        }
        
        /* Check for nodes that should transition from suspected to failed */
        gossip_check_suspicion_timeouts(state);
        
        /* Sleep for remainder of protocol period */
        uint64_t elapsed = get_time_ms() - round_start;
        if (elapsed < state->config.protocol_period_ms) {
            usleep((state->config.protocol_period_ms - elapsed) * 1000);
        }
    }
    
    return NULL;
}

/* ============================================================================
 * MESSAGE HANDLING
 * ========================================================================= */

/**
 * @brief Handle incoming PING message
 */
static void handle_ping(
    gossip_state_t* state,
    const gossip_message_t* msg,
    const char* src_addr,
    uint16_t src_port
) {
    /* Update membership with sender info */
    gossip_update_node_from_message(state, msg, src_addr, src_port);
    
    /* Send ACK */
    gossip_message_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.type = GOSSIP_MSG_ACK;
    strncpy(ack.sender_id, state->config.node_id, sizeof(ack.sender_id) - 1);
    ack.incarnation = state->self_incarnation;
    ack.sequence = msg->sequence;
    
    /* Add piggybacked updates */
    gossip_get_updates_to_broadcast(state, &ack.updates, &ack.update_count);
    
    send_message(state, &ack, src_addr, src_port);
    free_message(&ack);
}

/**
 * @brief Background message receiver thread
 */
void* gossip_receiver_thread(void* arg) {
    gossip_state_t* state = (gossip_state_t*)arg;
    
    while (__atomic_load_n(&state->running, __ATOMIC_SEQ_CST)) {
        gossip_message_t msg;
        char src_addr[INET_ADDRSTRLEN];
        uint16_t src_port;
        
        int result = receive_message(state, &msg, src_addr, &src_port, 100);  /* 100ms timeout */
        
        if (result == 0) {
            /* Process message based on type */
            switch (msg.type) {
                case GOSSIP_MSG_PING:
                    handle_ping(state, &msg, src_addr, src_port);
                    break;
                
                case GOSSIP_MSG_ACK:
                    /* ACK handled in protocol thread */
                    break;
                
                case GOSSIP_MSG_PING_REQ:
                    /* Handle indirect ping request */
                    /* TODO: Forward ping to target and relay ACK */
                    break;
                
                case GOSSIP_MSG_MEMBERSHIP:
                    gossip_update_node_from_message(state, &msg, src_addr, src_port);
                    break;
                
                case GOSSIP_MSG_LEAVE:
                    gossip_handle_node_leave(state, msg.sender_id);
                    break;
            }
            
            free_message(&msg);
        }
    }
    
    return NULL;
}