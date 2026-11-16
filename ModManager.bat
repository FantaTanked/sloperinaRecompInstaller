@echo off
rem sloperinaRecomp Mod Manager: double-click it. A window opens: pick your game's exe, install the mod loader once,
rem then install, update or remove mods (sloperinaGZ ...) with a click.
start "" powershell -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0ModManager.ps1" %*
