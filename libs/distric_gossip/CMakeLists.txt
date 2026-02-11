cmake_minimum_required(VERSION 3.15)

# Library: distric_gossip
project(distric_gossip VERSION 1.0.0 LANGUAGES C)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Find pthread
find_package(Threads REQUIRED)

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

# Set C11 properties explicitly with GNU extensions
set_target_properties(distric_gossip PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

# Include directories
target_include_directories(distric_gossip
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link dependencies
target_link_libraries(distric_gossip
    PUBLIC
        distric_obs
        Threads::Threads
    PRIVATE
        m  # Math library
)

# Add tests
if(BUILD_TESTING)
    add_subdirectory(tests)
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
    INCLUDES DESTINATION include
)

# Install public header (root level)
install(FILES distric_gossip.h
    DESTINATION include
)

# Install internal headers (for library development only, optional)
install(DIRECTORY include/distric_gossip
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)