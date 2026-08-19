cmake_minimum_required(VERSION 3.16)

project(icc_switch VERSION 1.0.0 LANGUAGES CXX RC)

add_executable(icc-switch src/main.cpp)
target_compile_features(icc-switch PRIVATE cxx_std_17)
target_compile_definitions(icc-switch PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(icc-switch PRIVATE user32)

add_executable(icc-switch-gui WIN32 src/gui.cpp src/app.rc)
target_compile_features(icc-switch-gui PRIVATE cxx_std_17)
target_compile_definitions(icc-switch-gui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(icc-switch-gui PRIVATE user32 gdi32 comdlg32)

if(MSVC)
    target_compile_options(icc-switch PRIVATE /utf-8 /W4)
    target_compile_options(icc-switch-gui PRIVATE /utf-8 /W4)
else()
    target_compile_options(icc-switch PRIVATE -Wall -Wextra -Wpedantic -Wno-cast-function-type)
    target_compile_options(icc-switch-gui PRIVATE -Wall -Wextra -Wpedantic -Wno-cast-function-type)
    target_link_options(icc-switch PRIVATE -municode)
    target_link_options(icc-switch-gui PRIVATE -municode)
endif()
