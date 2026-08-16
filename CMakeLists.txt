cmake_minimum_required(VERSION 3.20)
project(RemoteCodeServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4)
endif()

add_executable(RemoteCodeServer
    src/main.cpp
)

target_include_directories(RemoteCodeServer PRIVATE src)

enable_testing()
add_subdirectory(tests)
