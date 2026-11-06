@echo off
rem Збирання з Visual Studio (запускати з "x64 Native Tools Command Prompt")
cd /d %~dp0
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
copy /y build\Release\KairoBoost.exe . >nul
copy /y build\Release\KairoBoost-cli.exe . >nul
echo Готово: KairoBoost.exe (GUI), KairoBoost-cli.exe (консоль)
