# GW Drop & Vendor Price Research

Crowdsourced drop-rate observations and vendor pricing for Guild Wars via the **DropExplorer** GWToolbox++ plugin.

## Architecture

1. **Player Client (`DropExplorer.dll`)**:
   - **Completely Self-Contained**: Does not require `TelemetryHarness.dll` or any third-party plugins.
   - **Smart 5–10 Minute Batching**: In-game observations (mob kills, confirmed drops, vendor price quotes, and completed buy/sell transactions) are held in memory. Every 5–10 minutes (user-configurable), if data was collected, a single batch POST is dispatched to your server. If nothing was collected, **zero** network requests are made.
   - **Periodic GitHub Sync**: Downloads sanitized crowdsourced datasets (`community-rates.json` and `vendor-prices.json`) directly from this repository's raw GitHub URLs to show live drop rates (with sample size and 95% Wilson confidence intervals) and vendor price averages in tooltips.

2. **Collector Server (`server/server.js`)**:
   - Runs locally or on a server (`start-server.bat`).
   - Built on Node.js with zero external dependencies using the native `node:sqlite` engine and WAL mode.
   - Ingests batched telemetry at `POST /v1/telemetry`.
   - Aggregates mob drop rates with Wilson 95% confidence intervals and vendor prices (min, max, average, sample counts).
   - Periodically exports and pushes updated `data/community-rates.json` and `data/vendor-prices.json` to GitHub.

## Quick Start (Running the Server)

1. Double-click `start-server.bat` (or run `npm start`).
2. The server will start listening on port `8787` (`http://localhost:8787`).
3. In GWToolbox++ under **Settings -> Drop Explorer**:
   - Set **Collector URL** to `http://localhost:8787/v1/telemetry` (or your public endpoint).
   - Check **Contribute anonymous drop & vendor observations**.
   - Set **Batch Interval** (default 5.0 minutes).

## Repository Layout

- `plugin/`: Source code for `DropExplorer` (`DropExplorerPlugin.h`, `DropExplorerPlugin.cpp`, `DropExplorerData.h`, `DropExplorerData.cpp`).
- `release/DropExplorer.dll`: Pre-compiled release DLL ready for GWToolbox++.
- `server/server.js`: Native Node.js + SQLite collector server.
- `start-server.bat`: Windows launcher for the collector server.
- `schema.sql`: SQLite database schema for kills, drops, and vendor transactions.
- `data/community-rates.json`: Aggregated drop rates downloaded by clients.
- `data/vendor-prices.json`: Aggregated vendor buy/sell prices downloaded by clients.

