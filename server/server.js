import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { DatabaseSync } from "node:sqlite";
import { randomUUID } from "node:crypto";
import { fileURLToPath } from "node:url";

import { wilson, confidenceLabel } from "../collector/statistics.js";
import { validateBatch } from "../collector/validation.js";

const execFileAsync = promisify(execFile);
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const ROOT_DIR = path.resolve(__dirname, "..");
const DB_PATH = process.env.DB_PATH || path.join(ROOT_DIR, "data", "drops.db");
const DATA_DIR = path.dirname(DB_PATH);
const DROP_LOG_PATH = process.env.DROP_LOG_PATH || "";

if (!fs.existsSync(DATA_DIR)) {
  fs.mkdirSync(DATA_DIR, { recursive: true });
}

// Initialize SQLite database
const db = new DatabaseSync(DB_PATH);
db.exec("PRAGMA journal_mode = WAL;");
db.exec("PRAGMA foreign_keys = ON;");

// Initialize schema
const schemaPath = path.join(ROOT_DIR, "schema.sql");
if (fs.existsSync(schemaPath)) {
  const schemaSql = fs.readFileSync(schemaPath, "utf8");
  db.exec(schemaSql);
}
const auctionColumns = new Set(db.prepare("PRAGMA table_info(auction_listings)").all().map(column => column.name));
if (!auctionColumns.has("currency_type")) db.exec("ALTER TABLE auction_listings ADD COLUMN currency_type TEXT NOT NULL DEFAULT 'gold'");
if (!auctionColumns.has("currency_item")) db.exec("ALTER TABLE auction_listings ADD COLUMN currency_item TEXT NOT NULL DEFAULT ''");

const PORT = parseInt(process.env.PORT || "8787", 10);
const HOST = process.env.HOST || "0.0.0.0";
const AUTO_PUSH = process.env.AUTO_GIT_PUSH === "true";
const PUBLISH_INTERVAL_MINUTES = parseInt(process.env.PUBLISH_INTERVAL_MINUTES || "60", 10);
const MAX_REQUEST_BYTES = 1024 * 1024;
const MAX_KILLS_PER_MINUTE = 500;

console.log(`[GW-Drops Server] Initialized database at ${DB_PATH}`);

function jsonResponse(res, data, statusCode = 200) {
  const body = JSON.stringify(data, null, 2);
  res.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(body),
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type"
  });
  res.end(body);
}

function parseJsonBody(req) {
  return new Promise((resolve, reject) => {
    let raw = "";
    req.on("data", chunk => {
      raw += chunk;
      if (raw.length > MAX_REQUEST_BYTES) {
        req.destroy();
        reject(new Error("Payload too large"));
      }
    });
    req.on("end", () => {
      try {
        resolve(raw ? JSON.parse(raw) : {});
      } catch (err) {
        reject(err);
      }
    });
    req.on("error", reject);
  });
}

