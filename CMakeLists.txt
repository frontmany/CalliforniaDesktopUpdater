cmake_minimum_required(VERSION 3.24)

# Deliberately no VERSION: the updater is not versioned or released on its
# own. It ships inside the client's release tree and is identified by the
# client release's version (CallsApp/release.json) via the signed manifest.
project(Updater
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

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/ed25519/src/ed25519.h")
    message(FATAL_ERROR "Missing deps/ed25519. See deps/README.md.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/json/include/nlohmann/json.hpp")
    message(FATAL_ERROR "Missing deps/json. See deps/README.md.")
endif()

# orlp/ed25519 (public domain), full upstream clone (see deps/README.md).
# We only need signature *verification*: compile the arithmetic (field/group/
# scalar), SHA-512, and verify.c — not sign.c/keypair.c/key_exchange.c/seed.c
# (the updater never signs; the private key never leaves the publish machine).
add_library(ed25519_verify STATIC
    deps/ed25519/src/fe.c
    deps/ed25519/src/ge.c
    deps/ed25519/src/sc.c
    deps/ed25519/src/sha512.c
    deps/ed25519/src/verify.c
)
target_include_directories(ed25519_verify PUBLIC deps/ed25519/src)

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
target_include_directories(applylib PUBLIC src deps/json/include)
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
