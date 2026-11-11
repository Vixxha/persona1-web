@echo off
rem Run after a rebuild to copy the new exe into this folder
copy /y "%~dp0..\out\persona1\profiles\persona1\Release\Persona1Native.exe" "%~dp0Persona1Native.exe"
pause
