cmake_minimum_required(VERSION 3.15)

# Helper to create a test executable linked against the static library
function(obs_test target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        # Tests do NOT get src/internal in their include path — boundary enforcement
    )
    target_link_libraries(${target} PRIVATE distric_obs_static pthread m)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

# ─── Core tests ───────────────────────────────────────────────────────────────
obs_test(test_metrics      test_metrics.c)
obs_test(test_logging      test_logging.c)
obs_test(test_tracing      test_tracing.c)
obs_test(test_health       test_health.c)
obs_test(test_http_server  test_http_server.c)
obs_test(test_integration  test_integration.c)

obs_test(test_distric_obs test_distric_obs.c)

# ─── Stress and production invariant tests ────────────────────────────────────
obs_test(test_stress             test_stress.c)
obs_test(test_stress_production  test_stress_production.c)
obs_test(test_production_hardening  test_production_hardening.c)

# ─── Failure mode and chaos tests (new — Improvement #5) ─────────────────────
obs_test(test_failure_modes  test_failure_modes.c)

# Set longer timeout for stress and chaos tests
set_tests_properties(test_stress            PROPERTIES TIMEOUT 120)
set_tests_properties(test_stress_production PROPERTIES TIMEOUT 120)
set_tests_properties(test_failure_modes     PROPERTIES TIMEOUT 90)
set_tests_properties(test_production_hardening PROPERTIES TIMEOUT 120)

# ─── Benchmarks (not added to ctest — run manually) ───────────────────────────
add_executable(bench_metrics bench_metrics.c)
target_include_directories(bench_metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(bench_metrics PRIVATE distric_obs_static pthread m)

add_executable(bench_logging bench_logging.c)
target_include_directories(bench_logging PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(bench_logging PRIVATE distric_obs_static pthread m)