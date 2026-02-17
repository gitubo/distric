#include "distric_obs.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

void test_component_registration() {
    printf("Test: Health component registration...\n");

    health_registry_t* registry;
    distric_err_t err = health_init(&registry);
    assert(err == DISTRIC_OK);

    health_component_t* database;
    err = health_register_component(registry, "database", &database);
    assert(err == DISTRIC_OK);
    assert(database != NULL);

    health_component_t* cache;
    err = health_register_component(registry, "cache", &cache);
    assert(err == DISTRIC_OK);

    health_destroy(registry);
    printf("  PASSED\n\n");
}

void test_status_updates() {
    printf("Test: Health status updates...\n");

    health_registry_t* registry;
    health_init(&registry);

    health_component_t* component;
    health_register_component(registry, "service", &component);

    distric_err_t err = health_update_status(component, HEALTH_DEGRADED,
                                             "High latency detected");
    assert(err == DISTRIC_OK);
    assert(health_get_overall_status(registry) == HEALTH_DEGRADED);

    err = health_update_status(component, HEALTH_DOWN, "Connection refused");
    assert(err == DISTRIC_OK);
    assert(health_get_overall_status(registry) == HEALTH_DOWN);

    err = health_update_status(component, HEALTH_UP, "Recovered");
    assert(err == DISTRIC_OK);
    assert(health_get_overall_status(registry) == HEALTH_UP);

    health_destroy(registry);
    printf("  PASSED\n\n");
}

void test_overall_health() {
    printf("Test: Overall system health...\n");

    health_registry_t* registry;
    health_init(&registry);

    health_component_t *db, *cache, *api;
    health_register_component(registry, "database", &db);
    health_register_component(registry, "cache",    &cache);
    health_register_component(registry, "api",      &api);

    assert(health_get_overall_status(registry) == HEALTH_UP);

    health_update_status(cache, HEALTH_DEGRADED, "Slow");
    assert(health_get_overall_status(registry) == HEALTH_DEGRADED);

    health_update_status(db, HEALTH_DOWN, "Unavailable");
    assert(health_get_overall_status(registry) == HEALTH_DOWN);

    health_destroy(registry);
    printf("  PASSED\n\n");
}

void test_json_export() {
    printf("Test: Health JSON export...\n");

    health_registry_t* registry;
    health_init(&registry);

    health_component_t *db, *api;
    health_register_component(registry, "database", &db);
    health_register_component(registry, "api",      &api);

    health_update_status(db,  HEALTH_UP,       "Connected");
    health_update_status(api, HEALTH_DEGRADED, "High load");

    char* output;
    size_t size;
    distric_err_t err = health_export_json(registry, &output, &size);
    assert(err == DISTRIC_OK);
    assert(output != NULL);
    assert(size > 0);

    printf("  JSON output:\n%s\n", output);
    assert(strstr(output, "\"status\"")     != NULL);
    assert(strstr(output, "\"components\"") != NULL);
    assert(strstr(output, "\"database\"")   != NULL);
    assert(strstr(output, "\"api\"")        != NULL);
    assert(strstr(output, "DEGRADED")       != NULL);

    free(output);
    health_destroy(registry);
    printf("  PASSED\n\n");
}

void test_duplicate_registration() {
    printf("Test: Duplicate component registration...\n");

    health_registry_t* registry;
    health_init(&registry);

    health_component_t *comp1, *comp2;
    health_register_component(registry, "service", &comp1);
    health_register_component(registry, "service", &comp2);
    assert(comp1 == comp2);

    health_destroy(registry);
    printf("  PASSED\n\n");
}

int main() {
    printf("=== DistriC Health Monitoring Tests ===\n\n");
    test_component_registration();
    test_status_updates();
    test_overall_health();
    test_json_export();
    test_duplicate_registration();
    printf("=== All health tests passed ===\n");
    return 0;
}