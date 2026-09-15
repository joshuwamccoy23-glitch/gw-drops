# GW Drop Explorer and Auction House

Crowdsourced drop observations, vendor pricing, map guidance, and player-to-player listings for Guild Wars via the **DropExplorer** GWToolbox++ plugin.

## Architecture

1. **Player Client (`DropExplorer.dll`)**:
   - **Completely Self-Contained**: Does not require `TelemetryHarness.dll` or any third-party plugins.
   - **Smart 5–10 Minute Batching**: In-game observations (mob kills, confirmed drops, vendor price quotes, and completed buy/sell transactions) are held in memory. Every 5–10 minutes (user-configurable), if data was collected, a single batch POST is dispatched to your server. If nothing was collected, **zero** network requests are made.
   - **Periodic GitHub Sync**: Downloads sanitized crowdsourced datasets (`community-rates.json` and `vendor-prices.json`) directly from this repository's raw GitHub URLs to show live drop rates (with sample size and 95% Wilson confidence intervals) and vendor price averages in tooltips.
   - **Auction House**: Publishes sell listings and buy orders to the collector. Trades are never executed by the plugin; the Contact button starts an in-game whisper to the listing player.
   - **Inventory Listing Flow**: Right-click an inventory item in the existing GWToolbox++ item menu to create a sell listing or buy order. The plugin prefills its name, model, quantity, and decoded weapon or armor modifier text, then opens the Auction House tab.
   - **Canonical Equipment Modifiers**: Inscriptions, insignias, and armor runes use fixed dropdowns with a `None` choice. Readable modifiers on inventory items are selected automatically.
   - **Item Search**: Search the built-in DropExplorer catalog or enter any Guild Wars item name manually. Buy orders can include required inscriptions, runes, insignias, and other modifiers.

2. **Collector Server (`server/server.js`)**:
   - Runs locally or on a server (`start-server.bat`).
   - Built on Node.js with zero external dependencies using the native `node:sqlite` engine and WAL mode.
   - Ingests batched telemetry at `POST /v1/telemetry`.
   - Serves listings at `GET/POST /v1/listings` and owner-authorized cancellation at `DELETE /v1/listings/:id`.
   - Aggregates mob drop rates with Wilson 95% confidence intervals and vendor prices (min, max, average, sample counts).
   - Periodically exports and pushes updated `data/community-rates.json` and `data/vendor-prices.json` to GitHub.
   - Does not expose private installation IDs through public listing or recent-observation endpoints. Character names are published only after explicit confirmation.

## Quick Start (Running the Server)

Use `start-network.bat` to run the local server with a Cloudflare Quick Tunnel and automatic recovery. The launcher restarts the tunnel if it exits, checks the local server every 15 seconds, and publishes the verified public address to `data/auction-droplistings.txt` in this repository. The host needs working Git push credentials. Publication uses a separate temporary checkout so unrelated local changes are not committed.

On this host, the `GW-Drops Auction Server` scheduled task starts the network launcher at Windows logon and restarts it if the launcher exits.

The updated DLL periodically reads that address through GitHub's uncached contents endpoint and fetches live listings directly from the server. It never reads listings from GitHub or local files. A failed auction request forces an immediate address lookup, so tunnel address changes require no DLL rebuild. During an outage, automatic reads retry; failed publishing/cancellation is reported and is not automatically replayed.

The instructions below describe the older manually configured client:

1. Double-click `start-server.bat` (or run `npm start`).
2. The server will start listening on port `8787` (`http://localhost:8787`).
3. In GWToolbox++ under **Settings -> Drop Explorer**:
   - Set **Collector server** to `http://localhost:8787` (or your stable Cloudflare hostname). A URL ending in `/v1/telemetry` is also accepted.
   - Check **Contribute anonymous drop & vendor observations**.
   - Set **Batch Interval** (default 5.0 minutes).

## Repository Layout

- `plugin/`: Source code for `DropExplorer` (`DropExplorerPlugin.h`, `DropExplorerPlugin.cpp`, `DropExplorerData.h`, `DropExplorerData.cpp`).
- `release/DropExplorer.dll`: Pre-compiled DropExplorer plugin DLL.
- `release/GWToolboxdll.dll`: Matching GWToolbox++ core with the native inventory context-menu callback exports.
- `integration/`: Source patch and integration notes for the required GWToolbox++ core hook.
- `server/server.js`: Native Node.js + SQLite collector server.
- `start-server.bat`: Windows launcher for the collector server.
- `schema.sql`: SQLite database schema for kills, drops, vendor transactions, and Auction House listings.
- `data/community-rates.json`: Aggregated drop rates downloaded by clients.
- `data/vendor-prices.json`: Aggregated vendor buy/sell prices downloaded by clients.
