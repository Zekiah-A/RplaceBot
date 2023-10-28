cmake_minimum_required(VERSION 3.27)
project(RplaceBot C)

set(CMAKE_C_STANDARD 11)

include_directories(lib)

add_subdirectory(lib/telebot)

add_executable(RplaceBot
        lib/parson.c
        lib/parson.h
        main.c)

target_link_libraries(RplaceBot PRIVATE telebot)
target_link_libraries(RplaceBot PRIVATE pthread discord curl png sqlite3)