@echo off
title GW-Drops Collector Server
cd /d "%~dp0"
echo Starting GW-Drops Collector Server on port 8787...
set AUTO_GIT_PUSH=true
node server/server.js
pause
