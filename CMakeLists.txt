cmake_minimum_required(VERSION 3.16)

project(banking_management_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(bank main.cpp)

if(MSVC)
    target_compile_options(bank PRIVATE /W4)
else()
    target_compile_options(bank PRIVATE -Wall -Wextra -pedantic)
endif()
