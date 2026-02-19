cmake_minimum_required(VERSION 3.15)

# Library: distric_transport
project(distric_transport VERSION 1.0.0 LANGUAGES C)

# Set C11 standard with GNU extensions
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # Enable GNU extensions

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Find dependencies
find_package(Threads REQUIRED)

# Source files
set(TRANSPORT_SOURCES
    src/transport_error.c
    src/send_queue.c
    src/circuit_breaker.c
    src/tcp.c
    src/tcp_pool.c
    src/udp.c
)

# Create library
add_library(distric_transport STATIC ${TRANSPORT_SOURCES})

# Set C11 properties explicitly with GNU extensions
set_target_properties(distric_transport PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON  # Enable GNU extensions
)

# Include directories
target_include_directories(distric_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
# CRITICAL: Link to distric_obs library and inherit its public include directories
target_link_libraries(distric_transport
    PUBLIC
        distric_obs
        Threads::Threads
)

# Add tests
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS distric_transport
    EXPORT distric_transport-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)