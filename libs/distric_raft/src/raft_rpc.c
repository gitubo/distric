/**
 * @file raft_rpc.c
 * @brief Raft RPC Integration Implementation
 * 
 * Implements network communication for Raft consensus with partition support.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_raft/raft_rpc.h"
#include <distric_protocol/messages.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

/**
 * @brief Raft RPC context
 */
struct raft_rpc_context {
    /* Configuration */
    raft_rpc_config_t config;
    
    /* Associated Raft node */
    raft_node_t* raft_node;
    
    /* RPC infrastructure */
    rpc_server_t* rpc_server;
    rpc_client_t* rpc_client;
    tcp_server_t* tcp_server;
    tcp_pool_t* tcp_pool;
    
    /* Metrics */
    metric_t* request_vote_sent_metric;
    metric_t* request_vote_received_metric;
    metric_t* append_entries_sent_metric;
    metric_t* append_entries_received_metric;
    metric_t* rpc_errors_metric;
    
    /* Send filter (for testing partitions) */
    raft_rpc_send_filter_t send_filter;
    void* send_filter_userdata;
    
    /* Control */
    volatile bool running;
};

/* ============================================================================
 * RPC MESSAGE HANDLERS
 * ========================================================================= */

/**
 * @brief Handle incoming RequestVote RPC
 */
static int handle_request_vote_rpc(
    const uint8_t* request,
    size_t req_len,
    uint8_t** response,
    size_t* resp_len,
    void* userdata,
    trace_span_t* span
) {
    raft_rpc_context_t* context = (raft_rpc_context_t*)userdata;
    
    /* Add trace tag */
    if (span) {
        trace_add_tag(span, "rpc_type", "RequestVote");
    }
    
    /* Deserialize request */
    raft_request_vote_t request_vote;
    distric_err_t err = deserialize_raft_request_vote(request, req_len, &request_vote);
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to deserialize RequestVote");
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }

    char req_term_str[16];
    snprintf(req_term_str, sizeof(req_term_str), "%u", request_vote.term);
    LOG_DEBUG(context->config.logger, "raft_rpc", "Received RequestVote",
            "from", request_vote.candidate_id,
            "term", req_term_str, NULL);
    
    /* Update metrics */
    metrics_counter_inc(context->request_vote_received_metric);
    
    /* Handle RequestVote */
    bool vote_granted = false;
    uint32_t term = 0;
    
    err = raft_handle_request_vote(
        context->raft_node,
        request_vote.candidate_id,
        request_vote.term,
        request_vote.last_log_index,
        request_vote.last_log_term,
        &vote_granted,
        &term
    );

    char resp_term_str[16];
    snprintf(resp_term_str, sizeof(resp_term_str), "%u", term);
    LOG_INFO(context->config.logger, "raft_rpc", 
            vote_granted ? "RequestVote → GRANT" : "RequestVote → REJECT",
            "from", request_vote.candidate_id,
            "term", resp_term_str, NULL);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "raft_handle_request_vote failed");
        free_raft_request_vote(&request_vote);
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }
    
    /* Create response */
    raft_request_vote_response_t response_msg = {
        .term = term,
        .vote_granted = vote_granted
    };
    
    err = serialize_raft_request_vote_response(&response_msg, response, resp_len);
    
    free_raft_request_vote(&request_vote);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to serialize RequestVote response");
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }
    
    LOG_DEBUG(context->config.logger, "raft_rpc", "RequestVote handled",
             "candidate", request_vote.candidate_id,
             "vote_granted", vote_granted ? "true" : "false");
    
    return 0;
}

/**
 * @brief Handle incoming AppendEntries RPC
 */
