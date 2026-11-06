@echo off
rem Збирання з Visual Studio (запускати з "x64 Native Tools Command Prompt")
cd /d %~dp0
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
copy /y build\Release\LukashBoost.exe . >nul
copy /y build\Release\LukashBoost-cli.exe . >nul
echo Готово: LukashBoost.exe (GUI), LukashBoost-cli.exe (консоль)
