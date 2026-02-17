# Existing tests
add_executable(test_metrics test_metrics.c)
target_link_libraries(test_metrics PRIVATE distric_obs_static pthread m)
add_test(NAME test_metrics COMMAND test_metrics)

add_executable(test_logging test_logging.c)
target_link_libraries(test_logging PRIVATE distric_obs_static pthread m)
add_test(NAME test_logging COMMAND test_logging)

add_executable(test_distric_obs test_distric_obs.c)
target_link_libraries(test_distric_obs PRIVATE distric_obs_static pthread m)
add_test(NAME test_distric_obs COMMAND test_distric_obs)

# New tests (Phase 0 Sessions 0.3-0.5)
add_executable(test_tracing test_tracing.c)
target_link_libraries(test_tracing PRIVATE distric_obs_static pthread m)
add_test(NAME test_tracing COMMAND test_tracing)

add_executable(test_health test_health.c)
target_link_libraries(test_health PRIVATE distric_obs_static pthread m)
add_test(NAME test_health COMMAND test_health)

add_executable(test_http_server test_http_server.c)
target_link_libraries(test_http_server PRIVATE distric_obs_static pthread m)
add_test(NAME test_http_server COMMAND test_http_server)

add_executable(test_obs_integration test_integration.c)
target_link_libraries(test_obs_integration PRIVATE distric_obs_static pthread m)
add_test(NAME test_obs_integration COMMAND test_obs_integration)

# Benchmarks
add_executable(bench_metrics bench_metrics.c)
target_link_libraries(bench_metrics PRIVATE distric_obs_static pthread m)

add_executable(bench_logging bench_logging.c)
target_link_libraries(bench_logging PRIVATE distric_obs_static pthread m)

# Stress tests (Gap #5 — overload and non-blocking verification)
add_executable(test_stress test_stress.c)
target_link_libraries(test_stress PRIVATE distric_obs_static pthread m)
add_test(NAME test_stress COMMAND test_stress)