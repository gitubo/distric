# Test executables for distric_protocol

# Test: Binary Protocol
add_executable(test_binary test_binary.c)
target_link_libraries(test_binary distric_protocol distric_obs Threads::Threads)
add_test(NAME test_binary COMMAND test_binary)

# Test: TLV Encoder/Decoder
add_executable(test_tlv test_tlv.c)
target_link_libraries(test_tlv distric_protocol distric_obs Threads::Threads)
add_test(NAME test_tlv COMMAND test_tlv)

# Test: Message Serialization
add_executable(test_messages test_messages.c)
target_link_libraries(test_messages distric_protocol distric_obs Threads::Threads)
add_test(NAME test_messages COMMAND test_messages)

# Test: RPC Framework
add_executable(test_rpc test_rpc.c)
target_link_libraries(test_rpc distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_rpc COMMAND test_rpc)

# Test: Integration
add_executable(test_protocol_integration test_protocol_integration.c)
target_link_libraries(test_protocol_integration distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_protocol_integration COMMAND test_protocol_integration)