cmake_minimum_required(VERSION 3.15)
project(LukashBoost CXX RC)
set(CMAKE_CXX_STANDARD 17)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  add_compile_options(/utf-8)
endif()

set(IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)

# Графічна версія
add_executable(LukashBoost WIN32
  src/gui.cpp src/core.cpp src/app.rc
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(LukashBoost PRIVATE src ${IMGUI} ${IMGUI}/backends)
target_link_libraries(LukashBoost d3d11 d3dcompiler dxgi dwmapi gdi32 ws2_32 iphlpapi advapi32)

# Консольна версія
add_executable(LukashBoost-cli src/console.cpp src/core.cpp src/app.rc)
target_include_directories(LukashBoost-cli PRIVATE src)
target_link_libraries(LukashBoost-cli ws2_32 iphlpapi advapi32)

foreach(t LukashBoost LukashBoost-cli)
  if(MSVC)
    target_link_options(${t} PRIVATE /MANIFEST:NO)  # маніфест уже в app.rc
  else()
    target_link_options(${t} PRIVATE -static -s)
  endif()
endforeach()
