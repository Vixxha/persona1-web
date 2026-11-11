@echo off
rem Debug launch: writes log.txt next to this file
cd /d "%~dp0"
set PERSONA1_REWARDSLOG=1
Persona1Native.exe EBOOT_DECRYPTED.ELF game\PERSONA 0 Persona1.iso 2> log.txt