// Ingestion Handler
async function handleIngest(req, res) {
  let body;
  try {
    body = await parseJsonBody(req);
  } catch (err) {
    return jsonResponse(res, { error: "Invalid JSON: " + err.message }, 400);
  }

  const err = validateBatch(body);
  if (err) {
    return jsonResponse(res, { error: err }, 400);
  }

  const now = Math.floor(Date.now() / 1000);
  const events = Array.isArray(body.events) ? body.events : [];
  const vendorEvents = Array.isArray(body.vendor_events) ? body.vendor_events : [];
  const recentCount = db.prepare(
    "SELECT COUNT(*) AS count FROM kill_events WHERE install_id = ? AND received_at >= ?"
  ).get(body.install_id, now - 60)?.count || 0;
  if (recentCount + events.length > MAX_KILLS_PER_MINUTE) {
    return jsonResponse(res, { error: "Rate limit exceeded" }, 429);
  }

  const insertKillStmt = db.prepare(
    "INSERT OR IGNORE INTO kill_events (event_id, install_id, observed_at, received_at, map_id, hard_mode, party_size, mob_model_id, mob_name, confidence) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
  );
  const insertDropStmt = db.prepare(
    "INSERT OR IGNORE INTO event_drops (event_id, item_model_id, item_name, item_type, rarity) VALUES (?, ?, ?, ?, ?)"
  );
  const insertVendorStmt = db.prepare(
    "INSERT OR IGNORE INTO vendor_events (transaction_id, install_id, observed_at, received_at, map_id, vendor_type, transaction_type, item_model_id, item_name, unit_price, quantity) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
  );

  let killsAccepted = 0;
  let dropsAccepted = 0;
  let vendorAccepted = 0;
  const acceptedKills = [];

  for (const ev of events) {
    const insertedKill = insertKillStmt.run(
      ev.event_id,
      body.install_id,
      ev.observed_at,
      now,
      ev.map_id,
      ev.hard_mode ? 1 : 0,
      ev.party_size,
      ev.mob_model_id,
      ev.mob_name,
      ev.confidence
    );
    killsAccepted += insertedKill.changes;
    if (insertedKill.changes) acceptedKills.push(ev);

    for (const drop of (ev.drops || [])) {
      const insertedDrop = insertDropStmt.run(
        ev.event_id,
        drop.item_model_id,
        drop.item_name,
        drop.item_type || 0,
        drop.rarity || 0
      );
      dropsAccepted += insertedDrop.changes;
    }
  }

  if (DROP_LOG_PATH && acceptedKills.length) {
    fs.mkdirSync(path.dirname(DROP_LOG_PATH), { recursive: true });
    if (!fs.existsSync(DROP_LOG_PATH)) {
      fs.writeFileSync(DROP_LOG_PATH, "# Drop Log\n\nMob kills accepted by the GW-Drops server.\n\n");
    }
    const lines = acceptedKills.map(ev => {
      const drops = Array.isArray(ev.drops) ? ev.drops : [];
      const dropNames = drops.length ? drops.map(drop => `${drop.item_name} (${drop.item_model_id})`).join(", ") : "none";
      return `- ${new Date(ev.observed_at * 1000).toISOString()} | map ${ev.map_id} | ${ev.hard_mode ? "HM" : "NM"} | mob: ${ev.mob_name} (${ev.mob_model_id}) | listed item dropped: ${drops.length ? "yes" : "no"} | detected drops: ${dropNames} | confidence: ${ev.confidence}`;
    });
    fs.appendFileSync(DROP_LOG_PATH, `${lines.join("\n")}\n`);
  }

  for (const v of vendorEvents) {
    const insertedVendor = insertVendorStmt.run(
      v.transaction_id,
      body.install_id,
      v.observed_at,
      now,
      v.map_id,
      v.vendor_type,
      v.transaction_type,
      v.item_model_id,
      v.item_name,
      v.unit_price,
      v.quantity
    );
    vendorAccepted += insertedVendor.changes;
  }

  console.log(`[GW-Drops Ingest] Accepted from ${body.install_id}: ${killsAccepted} kills, ${dropsAccepted} drops, ${vendorAccepted} vendor records.`);

  return jsonResponse(res, {
    status: "ok",
    accepted: {
      kills: killsAccepted,
      drops: dropsAccepted,
      vendor: vendorAccepted
    }
  }, 202);
}

// Generate Drop Rates Document
export function computeDropRates(windowSize = 1000, minimum = 10) {
  const query = `
    WITH ranked AS (
      SELECT event_id, map_id, hard_mode, mob_model_id, mob_name,
             ROW_NUMBER() OVER (PARTITION BY map_id, hard_mode, mob_model_id ORDER BY received_at DESC, event_id DESC) AS rank
      FROM kill_events WHERE confidence = 'confirmed'
    ), observed_items AS (
      SELECT DISTINCT e.map_id, e.hard_mode, e.mob_model_id, d.item_model_id, d.item_name
      FROM kill_events e JOIN event_drops d ON d.event_id = e.event_id
      WHERE e.confidence = 'confirmed'
    )
    SELECT oi.map_id, oi.hard_mode, oi.mob_model_id, MAX(r.mob_name) AS mob_name,
           oi.item_model_id, oi.item_name, COUNT(r.event_id) AS eligible_kills,
           SUM(CASE WHEN EXISTS (
             SELECT 1 FROM event_drops d WHERE d.event_id = r.event_id
               AND d.item_model_id = oi.item_model_id AND d.item_name = oi.item_name
           ) THEN 1 ELSE 0 END) AS confirmed_drops
    FROM observed_items oi JOIN ranked r
      ON r.map_id = oi.map_id AND r.hard_mode = oi.hard_mode AND r.mob_model_id = oi.mob_model_id
    WHERE r.rank <= ?
    GROUP BY oi.map_id, oi.hard_mode, oi.mob_model_id, oi.item_model_id, oi.item_name
    HAVING COUNT(r.event_id) >= ?
    ORDER BY oi.item_name, mob_name, oi.map_id, oi.hard_mode
  `;

  const rows = db.prepare(query).all(windowSize, minimum);
  const records = rows.map(row => ({
    ...row,
    hard_mode: Boolean(row.hard_mode),
    sample_window: windowSize,
    confidence: confidenceLabel(row.eligible_kills),
    ...wilson(row.confirmed_drops, row.eligible_kills)
  }));

  return {
    schema_version: 1,
    generated_at: new Date().toISOString(),
    record_count: records.length,
    records
  };
}

