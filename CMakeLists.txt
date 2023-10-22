cmake_minimum_required(VERSION 3.27)
project(RplaceBot C)

set(CMAKE_C_STANDARD 11)

include_directories(lib)

add_executable(RplaceBot
        lib/parson.c
        lib/parson.h
        main.c)

target_link_libraries(RplaceBot -lpthread)
target_link_libraries(RplaceBot -ldiscord)
target_link_libraries(RplaceBot -lcurl)
target_link_libraries(RplaceBot -lpng)
target_link_libraries(RplaceBot -lsqlite3)