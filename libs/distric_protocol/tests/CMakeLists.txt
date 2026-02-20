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

# Test: Production hardening
add_executable(test_protocol_production test_protocol_production.c)
target_link_libraries(test_protocol_production distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_protocol_production COMMAND test_protocol_production)

# Test: Improvement regressions (Fix #1–#5)
# Covers: unaligned serialisation, pthread_once CRC race, required-field
# validation, pool-acquire timeout semaphore, and O(1) handler dispatch.
add_executable(test_protocol_improvements test_protocol_improvements.c)
target_link_libraries(test_protocol_improvements
    distric_protocol
    distric_obs
    Threads::Threads
)
# Enable UBSan + ASan to catch any remaining alignment/aliasing violations.
# These flags are added here rather than globally to keep CI fast for other
# targets.  Disable for release builds via -DENABLE_SANITIZERS=OFF.
if(NOT DEFINED ENABLE_SANITIZERS OR ENABLE_SANITIZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_protocol_improvements PRIVATE
            -fsanitize=undefined
            -fsanitize=address
            -fno-omit-frame-pointer
            -fstrict-aliasing
            -Wstrict-aliasing=3
        )
        target_link_options(test_protocol_improvements PRIVATE
            -fsanitize=undefined
            -fsanitize=address
        )
    endif()
endif()
add_test(NAME test_protocol_improvements COMMAND test_protocol_improvements)