@echo off
title GW-Drops — Server & Cloudflare Tunnel
cd /d "%~dp0"

echo ===================================================================
echo             GW-Drops Telemetry Network Launcher
echo ===================================================================
echo 1. Starting GW-Drops Ingestion Server on port 8787...
start "GW-Drops Server" /min node server/server.js

timeout /t 2 /nobreak >nul

echo 2. Launching Cloudflare Tunnel...
start "GW-Drops Cloudflare Tunnel" cloudflared tunnel --url http://localhost:8787

timeout /t 2 /nobreak >nul

echo 3. Opening Live Monitoring Dashboard in your browser...
start http://localhost:8787/

echo ===================================================================
echo [OK] Both the Collector Server and Cloudflare Tunnel are now running!
echo Monitoring Dashboard: http://localhost:8787/
echo Cloudflare Tunnel window is open in background.
echo ===================================================================
echo.
echo Press any key to stop all services...
pause >nul

echo Stopping services...
taskkill /F /IM cloudflared.exe >nul 2>&1
taskkill /F /FI "WINDOWTITLE eq GW-Drops Server*" >nul 2>&1
echo Done.
timeout /t 2 >nul
