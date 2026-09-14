@echo off
title GW-Drops Collector Server
cd /d "%~dp0"
echo Starting GW-Drops Collector Server on port 8787...
node server/server.js
pause