// Generate Vendor Prices Document
export function computeVendorPrices() {
  const query = `
    SELECT item_model_id, item_name, vendor_type, transaction_type,
           COUNT(*) AS sample_count,
           ROUND(AVG(unit_price), 1) AS avg_price,
           MIN(unit_price) AS min_price,
           MAX(unit_price) AS max_price,
           MAX(observed_at) AS last_observed_at
    FROM vendor_events
    GROUP BY item_model_id, item_name, vendor_type, transaction_type
    ORDER BY item_name, vendor_type
  `;

  const rows = db.prepare(query).all();
  return {
    schema_version: 1,
    generated_at: new Date().toISOString(),
    record_count: rows.length,
    prices: rows
  };
}

// Export Aggregated JSON Files and Push to GitHub
export async function exportAndPublishToGitHub() {
  console.log("[GW-Drops] Starting aggregation and export...");
  const ratesDoc = computeDropRates(1000, 100);
  const ratesFile = path.join(DATA_DIR, "community-rates.json");
  fs.writeFileSync(ratesFile, JSON.stringify(ratesDoc, null, 2), "utf8");

  const vendorDoc = computeVendorPrices();
  const vendorFile = path.join(DATA_DIR, "vendor-prices.json");
  fs.writeFileSync(vendorFile, JSON.stringify(vendorDoc, null, 2), "utf8");

  console.log(`[GW-Drops] Wrote ${ratesDoc.record_count} drop rate records and ${vendorDoc.record_count} vendor price records.`);

  if (!AUTO_PUSH) {
    console.log("[GW-Drops] AUTO_GIT_PUSH is disabled. Skipping git push.");
    return { success: true, pushed: false };
  }

  try {
    console.log("[GW-Drops] Staging files for git commit...");
    await execFileAsync("git", ["add", "data/community-rates.json", "data/vendor-prices.json"], { cwd: ROOT_DIR });
    
    // Check if there are changes
    const status = await execFileAsync("git", ["status", "--porcelain"], { cwd: ROOT_DIR });
    if (!status.stdout.trim()) {
      console.log("[GW-Drops] No changes to commit in data files.");
      return { success: true, pushed: false, message: "No data changes" };
    }

    const commitMsg = `chore(data): update community drop rates and vendor prices [${new Date().toISOString()}]`;
    await execFileAsync("git", ["commit", "-m", commitMsg], { cwd: ROOT_DIR });
    console.log("[GW-Drops] Committed changes:", commitMsg);

    console.log("[GW-Drops] Pushing to GitHub (origin main)...");
    const pushResult = await execFileAsync("git", ["push", "origin", "main"], { cwd: ROOT_DIR });
    console.log("[GW-Drops] Git push successful:", pushResult.stdout || "OK");
    return { success: true, pushed: true };
  } catch (gitErr) {
    console.warn("[GW-Drops] Git push failed (may need credentials or already up to date):", gitErr.message);
    return { success: false, error: gitErr.message };
  }
}

