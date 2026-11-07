#!/bin/sh
# Крос-збирання з Linux (потрібен пакет g++-mingw-w64-x86-64)
set -e
cd "$(dirname "$0")"
CXX=x86_64-w64-mingw32-g++
FLAGS="-std=c++17 -O2 -static -s"
IMGUI=third_party/imgui
CORE="src/core.cpp src/tweaks.cpp src/fortnite.cpp src/dns.cpp src/advisor.cpp src/games.cpp"
LIBS="-lws2_32 -liphlpapi -ladvapi32 -lpowrprof -lpsapi"
GUILIBS="-ld3d11 -ld3dcompiler -ldxgi -ldwmapi -lgdi32 -lshell32 -lcomdlg32"
IMGUI_SRC="$IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
  $IMGUI/backends/imgui_impl_win32.cpp $IMGUI/backends/imgui_impl_dx11.cpp"
x86_64-w64-mingw32-windres src/app.rc -O coff -o app.res

# Особиста версія (Fortnite)
$CXX $FLAGS -mwindows -Isrc -I$IMGUI -I$IMGUI/backends src/gui.cpp $CORE $IMGUI_SRC app.res \
  -o KairoBoost.exe $GUILIBS $LIBS

# Версія для продажу (будь-яка гра)
$CXX $FLAGS -DKB_SALE -mwindows -Isrc -I$IMGUI -I$IMGUI/backends src/gui.cpp $CORE $IMGUI_SRC app.res \
  -o KairoBoostPro.exe $GUILIBS $LIBS

# Консольна версія
$CXX $FLAGS -Wall -Wextra -Wno-missing-field-initializers -Isrc src/console.cpp $CORE app.res \
  -o KairoBoost-cli.exe $LIBS

rm -f app.res
echo "Готово: KairoBoost.exe (особиста), KairoBoostPro.exe (для продажу), KairoBoost-cli.exe (консоль)"