static int handle_append_entries_rpc(
    const uint8_t* request,
    size_t req_len,
    uint8_t** response,
    size_t* resp_len,
    void* userdata,
    trace_span_t* span
) {
    raft_rpc_context_t* context = (raft_rpc_context_t*)userdata;
    
    /* Add trace tag */
    if (span) {
        trace_add_tag(span, "rpc_type", "AppendEntries");
    }
    
    /* Deserialize request (wire format) */
    raft_append_entries_t append_entries_wire;
    distric_err_t err = deserialize_raft_append_entries(request, req_len, &append_entries_wire);
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to deserialize AppendEntries");
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }
    
    /* Update metrics */
    metrics_counter_inc(context->append_entries_received_metric);
    
    /* Convert wire format to internal format */
    raft_log_entry_t* entries_internal = NULL;
    if (append_entries_wire.entry_count > 0) {
        entries_internal = (raft_log_entry_t*)calloc(
            append_entries_wire.entry_count, sizeof(raft_log_entry_t));
        
        if (!entries_internal) {
            free_raft_append_entries(&append_entries_wire);
            metrics_counter_inc(context->rpc_errors_metric);
            return -1;
        }
        
        for (size_t i = 0; i < append_entries_wire.entry_count; i++) {
            entries_internal[i].index = append_entries_wire.entries[i].index;
            entries_internal[i].term = append_entries_wire.entries[i].term;
            entries_internal[i].type = (raft_entry_type_t)append_entries_wire.entries[i].entry_type;
            entries_internal[i].data = append_entries_wire.entries[i].data;
            entries_internal[i].data_len = append_entries_wire.entries[i].data_len;
        }
    }
    
    /* Handle AppendEntries with internal format */
    bool success = false;
    uint32_t term = 0;
    uint32_t last_log_index = 0;
    
    err = raft_handle_append_entries(
        context->raft_node,
        append_entries_wire.leader_id,
        append_entries_wire.term,
        append_entries_wire.prev_log_index,
        append_entries_wire.prev_log_term,
        entries_internal,
        append_entries_wire.entry_count,
        append_entries_wire.leader_commit,
        &success,
        &term,
        &last_log_index
    );
    
    /* Cleanup */
    if (entries_internal) {
        free(entries_internal);
    }
    free_raft_append_entries(&append_entries_wire);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "raft_handle_append_entries failed");
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }
    
    /* Create response */
    raft_append_entries_response_t response_msg = {
        .term = term,
        .success = success,
        .match_index = last_log_index
    };
    
    err = serialize_raft_append_entries_response(&response_msg, response, resp_len);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to serialize AppendEntries response");
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }
    
    LOG_DEBUG(context->config.logger, "raft_rpc", "AppendEntries handled",
             "leader", append_entries_wire.leader_id,
             "success", success ? "true" : "false");
    
    return 0;
}

/**
 * @brief Handle incoming InstallSnapshot RPC
 */
static int handle_install_snapshot_rpc(
    const uint8_t* request,
    size_t req_len,
    uint8_t** response,
    size_t* resp_len,
    void* userdata,
    trace_span_t* span
) {
    raft_rpc_context_t* context = (raft_rpc_context_t*)userdata;
    
    /* Add trace tag */
    if (span) {
        trace_add_tag(span, "rpc_type", "InstallSnapshot");
    }
    
    /* Deserialize request */
    raft_install_snapshot_t install_snapshot;
    distric_err_t err = deserialize_raft_install_snapshot(request, req_len, &install_snapshot);
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to deserialize InstallSnapshot");
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }
    
    /* Handle InstallSnapshot */
    bool success = false;
    uint32_t term = 0;
    
    err = raft_install_snapshot(
        context->raft_node,
        install_snapshot.last_included_index,
        install_snapshot.last_included_term,
        install_snapshot.data,
        install_snapshot.data_len
    );
    
    if (err == DISTRIC_OK) {
        success = true;
    }
    
    term = raft_get_term(context->raft_node);
    
    free_raft_install_snapshot(&install_snapshot);
    
    /* Create response */
    raft_install_snapshot_response_t response_msg = {
        .term = term,
        .success = success
    };
    
    err = serialize_raft_install_snapshot_response(&response_msg, response, resp_len);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to serialize InstallSnapshot response");
        metrics_counter_inc(context->rpc_errors_metric);
        return -1;
    }
    
    LOG_DEBUG(context->config.logger, "raft_rpc", "InstallSnapshot handled",
             "success", success ? "true" : "false");
    
    return 0;
}

/* ============================================================================
 * RPC CONTEXT LIFECYCLE
 * ========================================================================= */

