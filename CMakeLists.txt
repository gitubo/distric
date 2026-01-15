cmake_minimum_required(VERSION 3.15)
project(DistriC VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Global compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Enable testing
enable_testing()

# Add subdirectories
add_subdirectory(libs/distric_obs)

# Future libraries can be added here
# add_subdirectory(libs/distric_core)
# add_subdirectory(libs/distric_net)