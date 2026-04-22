@ECHO OFF

IF EXIST \GEMAPPS\GEMSYS\GEMVDI.EXE GOTO OPENGEM

ECHO FYI: OpenGEM needs to be "installed" at the root directory of
ECHO whatever drive it runs from. Before you run OpenGEM, you should
ECHO run this SWSUBST command to set another drive letter to the
ECHO OpenGEM directory:
ECHO.
ECHO SWSUBST O: C:\OPENGEM
ECHO.
ECHO ..and then run GEM.BAT from the O: drive.

GOTO END

:OPENGEM

CD \GEMAPPS\GEMSYS
GEMVDI %1 %2 %3

:END

