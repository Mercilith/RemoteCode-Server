add_executable(RemoteCodeServerTests
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/src/third_party/sqlite3.c
    ${CMAKE_SOURCE_DIR}/src/db/Database.cpp
    ${CMAKE_SOURCE_DIR}/src/db/Schema.cpp
    ${CMAKE_SOURCE_DIR}/src/db/AgentStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ChatStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ApprovalStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/AgentSessionStore.cpp
    ${CMAKE_SOURCE_DIR}/src/config/Secrets.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpServer.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/Tools.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Text.cpp
)
target_include_directories(RemoteCodeServerTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(RemoteCodeServerTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME RemoteCodeServerTests COMMAND RemoteCodeServerTests)
