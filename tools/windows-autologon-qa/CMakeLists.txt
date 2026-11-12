cmake_minimum_required(VERSION 3.20)
project(MultitapWindowsAutologonQA LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(MultitapWindowsAutologonMemoryTest memory_test.cpp)
if(MSVC)
  target_compile_options(MultitapWindowsAutologonMemoryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
# No native adapter target is linked or run by this isolated memory suite.
