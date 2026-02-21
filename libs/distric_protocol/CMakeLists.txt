cmake_minimum_required(VERSION 3.15)

project(distric_protocol VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

find_package(Threads REQUIRED)

set(PROTOCOL_SOURCES
    src/binary.c
    src/crc32.c
    src/tlv.c
    src/messages.c
    src/rpc.c
    src/auth.c
    src/capability.c
    src/error_info.c
)

add_library(distric_protocol STATIC ${PROTOCOL_SOURCES})

set_target_properties(distric_protocol PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

target_include_directories(distric_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(distric_protocol
    PUBLIC
        distric_obs
        distric_transport
        Threads::Threads
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---- Optional: libFuzzer targets (requires clang + -DBUILD_FUZZ=ON) ----
option(BUILD_FUZZ "Build libFuzzer harnesses" OFF)

if(BUILD_FUZZ)
    foreach(TARGET fuzz_tlv fuzz_header fuzz_auth)
        add_executable(${TARGET} tests/${TARGET}.c)
        target_compile_options(${TARGET} PRIVATE
            -fsanitize=fuzzer,address,undefined -O1)
        target_link_options(${TARGET} PRIVATE
            -fsanitize=fuzzer,address,undefined)
        target_link_libraries(${TARGET} PRIVATE distric_protocol)
        target_include_directories(${TARGET} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    endforeach()
endif()

install(TARGETS distric_protocol
    EXPORT distric_protocol-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)