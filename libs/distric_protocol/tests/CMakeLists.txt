# Test executables for distric_protocol

# Test: Binary Protocol
add_executable(test_protocol_binary test_protocol_binary.c)
target_link_libraries(test_protocol_binary distric_protocol distric_obs Threads::Threads)
add_test(NAME test_protocol_binary COMMAND test_protocol_binary)

# Test: TLV Encoder/Decoder
add_executable(test_protocol_tlv test_protocol_tlv.c)
target_link_libraries(test_protocol_tlv distric_protocol distric_obs Threads::Threads)
add_test(NAME test_protocol_tlv COMMAND test_protocol_tlv)

# Test: Message Serialization
add_executable(test_protocol_messages test_protocol_messages.c)
target_link_libraries(test_protocol_messages distric_protocol distric_obs Threads::Threads)
add_test(NAME test_protocol_messages COMMAND test_protocol_messages)

# Test: RPC Framework
add_executable(test_protocol_rpc test_protocol_rpc.c)
target_link_libraries(test_protocol_rpc distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_protocol_rpc COMMAND test_protocol_rpc)

# Test: Integration
add_executable(test_protocol_integration test_protocol_integration.c)
target_link_libraries(test_protocol_integration distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_protocol_integration COMMAND test_protocol_integration)