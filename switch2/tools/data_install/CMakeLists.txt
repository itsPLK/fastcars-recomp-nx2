# data_install (Linux host tool for the Switch 2 build container): the game data install
# (install.cpp) in a terminal. See main.cpp.
cmake_minimum_required(VERSION 3.25)
project(fastcars_data_install LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(data_install main.cpp install.cpp sources.cpp)
target_link_libraries(data_install PRIVATE pthread)
