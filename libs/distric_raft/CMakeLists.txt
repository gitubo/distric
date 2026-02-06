cmake_minimum_required(VERSION 3.15)

# Library: distric_raft
project(distric_raft VERSION 1.0.0 LANGUAGES C)

# Set C11 standard with GNU extensions
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Find dependencies
find_package(Threads REQUIRED)

# Source files
set(RAFT_SOURCES
    src/raft_core.c
    src/raft_rpc.c
    src/raft_persistence.c
)

# Create library
add_library(distric_raft STATIC ${RAFT_SOURCES})

# Set C11 properties explicitly with GNU extensions
set_target_properties(distric_raft PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

# Include directories
target_include_directories(distric_raft
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
# Raft depends on observability and protocol layers
target_link_libraries(distric_raft
    PUBLIC
        distric_obs
        distric_protocol
        Threads::Threads
    PRIVATE
        m  # Math library for random
)

# Add tests
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS distric_raft
    EXPORT distric_raft-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)