@echo off
echo flow start
if exist \AUTOEXEC.BAT echo autoexec present
if not exist \NOPE.BAT echo nope missing
if "%A%"=="" goto l_set
:l_set
set A=hit
if "%A%"=="hit" goto l_call
echo skipped
:l_call
call \tests\bat\minimal.bat
if errorlevel 0 goto l_end
:l_end
echo flow end
goto :eof
echo never printed
