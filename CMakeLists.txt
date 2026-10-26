cmake_minimum_required(VERSION 3.16)
project(mclc VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(mclc
    src/main.c
    src/util.c
    src/json.c
    src/hash.c
    src/http.c
    src/inflate.c
    src/zip.c
    src/version.c
    src/install.c
    src/remote.c
    src/launch.c
)

target_compile_definitions(mclc PRIVATE
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(mclc PRIVATE /std:c11 /W4 /utf-8)
    target_link_libraries(mclc PRIVATE winhttp shell32)
else()
    target_compile_options(mclc PRIVATE -Wall -Wextra -Wpedantic -O2)
    target_link_options(mclc PRIVATE -static -static-libgcc)
    target_link_libraries(mclc PRIVATE winhttp shell32)
endif()

if(MINGW)
    # Pure Win32 application entry point (we use main + GetCommandLineW).
    target_link_options(mclc PRIVATE -mconsole)
endif()