distric_err_t raft_rpc_create(
    const raft_rpc_config_t* config,
    raft_node_t* raft_node,
    raft_rpc_context_t** context_out
) {
    if (!config || !raft_node || !context_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    raft_rpc_context_t* context = (raft_rpc_context_t*)calloc(1, sizeof(raft_rpc_context_t));
    if (!context) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Copy configuration */
    memcpy(&context->config, config, sizeof(raft_rpc_config_t));
    context->raft_node = raft_node;
    
    /* Initialize send filter to NULL (disabled by default) */
    context->send_filter = NULL;
    context->send_filter_userdata = NULL;
    
    /* Create TCP server */
    distric_err_t err = tcp_server_create(
        config->bind_address,
        config->bind_port,
        config->metrics,
        config->logger,
        &context->tcp_server
    );
    
    if (err != DISTRIC_OK) {
        free(context);
        return err;
    }
    
    /* Create TCP connection pool */
    err = tcp_pool_create(100, config->metrics, config->logger, &context->tcp_pool);
    if (err != DISTRIC_OK) {
        tcp_server_destroy(context->tcp_server);
        free(context);
        return err;
    }
    
    /* Create RPC server */
    err = rpc_server_create(context->tcp_server, config->metrics, config->logger, NULL, &context->rpc_server);
    if (err != DISTRIC_OK) {
        tcp_pool_destroy(context->tcp_pool);
        tcp_server_destroy(context->tcp_server);
        free(context);
        return err;
    }
    
    /* Create RPC client */
    err = rpc_client_create(context->tcp_pool, config->metrics, config->logger, NULL, &context->rpc_client);
    if (err != DISTRIC_OK) {
        rpc_server_destroy(context->rpc_server);
        tcp_pool_destroy(context->tcp_pool);
        tcp_server_destroy(context->tcp_server);
        free(context);
        return err;
    }
    
    /* Register RPC handlers */
    err = rpc_server_register_handler(
        context->rpc_server,
        MSG_RAFT_REQUEST_VOTE,
        handle_request_vote_rpc,
        context
    );
    
    if (err == DISTRIC_OK) {
        err = rpc_server_register_handler(
            context->rpc_server,
            MSG_RAFT_APPEND_ENTRIES,
            handle_append_entries_rpc,
            context
        );
    }
    
    if (err == DISTRIC_OK) {
        err = rpc_server_register_handler(
            context->rpc_server,
            MSG_RAFT_INSTALL_SNAPSHOT,
            handle_install_snapshot_rpc,
            context
        );
    }
    
    if (err != DISTRIC_OK) {
        rpc_client_destroy(context->rpc_client);
        rpc_server_destroy(context->rpc_server);
        tcp_pool_destroy(context->tcp_pool);
        tcp_server_destroy(context->tcp_server);
        free(context);
        return err;
    }
    
    /* Register metrics */
    if (config->metrics) {
        metrics_register_counter(config->metrics, "raft_request_vote_sent_total",
                                 "Total RequestVote RPCs sent", NULL, 0,
                                 &context->request_vote_sent_metric);
        metrics_register_counter(config->metrics, "raft_request_vote_received_total",
                                 "Total RequestVote RPCs received", NULL, 0,
                                 &context->request_vote_received_metric);
        metrics_register_counter(config->metrics, "raft_append_entries_sent_total",
                                 "Total AppendEntries RPCs sent", NULL, 0,
                                 &context->append_entries_sent_metric);
        metrics_register_counter(config->metrics, "raft_append_entries_received_total",
                                 "Total AppendEntries RPCs received", NULL, 0,
                                 &context->append_entries_received_metric);
        metrics_register_counter(config->metrics, "raft_rpc_errors_total",
                                 "Total RPC errors", NULL, 0,
                                 &context->rpc_errors_metric);
    }
    
    context->running = false;
    
    *context_out = context;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", config->bind_port);
    
    LOG_INFO(config->logger, "raft_rpc", "RPC context created",
            "bind_address", config->bind_address,
            "bind_port", port_str);
    
    return DISTRIC_OK;
}

distric_err_t raft_rpc_start(raft_rpc_context_t* context) {
    if (!context) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Start RPC server - this will start the TCP server internally */
    distric_err_t err = rpc_server_start(context->rpc_server);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    context->running = true;
    
    LOG_INFO(context->config.logger, "raft_rpc", "RPC server started");
    
    return DISTRIC_OK;
}

distric_err_t raft_rpc_stop(raft_rpc_context_t* context) {
    if (!context) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    context->running = false;
    
    /* Stop RPC server */
    rpc_server_stop(context->rpc_server);
    
    LOG_INFO(context->config.logger, "raft_rpc", "RPC server stopped");
    
    return DISTRIC_OK;
}

void raft_rpc_destroy(raft_rpc_context_t* context) {
    if (!context) return;
    
    if (context->running) {
        raft_rpc_stop(context);
    }
    
    rpc_client_destroy(context->rpc_client);
    rpc_server_destroy(context->rpc_server);
    tcp_pool_destroy(context->tcp_pool);
    tcp_server_destroy(context->tcp_server);
    
    free(context);
}

/* ============================================================================
 * SEND FILTER (FOR TESTING)
 * ========================================================================= */

void raft_rpc_set_send_filter(
    raft_rpc_context_t* context,
    raft_rpc_send_filter_t filter,
    void* userdata
) {
    if (!context) {
        return;
    }
    
    context->send_filter = filter;
    context->send_filter_userdata = userdata;
}

/* ============================================================================
 * RPC CLIENT OPERATIONS
 * ========================================================================= */

distric_err_t raft_rpc_send_request_vote(
    raft_rpc_context_t* context,
    const char* peer_address,
    uint16_t peer_port,
    const char* candidate_id,
    uint32_t term,
    uint32_t last_log_index,
    uint32_t last_log_term,
    bool* vote_granted_out,
    uint32_t* term_out
) {
    if (!context || !peer_address || !candidate_id || !vote_granted_out || !term_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Check send filter */
    if (context->send_filter) {
        if (!context->send_filter(context->send_filter_userdata, peer_address, peer_port)) {
            LOG_DEBUG(context->config.logger, "raft_rpc", "RequestVote blocked by send filter",
                     "peer", peer_address);
            return DISTRIC_ERR_UNAVAILABLE;
        }
    }
    
    /* Create request */
    raft_request_vote_t request = {
        .term = term,
        .last_log_index = last_log_index,
        .last_log_term = last_log_term
    };
    strncpy(request.candidate_id, candidate_id, sizeof(request.candidate_id) - 1);
    
    /* Serialize request */
    uint8_t* request_buf = NULL;
    size_t request_len = 0;
    distric_err_t err = serialize_raft_request_vote(&request, &request_buf, &request_len);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Send RPC with retry */
    uint8_t* response_buf = NULL;
    size_t response_len = 0;
    uint32_t retries = 0;
    
    while (retries <= context->config.max_retries) {
        char term_str[16];
        snprintf(term_str, sizeof(term_str), "%u", term);
        LOG_DEBUG(context->config.logger, "raft_rpc", "Sending RequestVote",
                "to", peer_address,
                "term", term_str,
                "last_log_index", &(int){last_log_index},
                "last_log_term", &(int){last_log_term}, NULL);
                
        err = rpc_call(
            context->rpc_client,
            peer_address,
            peer_port,
            MSG_RAFT_REQUEST_VOTE,
            request_buf,
            request_len,
            &response_buf,
            &response_len,
            context->config.rpc_timeout_ms
        );
        
        if (err == DISTRIC_OK) {
            break;
        }
        
        retries++;
        if (retries <= context->config.max_retries) {
            char retry_str[16];
            snprintf(retry_str, sizeof(retry_str), "%u", retries);
            LOG_WARN(context->config.logger, "raft_rpc", "RequestVote RPC failed, retrying",
                    "peer", peer_address,
                    "retry", retry_str);
            usleep(100000);  /* 100ms backoff */
        }
    }
    
    free(request_buf);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "RequestVote RPC failed after retries",
                 "peer", peer_address);
        metrics_counter_inc(context->rpc_errors_metric);
        return err;
    }
    
    /* Deserialize response */
    raft_request_vote_response_t response;
    err = deserialize_raft_request_vote_response(response_buf, response_len, &response);
    free(response_buf);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to deserialize RequestVote response");
        metrics_counter_inc(context->rpc_errors_metric);
        return err;
    }
    
    *vote_granted_out = response.vote_granted;
    *term_out = response.term;
    
    /* Update metrics */
    metrics_counter_inc(context->request_vote_sent_metric);
    
    LOG_DEBUG(context->config.logger, "raft_rpc", "RequestVote sent",
             "peer", peer_address,
             "vote_granted", response.vote_granted ? "true" : "false");
    
    return DISTRIC_OK;
}

