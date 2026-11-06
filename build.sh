#!/bin/sh
# Крос-збирання з Linux (потрібен пакет g++-mingw-w64-x86-64)
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres src/app.rc -O coff -o app.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -s src/main.cpp app.res \
  -o LukashBoost.exe -lws2_32 -liphlpapi -ladvapi32
rm -f app.res
echo "Готово: LukashBoost.exe"
