@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\prog\claude\tsanpr_emu_cpp
cl /nologo /EHsc /O2 /Fe:ndbg.exe scratch_ndbg.cpp /link psapi.lib 1>ndbg_build.log 2>&1
echo DONE exit %ERRORLEVEL%
