cmake_minimum_required(VERSION 3.15)
project(distric_obs VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")

# Source files
set(SOURCES
    src/error.c
    src/metrics.c
    src/logging.c
)

# Header files
set(HEADERS
    include/distric_obs.h
)

set(INTERNAL_HEADERS
    include/distric_obs/error.h
    include/distric_obs/metrics.h
    include/distric_obs/logging.h
)

# Static library
add_library(distric_obs_static STATIC ${SOURCES})
target_include_directories(distric_obs_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(distric_obs_static PROPERTIES OUTPUT_NAME distric_obs)

# Shared library
add_library(distric_obs_shared SHARED ${SOURCES})
target_include_directories(distric_obs_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(distric_obs_shared PROPERTIES 
    OUTPUT_NAME distric_obs
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

# Link libraries
target_link_libraries(distric_obs_static PRIVATE m)
target_link_libraries(distric_obs_shared PRIVATE m)

# Install targets
install(TARGETS distric_obs_static distric_obs_shared
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES ${HEADERS} DESTINATION include)
install(DIRECTORY include/distric_obs DESTINATION include)

# Tests
enable_testing()
add_subdirectory(tests)