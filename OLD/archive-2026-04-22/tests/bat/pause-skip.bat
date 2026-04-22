@echo off
rem pause is recognized but only reached when explicitly jumped to
goto l_end
pause
:l_end
echo pause-skip ok
