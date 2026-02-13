cmake_minimum_required(VERSION 3.15)

# Library: distric_cluster
project(distric_cluster VERSION 1.0.0 LANGUAGES C)

# Set C11 standard with GNU extensions
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Find dependencies
find_package(Threads REQUIRED)

# Source files
set(CLUSTER_SOURCES
    src/cluster_coordinator.c
    src/worker_pool.c
)

# Create library
add_library(distric_cluster STATIC ${CLUSTER_SOURCES})

# Set C11 properties explicitly with GNU extensions
set_target_properties(distric_cluster PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

# Include directories
target_include_directories(distric_cluster
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include/distric_cluster
)

# Link dependencies
# Cluster depends on raft, gossip, and observability layers
target_link_libraries(distric_cluster
    PUBLIC
        distric_raft
        distric_gossip
        distric_obs
        Threads::Threads
)

# Add tests
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS distric_cluster
    EXPORT distric_cluster-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)