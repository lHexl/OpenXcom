@echo off
setlocal

set "ROOT=%~dp0.."
set "VCVARS=D:\vscpp\VC\Auxiliary\Build\vcvarsall.bat"
set "CMAKE=D:\vscpp\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD_DIR=%ROOT%\build-msvc-ninja"
set "BUILD_LOG=%BUILD_DIR%\build.log"

call "%VCVARS%" x64
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" --build "%BUILD_DIR%" --config Release > "%BUILD_LOG%" 2>&1
if errorlevel 1 (
	echo Build failed. See "%BUILD_LOG%".
	exit /b %errorlevel%
)

echo Build finished. Log: "%BUILD_LOG%".
