@echo off
rem Windows build with TDM-GCC (32-bit target).
setlocal
if "%CXX%"=="" set CXX=C:\TDM-GCC-64\bin\g++.exe
if not exist "%CXX%" (
    echo [error] compiler not found: %CXX%
    echo Set CXX to your i686 g++ path.
    exit /b 1
)
"%CXX%" -m32 -shared -O2 -Wall -static-libgcc -static-libstdc++ -o re5_chaindbg.asi src\dllmain.cpp
if errorlevel 1 (
    echo [error] build failed
    exit /b 1
)
echo [ok] re5_chaindbg.asi
