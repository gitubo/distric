cmake_minimum_required(VERSION 3.15)
project(DistriC VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Global compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Enable testing
enable_testing()

# CRITICAL: Build order matters!
# distric_obs must be built BEFORE distric_transport
# because transport depends on obs

# Add observability library FIRST
add_subdirectory(libs/distric_obs)

# Add transport library SECOND (depends on distric_obs)
add_subdirectory(libs/distric_transport)

# Future libraries can be added here
# add_subdirectory(libs/distric_core)
# add_subdirectory(libs/distric_net)