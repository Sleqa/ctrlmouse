cmake_minimum_required(VERSION 3.15)
project(ctrlmouse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_language(RC)

# WIN32 -> /SUBSYSTEM:WINDOWS, so no console window appears.
add_executable(ctrlmouse WIN32 ctrlmouse.cpp ctrlmouse.rc)

target_link_libraries(ctrlmouse PRIVATE
    user32
    gdi32
    comctl32
    shell32
    dinput8
    dxguid
    dwmapi
    d2d1
    dwrite
    hid
    setupapi
    advapi32
)
