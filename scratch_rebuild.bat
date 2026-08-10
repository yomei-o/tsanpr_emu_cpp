@echo off
cd /d C:\prog\claude\tsanpr_emu_cpp
"C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --target x86emu 1>build_compile.log 2>&1
echo DONE exit %ERRORLEVEL%
