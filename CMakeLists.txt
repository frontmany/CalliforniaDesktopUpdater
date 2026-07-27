cmake_minimum_required(VERSION 3.24)

project(Updater
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Calls client update applier"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT: the updater replaces DLLs next to itself, so it must depend on
# exactly nothing at runtime (no Qt, no OpenSSL, no vcruntime DLLs).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Vendored Ed25519 (orlp/ed25519, verify-only subset; public domain).
add_library(ed25519_verify STATIC
    deps/ed25519/fe.c
    deps/ed25519/ge.c
    deps/ed25519/sc.c
    deps/ed25519/sha512.c
    deps/ed25519/verify.c
)
target_include_directories(ed25519_verify PUBLIC deps/ed25519)

# Everything except the entry point lives in a static lib so tests link it.
add_library(applylib STATIC
    src/apply.cpp
    src/args.cpp
    src/fsops.cpp
    src/journal.cpp
    src/log.cpp
    src/manifest.cpp
    src/sha256.cpp
    src/verify.cpp
)
target_include_directories(applylib PUBLIC src deps/json)
target_link_libraries(applylib PUBLIC ed25519_verify)
if(WIN32)
    target_link_libraries(applylib PUBLIC bcrypt shell32 advapi32)
endif()
if(MSVC)
    target_compile_definitions(applylib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# WIN32 subsystem: no console window flashes when the app or RunOnce launches it.
add_executable(updater WIN32 src/main.cpp)
target_link_libraries(updater PRIVATE applylib)
set_target_properties(updater PROPERTIES OUTPUT_NAME "updater")

option(UPDATER_BUILD_TESTS "Build updater unit tests" ON)
if(UPDATER_BUILD_TESTS)
    enable_testing()
    add_executable(updater_tests tests/tests.cpp)
    target_link_libraries(updater_tests PRIVATE applylib)
    add_test(NAME updater_tests COMMAND updater_tests)
endif()
