@echo off
rem ============================================================
rem  MarkPeek build script (MinGW-W64 i686, C:\PORTABLE\mingw32)
rem  Usage: build.bat   -> produces dist\MarkPeek.exe
rem ============================================================
setlocal
set MINGW=C:\PORTABLE\mingw32
set ROOT=%~dp0
set SRC=%ROOT%src
set OUT=%ROOT%dist
if not exist "%OUT%" mkdir "%OUT%"

pushd "%SRC%"
"%MINGW%\bin\windres.exe" app.rc -O coff -o appres.o        || goto :err
"%MINGW%\bin\gcc.exe"    -O2 -c md4c\md4c.c        -o md4c.o        || goto :err
"%MINGW%\bin\gcc.exe"    -O2 -c md4c\md4c-html.c   -o md4c-html.o   || goto :err
"%MINGW%\bin\gcc.exe"    -O2 -c md4c\entity.c      -o entity.o      || goto :err
"%MINGW%\bin\g++.exe"    -O2 -c main.cpp           -o main.o        || goto :err
"%MINGW%\bin\g++.exe" main.o md4c.o md4c-html.o entity.o appres.o ^
    -o "%OUT%\MarkPeek.exe" -mwindows -static ^
    -lole32 -loleaut32 -luuid -lcomctl32 -lshlwapi           || goto :err
popd

del /q "%SRC%\*.o" 2>nul
echo.
echo Build OK: %OUT%\MarkPeek.exe
exit /b 0

:err
popd
echo.
echo Build FAILED.
exit /b 1