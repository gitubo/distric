cmake_minimum_required(VERSION 3.15)
project(distric_gossip VERSION 1.0.0 LANGUAGES C)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wno-unused-parameter
    )
endif()

# ============================================================================
# DEPENDENCIES
# ============================================================================

# Find pthread
find_package(Threads REQUIRED)

# Find distric_common
find_package(distric_common REQUIRED)

# Find distric_obs (observability)
find_package(distric_obs REQUIRED)

# ============================================================================
# LIBRARY
# ============================================================================

# Source files
set(GOSSIP_SOURCES
    src/gossip_core.c
    src/gossip_membership.c
    src/gossip_lifecycle.c
)

# Create library
add_library(distric_gossip ${GOSSIP_SOURCES})

# Include directories
target_include_directories(distric_gossip
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
target_link_libraries(distric_gossip
    PUBLIC
        distric_common
        distric_obs
        Threads::Threads
    PRIVATE
        m  # Math library
)

# ============================================================================
# TESTS
# ============================================================================

if(BUILD_TESTING)
    enable_testing()
    
    # Unit tests
    add_executable(test_gossip tests/test_gossip.c)
    target_link_libraries(test_gossip
        PRIVATE
            distric_gossip
            distric_common
    )
    
    add_test(NAME GossipUnitTests COMMAND test_gossip)
    
    # Set test timeout
    set_tests_properties(GossipUnitTests PROPERTIES TIMEOUT 30)
endif()

# ============================================================================
# INSTALLATION
# ============================================================================

# Install library
install(TARGETS distric_gossip
    EXPORT distric_gossip-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

# Install headers
install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Install CMake config files
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/distric_gossip-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/distric_gossip-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/distric_gossip-config.cmake"
    INSTALL_DESTINATION lib/cmake/distric_gossip
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/distric_gossip-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/distric_gossip-config-version.cmake"
    DESTINATION lib/cmake/distric_gossip
)

install(EXPORT distric_gossip-targets
    FILE distric_gossip-targets.cmake
    NAMESPACE distric::
    DESTINATION lib/cmake/distric_gossip
)

# ============================================================================
# DOCUMENTATION
# ============================================================================

# Generate documentation target if Doxygen is available
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_GENERATE_MAN NO)
    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs)
    
    doxygen_add_docs(gossip_docs
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        COMMENT "Generating Gossip API documentation"
    )
endif()

# ============================================================================
# SUMMARY
# ============================================================================

message(STATUS "")
message(STATUS "DistriC Gossip Library Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Testing: ${BUILD_TESTING}")
message(STATUS "")