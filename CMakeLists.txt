cmake_minimum_required(VERSION 3.16)
project(BinanceDemo)

set(CMAKE_CXX_STANDARD 11)
add_executable(Main main.cpp)
target_include_directories(Main PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(Main -lcurl -pthread -lssl -lcrypto)