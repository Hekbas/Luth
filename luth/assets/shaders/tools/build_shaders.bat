@echo off
:: Runs the PowerShell script in the current directory
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0compile_shaders.ps1"