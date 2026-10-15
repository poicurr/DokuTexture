cmake_minimum_required(VERSION 3.18)
project(DokuTextureConsumer LANGUAGES CXX)
# The linked library must propagate its C++17 requirement to this parent target.
set(CMAKE_CXX_STANDARD 14)
add_subdirectory(../.. dokutexture)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE DokuTexture::DokuTexture)
