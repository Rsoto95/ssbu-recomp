@echo off
rem Builds ssbu_glslang.dll (vk_glslang_shim.c + the glslang 16.6.0 release libraries) with MSVC. Run from the source folder
rem (scripts\setup.py does). GLSLANG_ROOT = the folder of the glslang release (include\ and lib\); default ..\reference\upstream\glslang-16.6.0.
rem See plan\phases\V1_vulkan.md section 4 for why this is a DLL and not linked into the exe.
setlocal
if "%GLSLANG_ROOT%"=="" set GLSLANG_ROOT=..\reference\upstream\glslang-16.6.0
set GL=%GLSLANG_ROOT%
set VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSDIR=
if exist "%VSW%" "%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\ssbu_vsdir.txt"
if exist "%TEMP%\ssbu_vsdir.txt" set /p VSDIR=<"%TEMP%\ssbu_vsdir.txt"
if "%VSDIR%"=="" ( echo build_glslang: no Visual Studio / Build Tools with the C++ x64 tools found & exit /b 1 )
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
rem (2026-11-12) Static C / C++ runtime when the glslang libraries were built with it: "<GLSLANG_ROOT>-mt" (deps\glslang-mt,
rem made by scripts\build_glslang_static.bat) is taken when it is there. The official release libraries are /MD: the DLL then
rem needs the player's MSVCP140.dll, and one older than 14.40 crashes at the first shader. scripts\make_release.py refuses
rem to package such a DLL.
if exist "%GL%-mt\lib\glslang.lib" set GL=%GL%-mt
set CRT=/MD
dumpbin /nologo /directives "%GL%\lib\glslang.lib" | findstr /C:"RuntimeLibrary=MT_StaticRelease" >nul && set CRT=/MT
echo build_glslang: %GL% (%CRT%)
if not exist obj_host mkdir obj_host
cl /nologo %CRT% /O2 /W3 /LD /I "%GL%\include" vk_glslang_shim.c /Fo:obj_host\vk_glslang_shim.obj /Fe:ssbu_glslang.dll /link /LIBPATH:"%GL%\lib" glslang.lib glslang-default-resource-limits.lib SPIRV-Tools-opt.lib SPIRV-Tools.lib
exit /b %errorlevel%
