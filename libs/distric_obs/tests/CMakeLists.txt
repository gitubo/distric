add_executable(test_metrics test_metrics.c)
target_link_libraries(test_metrics PRIVATE distric_obs_static pthread m)
add_test(NAME test_metrics COMMAND test_metrics)

add_executable(test_logging test_logging.c)
target_link_libraries(test_logging PRIVATE distric_obs_static pthread m)
add_test(NAME test_logging COMMAND test_logging)

add_executable(test_distric_obs test_distric_obs.c)
target_link_libraries(test_distric_obs PRIVATE distric_obs_static pthread m)
add_test(NAME test_distric_obs COMMAND test_distric_obs)

add_executable(bench_metrics bench_metrics.c)
target_link_libraries(bench_metrics PRIVATE distric_obs_static pthread m)

add_executable(bench_logging bench_logging.c)
target_link_libraries(bench_logging PRIVATE distric_obs_static pthread m)