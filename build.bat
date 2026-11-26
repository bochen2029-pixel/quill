@echo off
rem quill build - MSVC (VS2022), C++20, static CRT, /W4 with warnings as errors, strict floating point.
rem Produces quill.exe beside this file. No third-party code and no other build step.
setlocal enabledelayedexpansion
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
  set "VCV=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  if not exist "!VCV!" (
    set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    for /f "usebackq tokens=*" %%i in (`""!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSDIR=%%i"
    set "VCV=!VSDIR!\VC\Auxiliary\Build\vcvars64.bat"
  )
  if not exist "!VCV!" ( echo build: vcvars64.bat not found & exit /b 1 )
  call "!VCV!" >nul 2>nul
  where cl >nul 2>nul
  if errorlevel 1 ( echo build: the MSVC compiler cl.exe is not available after vcvars64.bat & exit /b 1 )
)
if not exist obj mkdir obj
rem /fp:strict: nothing is fused or reordered, and the processor's inexact flag is reliable (prove reads it).
set CXXFLAGS=/nologo /c /MP /std:c++20 /O2 /W4 /WX /permissive- /EHsc /utf-8 /MT /fp:strict /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Foobj\
set SOURCES=base.cpp npz.cpp net.cpp spec.cpp explain.cpp prove.cpp verbs.cpp kit.cpp examples.cpp selftest.cpp mcp.cpp quill.cpp
cl %CXXFLAGS% %SOURCES% || exit /b 1
set OBJS=
for %%f in (%SOURCES%) do set OBJS=!OBJS! obj\%%~nf.obj
link /nologo /SUBSYSTEM:CONSOLE /OUT:quill.exe %OBJS% || exit /b 1
echo OK: quill.exe