distric_err_t raft_rpc_send_append_entries(
    raft_rpc_context_t* context,
    const char* peer_address,
    uint16_t peer_port,
    const char* leader_id,
    uint32_t term,
    uint32_t prev_log_index,
    uint32_t prev_log_term,
    const raft_log_entry_t* entries,
    size_t entry_count,
    uint32_t leader_commit,
    bool* success_out,
    uint32_t* term_out,
    uint32_t* match_index_out
) {
    if (!context || !peer_address || !leader_id || !success_out || !term_out || !match_index_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Check send filter */
    if (context->send_filter) {
        if (!context->send_filter(context->send_filter_userdata, peer_address, peer_port)) {
            /* Silently block heartbeats/replication to avoid log spam */
            return DISTRIC_ERR_UNAVAILABLE;
        }
    }
    
    /* Convert internal format to wire format */
    raft_log_entry_wire_t* entries_wire = NULL;
    if (entry_count > 0 && entries) {
        entries_wire = (raft_log_entry_wire_t*)calloc(entry_count, sizeof(raft_log_entry_wire_t));
        if (!entries_wire) {
            return DISTRIC_ERR_NO_MEMORY;
        }
        
        for (size_t i = 0; i < entry_count; i++) {
            entries_wire[i].term = entries[i].term;
            entries_wire[i].index = entries[i].index;
            entries_wire[i].entry_type = (uint8_t)entries[i].type;
            entries_wire[i].data = entries[i].data;
            entries_wire[i].data_len = entries[i].data_len;
        }
    }
    
    /* Create request with wire format */
    raft_append_entries_t request = {
        .term = term,
        .prev_log_index = prev_log_index,
        .prev_log_term = prev_log_term,
        .leader_commit = leader_commit,
        .entries = entries_wire,
        .entry_count = entry_count
    };
    strncpy(request.leader_id, leader_id, sizeof(request.leader_id) - 1);
    
    /* Serialize request */
    uint8_t* request_buf = NULL;
    size_t request_len = 0;
    distric_err_t err = serialize_raft_append_entries(&request, &request_buf, &request_len);
    
    /* Cleanup wire format entries */
    if (entries_wire) {
        free(entries_wire);
    }
    
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Send RPC */
    uint8_t* response_buf = NULL;
    size_t response_len = 0;
    
    err = rpc_call(
        context->rpc_client,
        peer_address,
        peer_port,
        MSG_RAFT_APPEND_ENTRIES,
        request_buf,
        request_len,
        &response_buf,
        &response_len,
        context->config.rpc_timeout_ms
    );
    
    free(request_buf);
    
    if (err != DISTRIC_OK) {
        LOG_WARN(context->config.logger, "raft_rpc", "AppendEntries RPC failed",
                "peer", peer_address);
        metrics_counter_inc(context->rpc_errors_metric);
        return err;
    }
    
    /* Deserialize response */
    raft_append_entries_response_t response;
    err = deserialize_raft_append_entries_response(response_buf, response_len, &response);
    free(response_buf);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to deserialize AppendEntries response");
        metrics_counter_inc(context->rpc_errors_metric);
        return err;
    }
    
    *success_out = response.success;
    *term_out = response.term;
    *match_index_out = response.match_index;
    
    /* Update metrics */
    metrics_counter_inc(context->append_entries_sent_metric);
    
    char entries_str[16];
    snprintf(entries_str, sizeof(entries_str), "%zu", entry_count);
    LOG_DEBUG(context->config.logger, "raft_rpc", "AppendEntries sent",
            "peer", peer_address,
            "entries", entries_str,
            "success", response.success ? "true" : "false");
    
    return DISTRIC_OK;
}

