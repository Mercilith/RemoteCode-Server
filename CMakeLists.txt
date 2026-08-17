cmake_minimum_required(VERSION 3.20)
project(RemoteCodeServer C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4)
endif()

# --- DPP (Discord library) — prebuilt Windows binaries ---
# Bypassing DPP's own CMake config package (find_package(dpp CONFIG)) on
# purpose: it calls find_dependency(OpenSSL REQUIRED), which needs a real
# OpenSSL *development* install (headers + .lib, discoverable by CMake) —
# not present in this environment, and not actually needed, since the
# prebuilt dpp.dll already bundles libssl/libcrypto at runtime. Linking the
# .lib/.dll directly sidesteps that unnecessary configure-time dependency.
set(DPP_VERSION "10.1.6")
set(DPP_VERSION_SHORT "10.1") # DPP's own include/lib subfolders use MAJOR.MINOR only
include(FetchContent)
FetchContent_Declare(
    dpp_prebuilt
    URL "https://github.com/brainboxdotcc/DPP/releases/download/v${DPP_VERSION}/libdpp-${DPP_VERSION}-win64-release-vs2022.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(dpp_prebuilt)
# FetchContent strips the archive's single top-level folder automatically,
# so the populated source dir *is* the package root (bin/include/lib
# directly inside it) — not <dir>/libdpp-<version>-win64/... as the zip's
# own internal layout might suggest.
set(DPP_ROOT "${dpp_prebuilt_SOURCE_DIR}")

add_library(dpp_imported SHARED IMPORTED)
set_target_properties(dpp_imported PROPERTIES
    IMPORTED_LOCATION "${DPP_ROOT}/bin/dpp.dll"
    IMPORTED_IMPLIB "${DPP_ROOT}/lib/dpp-${DPP_VERSION_SHORT}/dpp.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${DPP_ROOT}/include/dpp-${DPP_VERSION_SHORT}"
)

add_executable(RemoteCodeServer
    src/main.cpp
    src/service/ServiceMain.cpp
    src/third_party/sqlite3.c
    src/db/Database.cpp
    src/db/Schema.cpp
    src/db/AgentStore.cpp
    src/db/ChatStore.cpp
    src/config/ServerConfig.cpp
    src/discord/DiscordBot.cpp
    src/mcp/McpServer.cpp
    src/mcp/Tools.cpp
    src/orchestrator/AgentTurn.cpp
    src/orchestrator/Orchestrator.cpp
)

target_include_directories(RemoteCodeServer PRIVATE src)

target_compile_definitions(RemoteCodeServer PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(RemoteCodeServer PRIVATE
    advapi32
    shell32
    crypt32
    dpp_imported
)

# DPP's DLL and its own runtime dependencies (OpenSSL, opus, zlib) must sit
# next to the exe to actually run — copied here for local dev/testing;
# RemoteCode-Installer's build step copies the same set into the install
# directory for real deployments (see its AppController.cpp).
add_custom_command(TARGET RemoteCodeServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DPP_ROOT}/bin/dpp.dll"
        "${DPP_ROOT}/bin/libcrypto-1_1-x64.dll"
        "${DPP_ROOT}/bin/libssl-1_1-x64.dll"
        "${DPP_ROOT}/bin/opus.dll"
        "${DPP_ROOT}/bin/zlib1.dll"
        "$<TARGET_FILE_DIR:RemoteCodeServer>"
)

enable_testing()
add_subdirectory(tests)
