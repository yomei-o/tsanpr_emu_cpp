@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\prog\claude\tsanpr_emu_cpp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release 1>build_configure.log 2>&1
cmake --build build --config Release 1>build_compile.log 2>&1
echo DONE exit %ERRORLEVEL%
