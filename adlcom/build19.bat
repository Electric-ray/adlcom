@echo off
setlocal

:: ============================================================
::  adlcom / jadlcom build script (Open Watcom 1.9)
:: ============================================================

set VERSION_MAJOR=1
set VERSION_MINOR=01

:: Watcom 1.9 ?? ??
set WATCOM=C:\watcom19
set PATH=%WATCOM%\binnt;%WATCOM%\binw;%PATH%
set INCLUDE=%WATCOM%\h;%WATCOM%\h\nt
set LIB=%WATCOM%\lib286;%WATCOM%\lib286\dos

set DEFS=-dVERSION_MAJOR=%VERSION_MAJOR% -dVERSION_MINOR=%VERSION_MINOR%

set CC=wcc -bt=dos -zq -oxhs
set CC32=wcc386 -mf -zl -zls -zq -oxhs
set AS=wasm -zq

echo [1/8] Compiling adlcom.c  (16-bit DOS)
%CC% %DEFS% adlcom.c
if errorlevel 1 goto :error

echo [2/8] Compiling cmdline.c  (16-bit DOS)
%CC% %DEFS% cmdline.c
if errorlevel 1 goto :error

echo [3/8] Compiling res_opl2.c  (16-bit DOS)
%CC% %DEFS% res_opl2.c
if errorlevel 1 goto :error

echo [4/8] Assembling res_glue.s
%AS% %DEFS% res_glue.s
if errorlevel 1 goto :error

echo [5/8] Assembling res_end.s
%AS% %DEFS% res_end.s
if errorlevel 1 goto :error

echo [6/8] Linking adlcom
wlink @adlcom.wl
if errorlevel 2 goto :error

echo [7/8] Compiling jadlcom  (32-bit NT DLL)
%CC32% %DEFS% jadlcom.c
if errorlevel 1 goto :error

%CC32% %DEFS% -fo=jlm_cmdl.obj cmdline.c
if errorlevel 1 goto :error

%CC32% %DEFS% -fo=jlm_opl2.obj res_opl2.c
if errorlevel 1 goto :error

echo [8/8] Linking jadlcom.dll
wlink @jadlcom.wl
if errorlevel 1 goto :error

echo [post] Patching PE (patchpe.py)
python patchpe.py jadlcom.dll
if errorlevel 1 goto :error

echo.
echo *** Build succeeded! (OW 1.9) ***
goto :end

:error
echo.
echo *** Build FAILED (errorlevel %errorlevel%) ***
exit /b 1

:end
endlocal
