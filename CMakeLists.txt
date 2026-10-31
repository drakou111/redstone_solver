cmake_minimum_required(VERSION 3.25)
project(redstone_solver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REDSTONE_BUILD_TESTS "Build unit tests" ON)

if (MSVC)
    set(REDSTONE_WARNINGS /W4 /permissive-)
else ()
    set(REDSTONE_WARNINGS -Wall -Wextra -Wpedantic)
endif ()

add_library(redstone_core STATIC
        src/redstone/block.cpp
        src/redstone/world.cpp
        src/redstone/update_order.cpp
        src/redstone/simulator.cpp)
target_include_directories(redstone_core PUBLIC src)
target_compile_options(redstone_core PRIVATE ${REDSTONE_WARNINGS})

add_executable(redstone_solver src/app/main.cpp)
target_link_libraries(redstone_solver PRIVATE redstone_core)
target_compile_options(redstone_solver PRIVATE ${REDSTONE_WARNINGS})
if (MINGW)
    # Ship a single .exe without MinGW runtime DLLs.
    target_link_options(redstone_solver PRIVATE -static)
endif ()

if (REDSTONE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.8.1
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)

    add_executable(redstone_tests
            tests/test_geometry.cpp
            tests/test_world.cpp
            tests/test_update_order.cpp
            tests/test_simulator.cpp)
    target_link_libraries(redstone_tests PRIVATE redstone_core Catch2::Catch2WithMain)
    target_compile_options(redstone_tests PRIVATE ${REDSTONE_WARNINGS})
    if (MINGW)
        target_link_options(redstone_tests PRIVATE -static)
    endif ()

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(redstone_tests)
endif ()
