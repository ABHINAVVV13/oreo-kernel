@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >/dev/null 2>&1
cd /d C:\Users\AbhinavPutta\Desktop\projects\oreoCAD\oreo-kernel
cmake --preset default 2>&1
cmake --build build 2>&1
ctest --test-dir build --output-on-failure 2>&1
