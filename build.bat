@echo off
rem NetShutter 빌드 - MSYS2/MinGW-w64 (g++) 필요
rem   설치: https://www.msys2.org  →  pacman -S mingw-w64-ucrt-x86_64-gcc
cd /d "%~dp0src"
windres netmeter.rc -O coff -o netmeter.res.o || goto err
g++ -std=c++17 -municode -O2 -s -mwindows -static -static-libgcc -static-libstdc++ ^
  -o ..\NetShutter.exe main.cpp netmeter.res.o ^
  -lgdiplus -lole32 -loleaut32 -luuid -liphlpapi -ldwmapi -lshell32 -ladvapi32 -lversion -lntdll -lws2_32 -lgdi32 -luser32 || goto err
del netmeter.res.o 2>nul
echo.
echo 완료: NetShutter.exe
goto :eof
:err
echo 빌드 실패
exit /b 1
