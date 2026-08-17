# Auto-detect a vcpkg toolchain via the standard VCPKG_ROOT env var, unless
# one was already passed explicitly via -DCMAKE_TOOLCHAIN_FILE=. Must happen
# before project() — that's when CMake applies the toolchain file.
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

cmake_minimum_required(VERSION 3.21)
project(RemoteCodeServer C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4)
endif()

# --- DPP (Discord library) ---
# Prefer a vcpkg-built dpp (configure with VCPKG_ROOT set, or pass
# -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake directly) —
# vcpkg's dpp port links against a current OpenSSL 3.x.
#
# The fallback below does NOT use DPP's own official GitHub-release
# binaries: those bundle OpenSSL 1.1.1 (EOL since 2023), which fails the TLS
# handshake against Discord's current Cloudflare-fronted API on every REST
# call ("Malformed HTTP response" from https_client::get_status() —
# confirmed via direct testing that curl and a current OpenSSL both connect
# to the same endpoint cleanly, only DPP's bundled 1.1.1 client fails).
# Instead it downloads a self-hosted repackaging of the exact same vcpkg
# build the primary path above uses (built once, uploaded to a
# RemoteCode-Server release — see its release notes for how it was built),
# so RemoteCode-Installer's automated build — which doesn't provision vcpkg
# itself — still ends up with working Discord connectivity, without paying
# a ~20-minute OpenSSL/zlib/opus/dpp source-build cost on every install.
find_package(dpp CONFIG QUIET)

if (dpp_FOUND)
    set(DPP_TARGET dpp::dpp)
else()
    include(FetchContent)
    FetchContent_Declare(
        dpp_prebuilt
        URL "https://github.com/Mercilith/RemoteCode-Server/releases/download/libdpp-openssl3-win64-v1/libdpp-openssl3-win64.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(dpp_prebuilt)
    set(DPP_ROOT "${dpp_prebuilt_SOURCE_DIR}")

    add_library(dpp_imported SHARED IMPORTED)
    set_target_properties(dpp_imported PROPERTIES
        IMPORTED_LOCATION "${DPP_ROOT}/bin/dpp.dll"
        IMPORTED_IMPLIB "${DPP_ROOT}/lib/dpp.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${DPP_ROOT}/include"
    )
    set(DPP_TARGET dpp_imported)
endif()

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
    ${DPP_TARGET}
)

# DPP's DLL and its own runtime dependencies (OpenSSL, opus, zlib) must sit
# next to the exe to actually run — copied here for local dev/testing;
# RemoteCode-Installer's build step copies every DLL found in the build
# output directory into the install directory for real deployments (see its
# AppController.cpp), so either path below is covered.
if (dpp_FOUND)
    # vcpkg's own runtime dependency tracking — copies dpp.dll plus its
    # transitive OpenSSL/opus/zlib DLLs, whatever the exact set is.
    add_custom_command(TARGET RemoteCodeServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:RemoteCodeServer>
            "$<TARGET_FILE_DIR:RemoteCodeServer>"
        COMMAND_EXPAND_LISTS
    )
else()
    add_custom_command(TARGET RemoteCodeServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DPP_ROOT}/bin/dpp.dll"
            "${DPP_ROOT}/bin/libcrypto-3-x64.dll"
            "${DPP_ROOT}/bin/libssl-3-x64.dll"
            "${DPP_ROOT}/bin/opus.dll"
            "${DPP_ROOT}/bin/z.dll"
            "$<TARGET_FILE_DIR:RemoteCodeServer>"
    )
endif()

enable_testing()
add_subdirectory(tests)