function activeListings(res, parsedUrl) {
  const now = Math.floor(Date.now() / 1000);
  const search = (parsedUrl.searchParams.get("search") || "").trim().slice(0, 80);
  const listingType = parsedUrl.searchParams.get("type");
  const typeFilter = listingType === "buy" || listingType === "sell" ? listingType : null;
  const limit = Math.min(200, Math.max(1, parseInt(parsedUrl.searchParams.get("limit") || "100", 10)));
  const rows = db.prepare(`
    SELECT listing_id, seller_name, listing_type, item_name, item_model_id, quantity, unit_price, currency_type, currency_item, notes, modifiers, created_at, expires_at
    FROM auction_listings
    WHERE status = 'active' AND expires_at > ? AND (? = '' OR item_name LIKE ?)
      AND (? IS NULL OR listing_type = ?)
    ORDER BY created_at DESC LIMIT ?
  `).all(now, search, `%${search}%`, typeFilter, typeFilter, limit);
  return jsonResponse(res, { schema_version: 2, generated_at: new Date().toISOString(), listings: rows });
}

async function createListing(req, res) {
  let body;
  try {
    body = await parseJsonBody(req);
  } catch {
    return jsonResponse(res, { error: "Invalid JSON" }, 400);
  }
  const validId = typeof body.install_id === "string" && /^[a-f0-9-]{16,64}$/i.test(body.install_id);
  const validName = typeof body.seller_name === "string" && body.seller_name.trim().length >= 3 && body.seller_name.trim().length <= 20;
  const validItem = typeof body.item_name === "string" && body.item_name.trim().length >= 1 && body.item_name.trim().length <= 160;
  const validType = body.listing_type === "sell" || body.listing_type === "buy";
  const validQuantity = Number.isInteger(body.quantity) && body.quantity >= 1 && body.quantity <= 250;
  const validPrice = Number.isInteger(body.unit_price) && body.unit_price >= 0 && body.unit_price <= 100000000;
  const currencyType = typeof body.currency_type === "string" ? body.currency_type : "gold";
  const validCurrency = ["gold", "platinum", "armbrace", "ectoplasm", "other"].includes(currencyType);
  const currencyItem = typeof body.currency_item === "string" ? body.currency_item.trim().slice(0, 160) : "";
  const validCurrencyItem = currencyType !== "other" || currencyItem.length > 0;
  const notes = typeof body.notes === "string" ? body.notes.trim().slice(0, 240) : "";
  const modifiers = typeof body.modifiers === "string" ? body.modifiers.trim().slice(0, 1000) : "";
  const durationHours = Number.isInteger(body.duration_hours) ? Math.min(168, Math.max(1, body.duration_hours)) : 24;
  if (!validId || !validName || !validItem || !validType || !validQuantity || !validPrice || !validCurrency || !validCurrencyItem) {
    return jsonResponse(res, { error: "Invalid listing" }, 400);
  }
  const now = Math.floor(Date.now() / 1000);
  const recent = db.prepare(
    "SELECT COUNT(*) AS count FROM auction_listings WHERE seller_install_id = ? AND created_at >= ?"
  ).get(body.install_id, now - 3600)?.count || 0;
  if (recent >= 20) return jsonResponse(res, { error: "Listing limit exceeded" }, 429);
  const listingId = randomUUID();
  db.prepare(`
    INSERT INTO auction_listings
      (listing_id, seller_install_id, seller_name, listing_type, item_name, item_model_id, quantity, unit_price, currency_type, currency_item, notes, modifiers, created_at, expires_at, status)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'active')
  `).run(listingId, body.install_id, body.seller_name.trim(), body.listing_type, body.item_name.trim(),
    Number.isInteger(body.item_model_id) ? body.item_model_id : 0, body.quantity, body.unit_price, currencyType, currencyItem, notes, modifiers,
    now, now + durationHours * 3600);
  return jsonResponse(res, { status: "ok", listing_id: listingId }, 201);
}

async function cancelListing(req, res, listingId) {
  let body;
  try {
    body = await parseJsonBody(req);
  } catch {
    return jsonResponse(res, { error: "Invalid JSON" }, 400);
  }
  if (typeof body.install_id !== "string" || !/^[a-f0-9-]{16,64}$/i.test(body.install_id)) {
    return jsonResponse(res, { error: "Invalid owner" }, 400);
  }
  const result = db.prepare(
    "UPDATE auction_listings SET status = 'cancelled' WHERE listing_id = ? AND seller_install_id = ? AND status = 'active'"
  ).run(listingId, body.install_id);
  return result.changes ? jsonResponse(res, { status: "ok" }) : jsonResponse(res, { error: "Listing not found" }, 404);
}

