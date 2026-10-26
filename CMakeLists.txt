cmake_minimum_required(VERSION 3.16)
project(mclc VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(WIN32)
    enable_language(RC)
endif()

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

add_executable(mclc_gui WIN32
    src/gui.c
    src/mclc.rc
    ${MCLC_CORE}
)

target_compile_definitions(mclc_gui PRIVATE
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)
target_include_directories(mclc_gui PRIVATE src)
target_link_libraries(mclc_gui PRIVATE
    winhttp shell32 comdlg32 ole32 user32 gdi32 comctl32
)
if(MSVC)
    target_compile_options(mclc_gui PRIVATE /std:c11 /W4 /utf-8)
    target_link_options(mclc_gui PRIVATE /MANIFEST:NO)
else()
    target_compile_options(mclc_gui PRIVATE
        -Wall -Wextra -Wpedantic -O2
    )
    target_link_options(mclc_gui PRIVATE -static -static-libgcc)
endif()
