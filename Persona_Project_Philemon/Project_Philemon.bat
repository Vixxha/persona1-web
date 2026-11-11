@echo off
cd /d "%~dp0"
if not exist Persona1Native.exe ( echo Persona1Native.exe is missing. See README.txt. & pause & exit /b 1 )
if not exist EBOOT_DECRYPTED.ELF ( echo EBOOT_DECRYPTED.ELF is missing. See README.txt. & pause & exit /b 1 )
if not exist game\PERSONA\PSP_GAME ( echo game\PERSONA\PSP_GAME is missing. Extract your own ISO there. See README.txt. & pause & exit /b 1 )
if not exist Persona1.iso ( echo Persona1.iso is missing. Put your own ISO here. See README.txt. & pause & exit /b 1 )
start "" Persona1Native.exe EBOOT_DECRYPTED.ELF game\PERSONA 0 Persona1.iso
