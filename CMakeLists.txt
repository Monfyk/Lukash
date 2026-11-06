cmake_minimum_required(VERSION 3.15)
project(KairoBoost CXX RC)
set(CMAKE_CXX_STANDARD 17)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  add_compile_options(/utf-8)
endif()

set(IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(CORE src/core.cpp src/tweaks.cpp src/fortnite.cpp src/dns.cpp)
set(CORE_LIBS ws2_32 iphlpapi advapi32 powrprof psapi)

# Графічна версія
add_executable(KairoBoost WIN32
  src/gui.cpp ${CORE} src/app.rc
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(KairoBoost PRIVATE src ${IMGUI} ${IMGUI}/backends)
target_link_libraries(KairoBoost d3d11 d3dcompiler dxgi dwmapi gdi32 shell32 ${CORE_LIBS})

# Консольна версія
add_executable(KairoBoost-cli src/console.cpp ${CORE} src/app.rc)
target_include_directories(KairoBoost-cli PRIVATE src)
target_link_libraries(KairoBoost-cli ${CORE_LIBS})

foreach(t KairoBoost KairoBoost-cli)
  if(MSVC)
    target_link_options(${t} PRIVATE /MANIFEST:NO)  # маніфест уже в app.rc
  else()
    target_link_options(${t} PRIVATE -static -s)
  endif()
endforeach()
