@echo off
setlocal
title STM32H755ZI DSP + WPT Web Dashboard (UDP :5002 to HTTP :8080)

cd /d "%~dp0"

where node >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Node.js was not found in PATH. Please install Node.js v18+ and try again.
    pause
    exit /b 1
)

echo ============================================================================
echo   STM32H755ZI Real-Time 4096-pt FFT + 16-Band db4 WPT Web Dashboard
echo   Target Board : 192.168.1.150:5002
echo   Dashboard URL: http://localhost:8080
echo ============================================================================
echo.

start "" "http://localhost:8080"
node "%~dp0server.js" --ip 192.168.1.150 --port 5002 --http 8080

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Dashboard server exited with code %ERRORLEVEL%.
    pause
)
