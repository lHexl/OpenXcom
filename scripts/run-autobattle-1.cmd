@echo off
setlocal

set "ROOT=%~dp0.."
set "OPENXCOM=%ROOT%\build-msvc-ninja\bin\openxcom.exe"
set "OPENXCOM_USER=C:\Users\user\Documents\OpenXcom"

"%OPENXCOM%" -user "%OPENXCOM_USER%" -config "%OPENXCOM_USER%" -master xcom1 -load 1.sav -playIntro false -autoBattle true -autoBattleLog true
exit /b %errorlevel%
