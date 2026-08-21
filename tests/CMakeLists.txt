add_executable(RemoteCodeServerTests
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/src/third_party/sqlite3.c
    ${CMAKE_SOURCE_DIR}/src/db/Database.cpp
    ${CMAKE_SOURCE_DIR}/src/db/Schema.cpp
    ${CMAKE_SOURCE_DIR}/src/db/AgentStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ChatStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ApprovalStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/AgentSessionStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ChatSummaryStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/RepoStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/WorkspaceStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/TempPermissionStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/PromptTemplateStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/TaskStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ReminderStore.cpp
    ${CMAKE_SOURCE_DIR}/src/db/SubagentTemplateStore.cpp
    ${CMAKE_SOURCE_DIR}/src/config/Secrets.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpServer.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/Tools.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestrator/WorkspaceCreator.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestrator/WorkspacePr.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestrator/GitHubOps.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestrator/RepoImport.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Text.cpp
    ${CMAKE_SOURCE_DIR}/src/util/ActivityLog.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Mentions.cpp
    ${CMAKE_SOURCE_DIR}/src/util/GitHubRepo.cpp
    ${CMAKE_SOURCE_DIR}/src/util/ProcessRunner.cpp
)
target_include_directories(RemoteCodeServerTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(RemoteCodeServerTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME RemoteCodeServerTests COMMAND RemoteCodeServerTests)
