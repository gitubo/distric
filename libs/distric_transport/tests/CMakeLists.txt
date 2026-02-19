# Test executables for distric_transport

# Test: TCP Server and Connection
add_executable(test_transport_tcp test_transport_tcp.c)
target_link_libraries(test_transport_tcp distric_transport distric_obs Threads::Threads)
add_test(NAME test_tcp COMMAND test_tcp)

# Test: TCP Connection Pool
add_executable(test_transport_tcp_pool test_transport_tcp_pool.c)
target_link_libraries(test_transport_tcp_pool distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_tcp_pool COMMAND test_transport_tcp_pool)

# Test: UDP Socket
add_executable(test_transport_udp test_transport_udp.c)
target_link_libraries(test_transport_udp distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_udp COMMAND test_transport_udp)

# Integration Test: Full Transport Layer
add_executable(test_transport_transport_integration test_transport_integration.c)
target_link_libraries(test_transport_transport_integration distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_transport_integration COMMAND test_transport_transport_integration)

add_executable(test_transport_backpressure test_transport_backpressure.c)
target_link_libraries(test_transport_backpressure distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_backpressure COMMAND test_transport_backpressure)

add_executable(test_transport_error_taxonomy test_transport_error_taxonomy.c)
target_link_libraries(test_transport_error_taxonomy distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_error_taxonomy COMMAND test_transport_error_taxonomy)

add_executable(test_transport_shutdown test_transport_shutdown.c)
target_link_libraries(test_transport_shutdown distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_shutdown COMMAND test_transport_shutdown)

add_executable(test_transport_udp_ratelimits test_transport_udp_ratelimits.c)
target_link_libraries(test_transport_udp_ratelimits distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_udp_ratelimits COMMAND test_transport_udp_ratelimits)

# =============================================================================
# Chaos and Failure-Injection Tests (v3 — Item 5)
#
# These tests validate behaviour under degraded conditions:
#   - slow handlers (saturation protection)
#   - global memory budget enforcement
#   - backpressure under slow receivers
#   - connection storms (fd leak detection)
#   - observability sampling correctness
#   - environment variable configuration override
# =============================================================================
add_executable(test_transport_chaos test_transport_chaos.c)
target_link_libraries(test_transport_chaos distric_transport distric_obs Threads::Threads)
# Chaos tests are slower — give them a generous timeout.
add_test(NAME test_transport_chaos COMMAND test_transport_chaos)
set_tests_properties(test_transport_chaos PROPERTIES TIMEOUT 60)