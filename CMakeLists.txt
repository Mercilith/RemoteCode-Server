cmake_minimum_required(VERSION 3.20)
project(RemoteCodeServer C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4)
endif()

add_executable(RemoteCodeServer
    src/main.cpp
    src/third_party/monocypher.c
    src/identity/ServerIdentity.cpp
    src/topic/TopicDerivation.cpp
    src/mqtt/WebSocketTransport.cpp
    src/mqtt/MqttPacket.cpp
    src/mqtt/MqttClient.cpp
    src/service/ServiceMain.cpp
)

target_include_directories(RemoteCodeServer PRIVATE src)

target_compile_definitions(RemoteCodeServer PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(RemoteCodeServer PRIVATE
    winhttp
    advapi32
    crypt32
    bcrypt
    shell32
)

enable_testing()
add_subdirectory(tests)
