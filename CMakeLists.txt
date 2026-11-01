cmake_minimum_required(VERSION 3.25)
project(redstone_solver LANGUAGES C CXX)

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

# --- CaDiCaL (SAT solver). It builds with make, so compile its sources here.
FetchContent_Declare(cadical
        GIT_REPOSITORY https://github.com/arminbiere/cadical.git
        GIT_TAG rel-3.0.1
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR no_cmake_project_here)
FetchContent_MakeAvailable(cadical)
file(GLOB CADICAL_SOURCES ${cadical_SOURCE_DIR}/src/*.cpp ${cadical_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM CADICAL_SOURCES ${cadical_SOURCE_DIR}/src/cadical.cpp ${cadical_SOURCE_DIR}/src/mobical.cpp)
add_library(cadical STATIC ${CADICAL_SOURCES})
target_include_directories(cadical SYSTEM PUBLIC ${cadical_SOURCE_DIR}/src)
target_compile_definitions(cadical PRIVATE NBUILD)
if (MSVC)
    set(CADICAL_COMPAT ${CMAKE_SOURCE_DIR}/cmake/cadical_compat)
    target_include_directories(cadical PRIVATE ${CADICAL_COMPAT}/posix)
    target_compile_options(cadical PRIVATE /W0 /FI${CADICAL_COMPAT}/msvc_compat.h)
else ()
    target_compile_options(cadical PRIVATE -w)
endif ()

# --- Core library
add_library(redstone_core STATIC
        src/redstone/block.cpp
        src/redstone/world.cpp
        src/redstone/update_order.cpp
        src/redstone/simulator.cpp
        src/redstone/spec.cpp
        src/redstone/cp_builder.cpp
        src/redstone/encoder.cpp
        src/redstone/verifier.cpp
        src/redstone/solver.cpp
        src/redstone/report.cpp
        src/redstone/solve_job.cpp
        src/redstone/sat_engine.cpp)
target_include_directories(redstone_core PUBLIC src)
target_compile_options(redstone_core PRIVATE ${REDSTONE_WARNINGS})
target_link_libraries(redstone_core PUBLIC ortools::ortools PRIVATE cadical)
# CaDiCaL recurses deeply; Windows threads default to a 1 MB stack, and the
# executable's stack reserve is what new threads get.
if (MSVC)
    target_link_options(redstone_core INTERFACE /STACK:67108864)
endif ()
if (MSVC)
    target_include_directories(redstone_core PRIVATE ${CADICAL_COMPAT})
endif ()

# Copies the OR-Tools runtime DLLs next to an executable. TARGET_RUNTIME_DLLS misses the
# transitive ones (zlib, bz2, soplex, ...), so take the whole prebuilt bin folder.
function(redstone_copy_runtime_dlls target)
    if (MSVC)
        file(GLOB ORTOOLS_DLLS ${ortools_prebuilt_SOURCE_DIR}/bin/*.dll)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORTOOLS_DLLS} $<TARGET_FILE_DIR:${target}>)
    endif ()
endfunction()

# --- Command line tool
add_executable(redstone_cli src/app/main.cpp)
target_link_libraries(redstone_cli PRIVATE redstone_core)
target_compile_options(redstone_cli PRIVATE ${REDSTONE_WARNINGS})
redstone_copy_runtime_dlls(redstone_cli)

# --- Benchmark: times the solver on bench/*.txt
add_executable(redstone_bench src/bench/main.cpp)
target_link_libraries(redstone_bench PRIVATE redstone_core)
target_compile_options(redstone_bench PRIVATE ${REDSTONE_WARNINGS})
redstone_copy_runtime_dlls(redstone_bench)

add_executable(redstone_satbench src/bench/sat_main.cpp)
target_link_libraries(redstone_satbench PRIVATE redstone_core)
redstone_copy_runtime_dlls(redstone_satbench)

# miniz reads block textures from the user's Minecraft jar.
FetchContent_Declare(miniz
        GIT_REPOSITORY https://github.com/richgel999/miniz.git
        GIT_TAG 3.1.2
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(miniz)

# --- Desktop app: native window (webview) showing the web UI from web/,
# served with its JSON API from 127.0.0.1.
# A localhost server needs no TLS or compression; probing for them would pick
# up unrelated toolchains' libraries (e.g. MSYS2's zlib).
foreach (feature OPENSSL ZLIB BROTLI ZSTD)
    set(HTTPLIB_USE_${feature}_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
endforeach ()
FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.60.1
        GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CLANG_FORMAT OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(webview
        GIT_REPOSITORY https://github.com/webview/webview.git
        GIT_TAG 0.12.0
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(httplib nlohmann_json webview)

find_program(NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)
set(WEB_DIR ${CMAKE_SOURCE_DIR}/web)
set(WEB_DIST ${CMAKE_BINARY_DIR}/web_dist)
file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS ${WEB_DIR}/src/*)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/web_node_modules.stamp
        COMMAND ${NPM_EXECUTABLE} ci --no-audit --no-fund
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/web_node_modules.stamp
        DEPENDS ${WEB_DIR}/package-lock.json
        WORKING_DIRECTORY ${WEB_DIR}
        COMMENT "Installing web UI packages")
add_custom_command(OUTPUT ${WEB_DIST}/index.html
        COMMAND ${NPM_EXECUTABLE} run build -- --outDir ${WEB_DIST}
        DEPENDS ${CMAKE_BINARY_DIR}/web_node_modules.stamp ${WEB_SOURCES}
        ${WEB_DIR}/index.html ${WEB_DIR}/vite.config.ts ${WEB_DIR}/tsconfig.json
        WORKING_DIRECTORY ${WEB_DIR}
        COMMENT "Building the web UI")
set(EMBEDDED_CPP ${CMAKE_BINARY_DIR}/generated/embedded_files.cpp)
add_custom_command(OUTPUT ${EMBEDDED_CPP}
        COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${WEB_DIST} -DOUTPUT=${EMBEDDED_CPP}
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
        DEPENDS ${WEB_DIST}/index.html ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
        COMMENT "Embedding the web UI")

add_executable(redstone_solver WIN32
        src/host/main.cpp
        src/host/api.cpp
        src/host/json_io.cpp
        src/host/minecraft_assets.cpp
        ${EMBEDDED_CPP})
target_link_libraries(redstone_solver PRIVATE redstone_core miniz httplib::httplib
        nlohmann_json::nlohmann_json webview::core_static)
target_compile_options(redstone_solver PRIVATE ${REDSTONE_WARNINGS})
if (MSVC)
    # Keep the standard main() entry point for a windowed (no console) app.
    target_link_options(redstone_solver PRIVATE /ENTRY:mainCRTStartup)
endif ()
redstone_copy_runtime_dlls(redstone_solver)
add_custom_command(TARGET redstone_solver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/examples $<TARGET_FILE_DIR:redstone_solver>/examples)

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
            tests/test_cp_sat.cpp
            tests/test_spec.cpp
            tests/test_encoder.cpp
            tests/test_solver.cpp
            tests/test_sat.cpp
            tests/test_verifier.cpp
            tests/test_report.cpp)
    target_link_libraries(redstone_tests PRIVATE redstone_core Catch2::Catch2WithMain)
    target_compile_options(redstone_tests PRIVATE ${REDSTONE_WARNINGS})
    redstone_copy_runtime_dlls(redstone_tests)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(redstone_tests DISCOVERY_MODE PRE_TEST)
endif ()
