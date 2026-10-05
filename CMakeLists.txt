cmake_minimum_required(VERSION 3.16)
project(SafeClaude CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(SafeClaude WIN32 src/main.cpp src/app.rc)
target_link_libraries(SafeClaude PRIVATE gdiplus dwmapi shell32 gdi32 -static -mwindows -municode)
target_compile_options(SafeClaude PRIVATE -O2 -Wall)
set_target_properties(SafeClaude PROPERTIES OUTPUT_NAME "Safe Claude")