distric_err_t raft_rpc_send_install_snapshot(
    raft_rpc_context_t* context,
    const char* peer_address,
    uint16_t peer_port,
    const char* leader_id,
    uint32_t term,
    uint32_t last_included_index,
    uint32_t last_included_term,
    const uint8_t* snapshot_data,
    size_t snapshot_len,
    bool* success_out,
    uint32_t* term_out
) {
    if (!context || !peer_address || !leader_id || !success_out || !term_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Check send filter */
    if (context->send_filter) {
        if (!context->send_filter(context->send_filter_userdata, peer_address, peer_port)) {
            LOG_DEBUG(context->config.logger, "raft_rpc", "InstallSnapshot blocked by send filter",
                     "peer", peer_address);
            return DISTRIC_ERR_UNAVAILABLE;
        }
    }
    
    /* Create request */
    raft_install_snapshot_t request = {
        .term = term,
        .last_included_index = last_included_index,
        .last_included_term = last_included_term,
        .data = (uint8_t*)snapshot_data,
        .data_len = snapshot_len
    };
    strncpy(request.leader_id, leader_id, sizeof(request.leader_id) - 1);
    
    /* Serialize request */
    uint8_t* request_buf = NULL;
    size_t request_len = 0;
    distric_err_t err = serialize_raft_install_snapshot(&request, &request_buf, &request_len);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Send RPC (longer timeout for snapshot) */
    uint8_t* response_buf = NULL;
    size_t response_len = 0;
    
    err = rpc_call(
        context->rpc_client,
        peer_address,
        peer_port,
        MSG_RAFT_INSTALL_SNAPSHOT,
        request_buf,
        request_len,
        &response_buf,
        &response_len,
        context->config.rpc_timeout_ms * 10  /* 10x timeout for snapshot */
    );
    
    free(request_buf);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "InstallSnapshot RPC failed",
                 "peer", peer_address);
        metrics_counter_inc(context->rpc_errors_metric);
        return err;
    }
    
    /* Deserialize response */
    raft_install_snapshot_response_t response;
    err = deserialize_raft_install_snapshot_response(response_buf, response_len, &response);
    free(response_buf);
    
    if (err != DISTRIC_OK) {
        LOG_ERROR(context->config.logger, "raft_rpc", "Failed to deserialize InstallSnapshot response");
        metrics_counter_inc(context->rpc_errors_metric);
        return err;
    }
    
    *success_out = response.success;
    *term_out = response.term;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * BROADCAST HELPERS
 * ========================================================================= */

