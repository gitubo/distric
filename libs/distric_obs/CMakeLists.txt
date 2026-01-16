cmake_minimum_required(VERSION 3.15)

# Library: distric_obs
project(distric_obs VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Find dependencies
find_package(Threads REQUIRED)

# Source files
set(OBS_SOURCES
    src/error.c
    src/metrics.c
    src/logging.c
    src/tracing.c
    src/health.c
    src/http_server.c
)

# Create static library
add_library(distric_obs_static STATIC ${OBS_SOURCES})
set_target_properties(distric_obs_static PROPERTIES OUTPUT_NAME distric_obs)

# Create shared library (optional, but good practice)
add_library(distric_obs SHARED ${OBS_SOURCES})

# Include directories for both library variants
foreach(target distric_obs distric_obs_static)
    target_include_directories(${target}
        PUBLIC
            # When building the library, use source include directory
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            # When installing, use install include directory
            $<INSTALL_INTERFACE:include>
        PRIVATE
            # Internal headers (not exported to consumers)
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    
    # Link dependencies
    target_link_libraries(${target}
        PUBLIC
            Threads::Threads
        PRIVATE
            m  # Math library for floating point operations
    )
endforeach()

# Add tests if enabled
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS distric_obs distric_obs_static
    EXPORT distric_obs-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# Install only the public header
install(FILES include/distric_obs.h
    DESTINATION include
)

# Install internal headers (for library developers, optional)
install(DIRECTORY include/distric_obs/
    DESTINATION include/distric_obs
    FILES_MATCHING PATTERN "*.h"
)

# Export targets for use by other projects
install(EXPORT distric_obs-targets
    FILE distric_obs-targets.cmake
    NAMESPACE distric::
    DESTINATION lib/cmake/distric_obs
)