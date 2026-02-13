cmake_minimum_required(VERSION 3.15)
project(distric_cluster C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Include directories
include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/libs/distric_cluster/include
    ${PROJECT_SOURCE_DIR}/libs/distric_obs/include
    ${PROJECT_SOURCE_DIR}/libs/distric_raft/include
    ${PROJECT_SOURCE_DIR}/libs/distric_gossip/include
    ${PROJECT_SOURCE_DIR}/libs/distric_common/include
)

# ============================================================================
# CLUSTER LIBRARY
# ============================================================================

add_library(distric_cluster STATIC
    cluster_coordinator.c
    worker_pool.c
)

target_link_libraries(distric_cluster
    distric_raft
    distric_gossip
    distric_obs
    pthread
)

# ============================================================================
# TESTS
# ============================================================================

# Integration tests
add_executable(test_cluster_integration
    test_cluster_integration.c
)

target_link_libraries(test_cluster_integration
    distric_cluster
    distric_raft
    distric_gossip
    distric_obs
    pthread
)

# Enable testing
enable_testing()
add_test(NAME cluster_integration_tests COMMAND test_cluster_integration)

# ============================================================================
# INSTALLATION
# ============================================================================

# Install headers
install(FILES distric_cluster.h
        DESTINATION include/)

install(FILES worker_pool.h
        DESTINATION include/distric_cluster/)

# Install library
install(TARGETS distric_cluster
        ARCHIVE DESTINATION lib)

# ============================================================================
# DOCUMENTATION
# ============================================================================

find_package(Doxygen)
if(DOXYGEN_FOUND)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in 
                   ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(doc_cluster
        ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation for distric_cluster"
    )
endif()

# ============================================================================
# EXAMPLES
# ============================================================================

# Example: Simple coordinator node
add_executable(example_coordinator
    examples/coordinator_node.c
)

target_link_libraries(example_coordinator
    distric_cluster
    distric_raft
    distric_gossip
    distric_obs
    pthread
)

# Example: Simple worker node
add_executable(example_worker
    examples/worker_node.c
)

target_link_libraries(example_worker
    distric_cluster
    distric_gossip
    distric_obs
    pthread
)

# Install examples
install(TARGETS example_coordinator example_worker
        RUNTIME DESTINATION bin/examples)