/**
 * @brief Thread argument for parallel RPC
 */
typedef struct {
    raft_rpc_context_t* context;
    raft_peer_t* peer;
    uint32_t term;
    uint32_t last_log_index;
    uint32_t last_log_term;
    char candidate_id[64];
    bool vote_granted;
    uint32_t peer_term;
    bool completed;
} request_vote_thread_arg_t;

static void* request_vote_thread(void* arg) {
    request_vote_thread_arg_t* targ = (request_vote_thread_arg_t*)arg;
    
    distric_err_t err = raft_rpc_send_request_vote(
        targ->context,
        targ->peer->address,
        targ->peer->port,
        targ->candidate_id,
        targ->term,
        targ->last_log_index,
        targ->last_log_term,
        &targ->vote_granted,
        &targ->peer_term
    );
    
    targ->completed = (err == DISTRIC_OK);
    
    return NULL;
}

distric_err_t raft_rpc_broadcast_request_vote(
    raft_rpc_context_t* context,
    raft_node_t* raft_node,
    uint32_t* votes_received_out
) {
    if (!context || !raft_node || !votes_received_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const raft_config_t* config = raft_get_config(raft_node);
    if (!config || config->peer_count == 0) {
        *votes_received_out = 1;  /* Self vote */
        return DISTRIC_OK;
    }
    
    uint32_t term = raft_get_term(raft_node);
    uint32_t last_log_index = raft_get_last_log_index(raft_node);
    uint32_t last_log_term = raft_get_last_log_term(raft_node);
    const char* node_id = raft_get_node_id(raft_node);
    
    /* Create threads for parallel broadcast */
    pthread_t* threads = (pthread_t*)calloc(config->peer_count, sizeof(pthread_t));
    request_vote_thread_arg_t* args = (request_vote_thread_arg_t*)calloc(
        config->peer_count, sizeof(request_vote_thread_arg_t));
    
    if (!threads || !args) {
        free(threads);
        free(args);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Start threads */
    for (size_t i = 0; i < config->peer_count; i++) {
        args[i].context = context;
        args[i].peer = &config->peers[i];
        args[i].term = term;
        args[i].last_log_index = last_log_index;
        args[i].last_log_term = last_log_term;
        strncpy(args[i].candidate_id, node_id, sizeof(args[i].candidate_id) - 1);
        args[i].vote_granted = false;
        args[i].completed = false;
        
        if (pthread_create(&threads[i], NULL, request_vote_thread, &args[i]) != 0) {
            args[i].completed = false;
        }
    }
    
    /* Wait for all threads */
    for (size_t i = 0; i < config->peer_count; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Count votes */
    uint32_t votes = 1;  /* Self vote */
    for (size_t i = 0; i < config->peer_count; i++) {
        if (args[i].completed && args[i].vote_granted) {
            votes++;
        }
        
        /* Check for higher term */
        if (args[i].completed && args[i].peer_term > term) {
            LOG_INFO(context->config.logger, "raft_rpc", "Discovered higher term during election",
                    "our_term", &term,
                    "peer_term", &(int){args[i].peer_term});
            
            /* Transition to follower */
            raft_step_down(raft_node, args[i].peer_term);
        }
    }
    
    free(threads);
    free(args);
    
    *votes_received_out = votes;
    
    char votes_str[16], needed_str[32];
    snprintf(votes_str, sizeof(votes_str), "%u", votes);
    snprintf(needed_str, sizeof(needed_str), "%zu", (config->peer_count + 1) / 2 + 1);
    LOG_INFO(context->config.logger, "raft_rpc", "RequestVote broadcast complete",
            "votes", votes_str,
            "needed", needed_str);
    
    return DISTRIC_OK;
}

/**
 * @brief Thread argument for AppendEntries broadcast
 */
typedef struct {
    raft_rpc_context_t* context;
    raft_node_t* raft_node;
    raft_peer_t* peer;
    size_t peer_index;
    bool completed;
} append_entries_thread_arg_t;

static void* append_entries_thread(void* arg) {
    append_entries_thread_arg_t* targ = (append_entries_thread_arg_t*)arg;
    
    /* Get peer's next_index and match_index from Raft node */
    uint32_t next_index = raft_get_peer_next_index(targ->raft_node, targ->peer_index);
    uint32_t last_log_index = raft_get_last_log_index(targ->raft_node);
    
    /* Determine entries to send */
    raft_log_entry_t* entries = NULL;
    size_t entry_count = 0;
    
    if (next_index <= last_log_index) {
        /* Get entries from next_index onwards */
        raft_get_log_entries(targ->raft_node, next_index, last_log_index + 1, &entries, &entry_count);
    }
    
    /* Get prev_log info */
    uint32_t prev_log_index = (next_index > 1) ? next_index - 1 : 0;
    uint32_t prev_log_term = (prev_log_index > 0) ? 
        raft_get_log_term(targ->raft_node, prev_log_index) : 0;
    
    const char* leader_id = raft_get_node_id(targ->raft_node);
    uint32_t term = raft_get_term(targ->raft_node);
    uint32_t leader_commit = raft_get_commit_index(targ->raft_node);
    
    bool success;
    uint32_t peer_term;
    uint32_t match_index;
    
    distric_err_t err = raft_rpc_send_append_entries(
        targ->context,
        targ->peer->address,
        targ->peer->port,
        leader_id,
        term,
        prev_log_index,
        prev_log_term,
        entries,
        entry_count,
        leader_commit,
        &success,
        &peer_term,
        &match_index
    );
    
    if (entries) {
        free(entries);
    }
    
    if (err == DISTRIC_OK) {
        if (success) {
            /* Update peer's next_index and match_index */
            uint32_t new_next_index = match_index + 1;
            raft_update_peer_indices(targ->raft_node, targ->peer_index, new_next_index, match_index);
        } else if (peer_term > term) {
            /* Higher term discovered - step down */
            raft_step_down(targ->raft_node, peer_term);
        } else {
            /* Decrement next_index and retry later */
            raft_decrement_peer_next_index(targ->raft_node, targ->peer_index);
        }
        targ->completed = true;
    } else {
        targ->completed = false;
    }
    
    return NULL;
}

distric_err_t raft_rpc_broadcast_append_entries(
    raft_rpc_context_t* context,
    raft_node_t* raft_node
) {
    if (!context || !raft_node) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const raft_config_t* config = raft_get_config(raft_node);
    if (!config || config->peer_count == 0) {
        return DISTRIC_OK;
    }
    
    /* Create threads for parallel broadcast */
    pthread_t* threads = (pthread_t*)calloc(config->peer_count, sizeof(pthread_t));
    append_entries_thread_arg_t* args = (append_entries_thread_arg_t*)calloc(
        config->peer_count, sizeof(append_entries_thread_arg_t));
    
    if (!threads || !args) {
        free(threads);
        free(args);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Start threads */
    for (size_t i = 0; i < config->peer_count; i++) {
        args[i].context = context;
        args[i].raft_node = raft_node;
        args[i].peer = &config->peers[i];
        args[i].peer_index = i;
        args[i].completed = false;
        
        if (pthread_create(&threads[i], NULL, append_entries_thread, &args[i]) != 0) {
            args[i].completed = false;
        }
    }
    
    /* Wait for all threads */
    for (size_t i = 0; i < config->peer_count; i++) {
        pthread_join(threads[i], NULL);
    }
    
    free(threads);
    free(args);
    
    return DISTRIC_OK;
}