cmake_minimum_required(VERSION 3.25)
project(redstone_solver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REDSTONE_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

if (MSVC)
    set(REDSTONE_WARNINGS /W4 /permissive- /utf-8)
    # The prebuilt OR-Tools only ships release binaries; a Debug runtime cannot link against it.
    set(CMAKE_CONFIGURATION_TYPES Release RelWithDebInfo CACHE STRING "" FORCE)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR "Debug builds are not supported with MSVC (OR-Tools is release-only). Use RelWithDebInfo.")
    endif ()
else ()
    set(REDSTONE_WARNINGS -Wall -Wextra -Wpedantic)
endif ()

# --- OR-Tools (CP-SAT)
if (MSVC)
    set(ORTOOLS_VERSION 9.15.6755)
    FetchContent_Declare(ortools_prebuilt
            URL https://github.com/google/or-tools/releases/download/v9.15/or-tools_x64_VisualStudio2022_cpp_v${ORTOOLS_VERSION}.zip
            URL_HASH SHA256=43429c741641c8b495ee77e44ea00f0f4524519495fd2edaf929003aa2b2ea30
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_SUBDIR no_cmake_project_here)
    FetchContent_MakeAvailable(ortools_prebuilt)
    list(PREPEND CMAKE_PREFIX_PATH ${ortools_prebuilt_SOURCE_DIR})
endif ()
# Elsewhere, install OR-Tools and point CMAKE_PREFIX_PATH at it.
find_package(ortools CONFIG REQUIRED)

# --- Core library
add_library(redstone_core STATIC
        src/redstone/block.cpp
        src/redstone/world.cpp
        src/redstone/update_order.cpp
        src/redstone/simulator.cpp)
target_include_directories(redstone_core PUBLIC src)
target_compile_options(redstone_core PRIVATE ${REDSTONE_WARNINGS})
target_link_libraries(redstone_core PUBLIC ortools::ortools)

# Copies the OR-Tools runtime DLLs next to an executable. TARGET_RUNTIME_DLLS misses the
# transitive ones (zlib, bz2, soplex, ...), so take the whole prebuilt bin folder.
function(redstone_copy_runtime_dlls target)
    if (MSVC)
        file(GLOB ORTOOLS_DLLS ${ortools_prebuilt_SOURCE_DIR}/bin/*.dll)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORTOOLS_DLLS} $<TARGET_FILE_DIR:${target}>)
    endif ()
endfunction()

# --- Application
add_executable(redstone_solver src/app/main.cpp)
target_link_libraries(redstone_solver PRIVATE redstone_core)
target_compile_options(redstone_solver PRIVATE ${REDSTONE_WARNINGS})
redstone_copy_runtime_dlls(redstone_solver)

# --- Tests
if (REDSTONE_BUILD_TESTS)
    FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.8.1
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)

    add_executable(redstone_tests
            tests/test_geometry.cpp
            tests/test_world.cpp
            tests/test_update_order.cpp
            tests/test_simulator.cpp
            tests/test_cp_sat.cpp)
    target_link_libraries(redstone_tests PRIVATE redstone_core Catch2::Catch2WithMain)
    target_compile_options(redstone_tests PRIVATE ${REDSTONE_WARNINGS})
    redstone_copy_runtime_dlls(redstone_tests)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(redstone_tests DISCOVERY_MODE PRE_TEST)
endif ()
