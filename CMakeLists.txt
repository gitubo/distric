cmake_minimum_required(VERSION 3.15)
project(DistriC VERSION 2.0.0 LANGUAGES C)

# Set C11 standard globally with GNU extensions for variadic macro support
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # Enable GNU extensions (needed for ##__VA_ARGS__)

# Global compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Enable testing
enable_testing()

# Add observability library FIRST
add_subdirectory(libs/distric_obs)

# Add transport library SECOND (depends on distric_obs)
add_subdirectory(libs/distric_transport)

# Add protocol library THIRD (depends on distric_obs)
add_subdirectory(libs/distric_protocol)

# Add raft library
add_subdirectory(libs/distric_raft)

# Add gossip library
add_subdirectory(libs/distric_gossip)

# Add cluster library
add_subdirectory(libs/distric_cluster)
