cmake_minimum_required(VERSION 3.15)

# Library: distric_protocol
project(distric_protocol VERSION 1.0.0 LANGUAGES C)

# Set C11 standard with GNU extensions
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Find dependencies
find_package(Threads REQUIRED)

# Source files
set(PROTOCOL_SOURCES
    src/binary.c
    src/crc32.c
    src/tlv.c
    src/messages.c
    src/rpc.c
)

# Create library
add_library(distric_protocol STATIC ${PROTOCOL_SOURCES})

# Set C11 properties explicitly with GNU extensions
set_target_properties(distric_protocol PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

# Include directories
target_include_directories(distric_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
# Protocol layer depends on observability (for future metrics/logging)
target_link_libraries(distric_protocol
    PUBLIC
        distric_obs
        distric_transport
        Threads::Threads
)

# Add tests
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation
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