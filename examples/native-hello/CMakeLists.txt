# apkfast não usa CMake, mas este arquivo ajuda editores a indexarem o código.
cmake_minimum_required(VERSION 3.22)
project(native_hello C CXX)
add_library(main SHARED main.cpp)
