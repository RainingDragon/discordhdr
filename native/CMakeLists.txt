cmake_minimum_required(VERSION 3.24)

project(DiscordHDRFixNative LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DiscordHDRFix native patch is Windows-only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build x64. Discord desktop is 64-bit.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(DiscordHDRFixNative SHARED
    probe/dllmain.cpp
)

set_target_properties(DiscordHDRFixNative PROPERTIES
    OUTPUT_NAME "DiscordHDRFix.Native"
)

target_compile_definitions(DiscordHDRFixNative PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_compile_options(DiscordHDRFixNative PRIVATE
    /W4
    /permissive-
)

add_executable(DiscordHDRFixInjector
    injector/main.cpp
)

set_target_properties(DiscordHDRFixInjector PROPERTIES
    OUTPUT_NAME "DiscordHDRFix.Injector"
)

target_compile_definitions(DiscordHDRFixInjector PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_compile_options(DiscordHDRFixInjector PRIVATE
    /W4
    /permissive-
)
