@echo off
rem Збирання з Visual Studio (запускати з "x64 Native Tools Command Prompt")
cd /d %~dp0
rc /nologo /fo src\app.res src\app.rc || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /MT src\main.cpp src\app.res /Fe:LukashBoost.exe /link /MANIFEST:NO ws2_32.lib iphlpapi.lib advapi32.lib || exit /b 1
del main.obj src\app.res 2>nul
echo Готово: LukashBoost.exe
