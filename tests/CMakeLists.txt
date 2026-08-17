add_executable(RemoteCodeServerTests
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/src/third_party/monocypher.c
    ${CMAKE_SOURCE_DIR}/src/topic/TopicDerivation.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt/MqttPacket.cpp
)
target_include_directories(RemoteCodeServerTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME RemoteCodeServerTests COMMAND RemoteCodeServerTests)
