cmake_minimum_required(VERSION 3.10)
project(_row_data_encoder)

set(CMAKE_CXX_STANDARD 14)

add_executable(_row_data_encoder main.cpp)

target_link_libraries(_row_data_encoder avformat avcodec avutil pthread)