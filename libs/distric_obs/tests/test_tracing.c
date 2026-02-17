#define _DEFAULT_SOURCE

#include "distric_obs.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

static int exported_span_count = 0;

void test_export_callback(trace_span_t* spans, size_t count, void* user_data) {
    (void)user_data;
    printf("Exporting %zu spans:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  - %s (trace=%016lx%016lx, span=%016lx, parent=%016lx)\n",
               spans[i].operation,
               spans[i].trace_id.high, spans[i].trace_id.low,
               spans[i].span_id, spans[i].parent_span_id);
        printf("    Duration: %lu ns\n",
               spans[i].end_time_ns - spans[i].start_time_ns);
        printf("    Status: %d\n", spans[i].status);
        printf("    Tags: %zu\n", spans[i].tag_count);
        for (size_t j = 0; j < spans[i].tag_count; j++)
            printf("      %s = %s\n", spans[i].tags[j].key, spans[i].tags[j].value);
    }
    exported_span_count += count;
}

void test_span_creation() {
    printf("Test: Basic span creation...\n");

    tracer_t* tracer;
    distric_err_t err = trace_init(&tracer, test_export_callback, NULL);
    assert(err == DISTRIC_OK);

    trace_span_t* span;
    err = trace_start_span(tracer, "test_operation", &span);
    assert(err == DISTRIC_OK);
    assert(span != NULL);
    assert(strcmp(span->operation, "test_operation") == 0);
    assert(span->parent_span_id == 0);
    assert(span->start_time_ns > 0);

    usleep(10000);
    trace_finish_span(tracer, span);

    sleep(2);
    trace_destroy(tracer);
    printf("  PASSED\n\n");
}

void test_span_hierarchy() {
    printf("Test: Parent-child span relationships...\n");

    tracer_t* tracer;
    trace_init(&tracer, test_export_callback, NULL);

    trace_span_t* parent;
    trace_start_span(tracer, "parent_operation", &parent);

    trace_span_t* child1;
    trace_start_child_span(tracer, parent, "child_operation_1", &child1);
    assert(child1->trace_id.high == parent->trace_id.high);
    assert(child1->trace_id.low  == parent->trace_id.low);
    assert(child1->parent_span_id == parent->span_id);

    trace_span_t* child2;
    trace_start_child_span(tracer, parent, "child_operation_2", &child2);
    assert(child2->parent_span_id == parent->span_id);

    usleep(5000);
    trace_finish_span(tracer, child1);
    trace_finish_span(tracer, child2);
    trace_finish_span(tracer, parent);

    sleep(2);
    trace_destroy(tracer);
    printf("  PASSED\n\n");
}

void test_span_tags() {
    printf("Test: Span tags...\n");

    tracer_t* tracer;
    trace_init(&tracer, test_export_callback, NULL);

    trace_span_t* span;
    trace_start_span(tracer, "tagged_operation", &span);

    distric_err_t err = trace_add_tag(span, "http.method", "GET");
    assert(err == DISTRIC_OK);
    err = trace_add_tag(span, "http.url", "/api/users");
    assert(err == DISTRIC_OK);
    err = trace_add_tag(span, "http.status_code", "200");
    assert(err == DISTRIC_OK);

    assert(span->tag_count == 3);
    assert(strcmp(span->tags[0].key,   "http.method") == 0);
    assert(strcmp(span->tags[0].value, "GET") == 0);

    trace_finish_span(tracer, span);

    sleep(2);
    trace_destroy(tracer);
    printf("  PASSED\n\n");
}

void test_context_propagation() {
    printf("Test: Context propagation...\n");

    tracer_t* tracer;
    trace_init(&tracer, test_export_callback, NULL);

    trace_span_t* span;
    trace_start_span(tracer, "service_a", &span);

    char header[256];
    distric_err_t err = trace_inject_context(span, header, sizeof(header));
    assert(err == DISTRIC_OK);
    printf("  Injected header: %s\n", header);

    trace_context_t context;
    err = trace_extract_context(header, &context);
    assert(err == DISTRIC_OK);
    assert(context.trace_id.high == span->trace_id.high);
    assert(context.trace_id.low  == span->trace_id.low);
    assert(context.span_id       == span->span_id);

    trace_span_t* remote_span;
    err = trace_start_span_from_context(tracer, &context, "service_b", &remote_span);
    assert(err == DISTRIC_OK);
    assert(remote_span->trace_id.high  == span->trace_id.high);
    assert(remote_span->trace_id.low   == span->trace_id.low);
    assert(remote_span->parent_span_id == span->span_id);

    trace_finish_span(tracer, remote_span);
    trace_finish_span(tracer, span);

    sleep(2);
    trace_destroy(tracer);
    printf("  PASSED\n\n");
}

void test_active_span() {
    printf("Test: Thread-local active span...\n");

    tracer_t* tracer;
    trace_init(&tracer, test_export_callback, NULL);

    trace_span_t* span;
    trace_start_span(tracer, "active_operation", &span);

    trace_set_active_span(span);
    assert(trace_get_active_span() == span);

    trace_set_active_span(NULL);
    assert(trace_get_active_span() == NULL);

    trace_finish_span(tracer, span);

    sleep(2);
    trace_destroy(tracer);
    printf("  PASSED\n\n");
}

int main() {
    printf("=== DistriC Tracing Tests ===\n\n");
    exported_span_count = 0;

    test_span_creation();
    test_span_hierarchy();
    test_span_tags();
    test_context_propagation();
    test_active_span();

    printf("=== All tracing tests passed ===\n");
    printf("Total spans exported: %d\n", exported_span_count);
    return 0;
}