// Create HTTP Server
const server = http.createServer(async (req, res) => {
  const parsedUrl = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const pathname = parsedUrl.pathname;

  if (req.method === "OPTIONS") {
    res.writeHead(204, {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type"
    });
    return res.end();
  }

  if (req.method === "POST" && pathname === "/v1/mobdroptelemetry") {
    return handleIngest(req, res);
  }

  if (req.method === "GET" && (pathname === "/" || pathname === "/dashboard" || pathname === "/index.html")) {
    const indexPath = path.join(__dirname, "public", "index.html");
    if (fs.existsSync(indexPath)) {
      const html = fs.readFileSync(indexPath, "utf8");
      res.writeHead(200, {
        "Content-Type": "text/html; charset=utf-8",
        "Content-Length": Buffer.byteLength(html)
      });
      return res.end(html);
    }
  }

  if (req.method === "GET" && pathname === "/v1/stats") {
    const kills = db.prepare("SELECT COUNT(*) as count FROM kill_events").get()?.count || 0;
    const drops = db.prepare("SELECT COUNT(*) as count FROM event_drops").get()?.count || 0;
    const vendors = db.prepare("SELECT COUNT(*) as count FROM vendor_events").get()?.count || 0;
    const installs = db.prepare("SELECT COUNT(DISTINCT install_id) as count FROM kill_events").get()?.count || 0;
    return jsonResponse(res, {
      total_kills: kills,
      total_drops: drops,
      total_vendor_records: vendors,
      distinct_installs: installs,
      timestamp: new Date().toISOString()
    });
  }

  if (req.method === "GET" && pathname === "/v1/recent") {
    const limit = Math.min(100, parseInt(parsedUrl.searchParams.get("limit") || "25", 10));
    const recentKills = db.prepare(
      "SELECT event_id, observed_at, map_id, hard_mode, mob_model_id, mob_name, confidence FROM kill_events ORDER BY observed_at DESC, event_id DESC LIMIT ?"
    ).all(limit);

    const getDrops = db.prepare("SELECT item_model_id, item_name, item_type, rarity FROM event_drops WHERE event_id = ?");
    for (const k of recentKills) {
      k.hard_mode = Boolean(k.hard_mode);
      k.drops = getDrops.all(k.event_id);
    }

    return jsonResponse(res, { recent_kills: recentKills });
  }

  if (req.method === "GET" && pathname === "/v1/rates") {
    const windowParam = parseInt(parsedUrl.searchParams.get("window") || "1000", 10);
    const minParam = Math.max(100, parseInt(parsedUrl.searchParams.get("min_samples") || "100", 10));
    return jsonResponse(res, computeDropRates(Math.min(10000, Math.max(100, windowParam)), minParam));
  }

  if (req.method === "GET" && pathname === "/v1/prices") {
    return jsonResponse(res, computeVendorPrices());
  }

  if (req.method === "GET" && pathname === "/v1/listings") {
    return activeListings(res, parsedUrl);
  }

  if (req.method === "POST" && pathname === "/v1/listings") {
    return createListing(req, res);
  }

  const listingMatch = pathname.match(/^\/v1\/listings\/([a-f0-9-]{36})$/i);
  if (req.method === "DELETE" && listingMatch) {
    return cancelListing(req, res, listingMatch[1]);
  }

  if (req.method === "GET" && pathname === "/health") {
    return jsonResponse(res, { status: "ok", timestamp: new Date().toISOString() });
  }

  return jsonResponse(res, { error: "Not found" }, 404);
});

server.listen(PORT, HOST, () => {
  console.log(`[GW-Drops Server] Running at http://${HOST}:${PORT}`);
  console.log(`[GW-Drops Server] Mob/drop telemetry ingest endpoint: POST http://${HOST}:${PORT}/v1/mobdroptelemetry`);
  console.log(`[GW-Drops Server] Auto-publish to GitHub scheduled every ${PUBLISH_INTERVAL_MINUTES} minutes.`);

  // Initial export on boot
  exportAndPublishToGitHub().catch(err => console.warn("[GW-Drops] Initial publish failed:", err.message));

  // Schedule periodic publish
  setInterval(() => {
    exportAndPublishToGitHub().catch(err => console.warn("[GW-Drops] Periodic publish failed:", err.message));
  }, PUBLISH_INTERVAL_MINUTES * 60 * 1000);
});
