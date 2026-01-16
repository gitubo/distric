# Test executables for distric_transport

# Test: TCP Server and Connection
add_executable(test_tcp test_tcp.c)
target_link_libraries(test_tcp distric_transport distric_obs Threads::Threads)
add_test(NAME test_tcp COMMAND test_tcp)

# Test: TCP Connection Pool
add_executable(test_tcp_pool test_tcp_pool.c)
target_link_libraries(test_tcp_pool distric_transport distric_obs Threads::Threads)
add_test(NAME test_tcp_pool COMMAND test_tcp_pool)

# Test: UDP Socket
add_executable(test_udp test_udp.c)
target_link_libraries(test_udp distric_transport distric_obs Threads::Threads)
add_test(NAME test_udp COMMAND test_udp)

# Integration Test: Full Transport Layer
add_executable(test_transport_integration test_integration.c)
target_link_libraries(test_transport_integration distric_transport distric_obs Threads::Threads)
add_test(NAME test_transport_integration COMMAND test_transport_integration)