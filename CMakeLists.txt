cmake_minimum_required(VERSION 3.20)
project(DJY_EvTLMT LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

add_executable(DJY_EvTLMT WIN32
    src/main.cpp
    src/ui/DashboardUI.cpp
    src/telemetry/TelemetrySender.cpp
    src/telemetry/EnergyLog.cpp
    src/telemetry/VirtualLogRecorder.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(DJY_EvTLMT PRIVATE
    src/ui
    src/telemetry
    third_party/json
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(DJY_EvTLMT PRIVATE ws2_32 d3d11 d3dcompiler comdlg32)

if(MSVC)
    target_compile_definitions(DJY_EvTLMT PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
    target_compile_options(DJY_EvTLMT PRIVATE /utf-8 /W4)
endif()
