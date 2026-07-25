@echo off
setlocal enabledelayedexpansion

echo ==========================================
echo   Dice!Next 3.0.0 — 后端编译脚本
echo ==========================================
echo.

:: ─── 设置环境 ───
set "PATH=C:\Program Files\CMake\bin;%PATH%"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;%PATH%"

:: Run from the folder this script lives in (portable across machines/drives)
cd /d "%~dp0"

:: ─── CMake 配置 ───
echo [1/2] CMake 配置中...
cmake -B build -S . ^
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    echo [FAIL] CMake 配置失败，检查上方错误信息
    exit /b 1
)
echo [OK] CMake 配置完成
echo.

:: ─── 编译 (MSBuild 直接输出每个文件的进度) ───
echo [2/2] 编译中 (共 16 个源文件)...
echo ------------------------------------------
msbuild build\dice-next-server.sln ^
  /p:Configuration=Release ^
  /p:Platform=x64 ^
  /m ^
  /v:minimal
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] 编译失败，滚动上方查看具体错误
    exit /b 1
)
echo ------------------------------------------
echo.
echo ==========================================
echo   编译成功！
echo   输出: build\Release\dice-next-server.exe
echo.
echo   启动命令:
echo   build\Release\dice-next-server.exe
echo ==========================================
