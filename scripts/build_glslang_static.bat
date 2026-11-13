@echo off
rem Builds glslang 16.6.0 (+ its SPIRV-Tools) from source with the STATIC C / C++ runtime (/MT) into deps\glslang-mt, the
rem folder runtime\build_glslang.bat then links ssbu_glslang.dll from. Why (2026-11-12): the official glslang release
rem libraries are built /MD, so ssbu_glslang.dll needed the player's MSVCP140.dll; a PC with one older than 14.40 crashed at
rem the first shader (access violation in MSVCP140.dll, reading 0: the std::mutex of a newer STL on an older DLL). With the
rem static runtime the DLL needs no Visual C++ Redistributable at all. Only whoever makes a release runs this (once).
rem   scripts\build_glslang_static.bat [output folder] [source folder]
rem Needs: Visual Studio / Build Tools with the C++ x64 tools and its "C++ CMake tools", git, Python (deps\python is used).
setlocal
set VER=16.6.0
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0..\deps\glslang-mt"
set "SRC=%~2"
if "%SRC%"=="" set "SRC=%TEMP%\ssbu_glslang_src_%VER%"
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VSDIR=
if exist "%VSW%" "%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\ssbu_vsdir.txt"
if exist "%TEMP%\ssbu_vsdir.txt" set /p VSDIR=<"%TEMP%\ssbu_vsdir.txt"
if "%VSDIR%"=="" ( echo build_glslang_static: no Visual Studio / Build Tools with the C++ x64 tools found & exit /b 1 )
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
where cmake >nul 2>nul || ( echo build_glslang_static: cmake not found ^(add "C++ CMake tools for Windows" to the Build Tools^) & exit /b 1 )
where ninja >nul 2>nul || ( echo build_glslang_static: ninja not found ^(it comes with "C++ CMake tools for Windows"^) & exit /b 1 )
set "PY=%~dp0..\deps\python\python.exe"
if not exist "%PY%" set PY=python

if not exist "%SRC%\CMakeLists.txt" (
    git clone --depth 1 --branch %VER% https://github.com/KhronosGroup/glslang.git "%SRC%" || exit /b 1
)
if not exist "%SRC%\External\spirv-tools\CMakeLists.txt" (
    pushd "%SRC%"
    "%PY%" update_glslang_sources.py || ( popd & exit /b 1 )
    popd
)
cmake -S "%SRC%" -B "%SRC%\build-mt" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=ON -DGLSLANG_TESTS=OFF ^
    -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF ^
    "-DCMAKE_INSTALL_PREFIX=%OUT%" || exit /b 1
cmake --build "%SRC%\build-mt" || exit /b 1
cmake --install "%SRC%\build-mt" || exit /b 1
if not exist "%OUT%\lib\glslang.lib" ( echo build_glslang_static: %OUT%\lib\glslang.lib was not produced & exit /b 1 )
echo build_glslang_static: done: %OUT% ^(static runtime; runtime\build_glslang.bat picks it up^)
exit /b 0
