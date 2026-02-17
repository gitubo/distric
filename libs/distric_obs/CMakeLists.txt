cmake_minimum_required(VERSION 3.15)

project(distric_obs VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")

find_package(Threads REQUIRED)

set(OBS_SOURCES
    src/error.c
    src/metrics.c
    src/logging.c
    src/tracing.c
    src/health.c
    src/http_server.c
)

add_library(distric_obs_static STATIC ${OBS_SOURCES})
set_target_properties(distric_obs_static PROPERTIES
    OUTPUT_NAME distric_obs
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

add_library(distric_obs SHARED ${OBS_SOURCES})
set_target_properties(distric_obs PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

foreach(target distric_obs distric_obs_static)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(${target}
        PUBLIC Threads::Threads
        PRIVATE m
    )
endforeach()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS distric_obs distric_obs_static
    EXPORT distric_obs-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(FILES include/distric_obs.h DESTINATION include)

install(EXPORT distric_obs-targets
    FILE distric_obs-targets.cmake
    NAMESPACE distric::
    DESTINATION lib/cmake/distric_obs
)