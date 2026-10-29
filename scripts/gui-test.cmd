@echo off
rem gui-test.cmd <test.py> <outdir> [timeout seconds]
rem
rem The Windows GUI test driver, scripts/gui-test.sh's counterpart: runs one
rem FreeCAD-driven test script (tests/gui) in a configuration of its own and
rem judges it by the result file the script writes -- DONE and no FAIL or
rem ABORT line passes, anything else fails (exit 1). The script gets GT_OUT,
rem GT_RESULT and GT_TIMEOUT, and runs through scripts/gui-test-guard.py,
rem which ends the session if the script fails to start or hangs.
rem
rem Where it runs, GT_DESKTOP:
rem   hidden    (default) a desktop of its own (scripts/run-on-hidden-desktop.ps1):
rem             real windows, so OpenGL, Direct3D and saveRenderDump work, and
rem             nothing shows on the user's screen. No system pointer there;
rem             QTest events still reach the widgets.
rem   screen    a window on the user's screen.
rem   offscreen QT_QPA_PLATFORM=offscreen. On Windows that platform has no
rem             OpenGL at all -- the 3D view never draws and every render dump
rem             times out -- so only for tests that need no pixels.
rem
rem Other settings, all optional:
rem   FC_BUILD    the build tree, build\win-relwithdebinfo-801 by default
rem   FC_EXE      the program, %FC_BUILD%\bin\Thundereal.exe by default
rem   FC_RUNNER   a .cmd called as `<runner> <program> <args>` that puts the
rem               run-time on PATH (the conda env, OCCT, Coin) and runs the
rem               rest; without it the caller's PATH must already have them
rem   GT_SEED_CFG a user.cfg the test starts from
rem   GT_ADDONS   a Mod directory of addons, linked as the test's own Mod
rem
rem The configuration and the app data live under <outdir>\home
rem (FREECAD_USER_HOME): never the user's own.
setlocal EnableExtensions
if "%~2"=="" goto :usage
for %%d in ("%~dp0..") do set "REPO=%%~fd"
set "SCRIPT=%~f1"
set "OUT=%~f2"
if not exist "%SCRIPT%" goto :no_script
set "GT_TIMEOUT=300"
if not "%~3"=="" set "GT_TIMEOUT=%~3"
if not defined GT_DESKTOP set "GT_DESKTOP=hidden"
if not defined FC_BUILD set "FC_BUILD=%REPO%\build\win-relwithdebinfo-801"
if not defined FC_EXE set "FC_EXE=%FC_BUILD%\bin\Thundereal.exe"
if not exist "%FC_EXE%" goto :no_exe

rem A directory of an earlier run is replaced; anything else in the way is not
rem deleted -- the marker says this script made it.
if not exist "%OUT%" goto :fresh
if not exist "%OUT%\gui-test.out" goto :not_ours
rmdir /s /q "%OUT%"
rem A run.log that survives is held open by a run that hung: the redirect
rem below would fail and the test would not run at all.
if exist "%OUT%" goto :held
:fresh
mkdir "%OUT%\home"
> "%OUT%\gui-test.out" echo %SCRIPT%
if defined GT_SEED_CFG copy /y "%GT_SEED_CFG%" "%OUT%\home\user.cfg" > nul
if defined GT_ADDONS if exist "%GT_ADDONS%" mklink /J "%OUT%\home\Mod" "%GT_ADDONS%" > nul

set "GT_OUT=%OUT%"
set "GT_RESULT=%OUT%\result.txt"
set "GT_SCRIPT=%SCRIPT%"
set "GT_PROFILE=%~dp0gui-test-profile.py"
set "FREECAD_USER_HOME=%OUT%\home"
set "FC_MCP_PORT=0"
set "QT_QPA_PLATFORM="
if /i "%GT_DESKTOP%"=="offscreen" set "QT_QPA_PLATFORM=offscreen"

set "RUNCALL="
if defined FC_RUNNER set RUNCALL=call "%FC_RUNNER%"
> "%OUT%\launch.cmd" echo @%RUNCALL% "%FC_EXE%" --user-cfg "%OUT%\home\user.cfg" --system-cfg "%OUT%\home\system.cfg" --log-file "%OUT%\FreeCAD.log" "%~dp0gui-test-guard.py" ^> "%OUT%\run.log" 2^>^&1
echo gui-test: %~nx1 on the %GT_DESKTOP% desktop, %GT_TIMEOUT% s, in %OUT%
if /i "%GT_DESKTOP%"=="hidden" goto :hidden
call "%OUT%\launch.cmd"
goto :ran
:hidden
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-on-hidden-desktop.ps1" "%OUT%\launch.cmd"
:ran
set "RC=%errorlevel%"
rem the redirect goes first: "EXIT 0>>" would redirect handle 0
>> "%OUT%\run.log" echo EXIT %RC%

if not exist "%GT_RESULT%" goto :no_result
type "%GT_RESULT%"
findstr /b /c:"FAIL" /c:"ABORT" "%GT_RESULT%" > nul && goto :failed
findstr /b /c:"DONE" "%GT_RESULT%" > nul || goto :no_done
if not "%RC%"=="0" echo gui-test: the process exited %RC% after DONE
echo gui-test: PASS
exit /b 0

:failed
echo gui-test: FAIL (exit %RC%), see %OUT%
exit /b 1
:no_done
echo gui-test: FAIL, no DONE (exit %RC%), see %OUT%\run.log
exit /b 1
:no_result
echo gui-test: FAIL, no result file (exit %RC%), see %OUT%\run.log
exit /b 1
:usage
echo usage: gui-test.cmd ^<test.py^> ^<outdir^> [timeout seconds]
exit /b 2
:no_script
echo gui-test: no script "%SCRIPT%"
exit /b 2
:no_exe
echo gui-test: no program "%FC_EXE%" (set FC_BUILD or FC_EXE)
exit /b 2
:not_ours
echo gui-test: "%OUT%" exists and is not a gui-test.cmd output directory; not touching it
exit /b 2
:held
echo gui-test: "%OUT%" is held by an earlier run, use another directory
exit /b 1
