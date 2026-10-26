cmake_minimum_required(VERSION 3.16)
project(mclc VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(MCLC_CORE
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
    src/progress.c
)

add_executable(mclc src/main.c ${MCLC_CORE})
add_executable(mclc_gui WIN32 src/gui.c ${MCLC_CORE})

foreach(target mclc mclc_gui)
    target_compile_definitions(${target} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(${target} PRIVATE winhttp shell32)
    if(MSVC)
        target_compile_options(${target} PRIVATE /std:c11 /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -O2)
        target_link_options(${target} PRIVATE -static -static-libgcc)
    endif()
endforeach()

target_link_libraries(mclc_gui PRIVATE comdlg32 ole32 user32 gdi32 comctl32)

if(MINGW)
    # mclc_gui is a WIN32 target, so CMake adds -mwindows automatically.
    # mclc remains a console application for automation and diagnostics.
    set_target_properties(mclc_gui PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
