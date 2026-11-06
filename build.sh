#!/bin/sh
# Крос-збирання з Linux (потрібен пакет g++-mingw-w64-x86-64)
set -e
cd "$(dirname "$0")"
CXX=x86_64-w64-mingw32-g++
FLAGS="-std=c++17 -O2 -static -s"
IMGUI=third_party/imgui
x86_64-w64-mingw32-windres src/app.rc -O coff -o app.res

# Графічна версія
$CXX $FLAGS -mwindows -Isrc -I$IMGUI -I$IMGUI/backends \
  src/gui.cpp src/core.cpp $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp \
  $IMGUI/imgui_widgets.cpp $IMGUI/backends/imgui_impl_win32.cpp $IMGUI/backends/imgui_impl_dx11.cpp app.res \
  -o LukashBoost.exe -ld3d11 -ld3dcompiler -ldxgi -ldwmapi -lgdi32 -lws2_32 -liphlpapi -ladvapi32

# Консольна версія
$CXX $FLAGS -Wall -Wextra src/console.cpp src/core.cpp app.res \
  -o LukashBoost-cli.exe -lws2_32 -liphlpapi -ladvapi32

rm -f app.res
echo "Готово: LukashBoost.exe (GUI), LukashBoost-cli.exe (консоль)"
