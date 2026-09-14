import { confidenceLabel, wilson } from "./statistics.js";
import { validateBatch } from "./validation.js";

const json = (value, status = 200) => new Response(JSON.stringify(value, null, 2), {
  status,
  headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" }
});

async function ingest(request, env) {
  let body;
  try {
    body = await request.json();
  } catch {
    return json({ error: "Invalid JSON" }, 400);
  }
  const validationError = validateBatch(body);
  if (validationError) return json({ error: validationError }, 400);

  const now = Math.floor(Date.now() / 1000);
  const recent = await env.DB.prepare(
    "SELECT COUNT(*) AS count FROM kill_events WHERE install_id = ? AND received_at >= ?"
  ).bind(body.install_id, now - 60).first();
  if ((recent?.count ?? 0) + body.events.length > 120) return json({ error: "Rate limit exceeded" }, 429);

  const statements = [];
  for (const event of body.events) {
    statements.push(env.DB.prepare(
      "INSERT OR IGNORE INTO kill_events (event_id, install_id, observed_at, received_at, map_id, hard_mode, party_size, mob_model_id, mob_name, confidence) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    ).bind(event.event_id, body.install_id, event.observed_at, now, event.map_id, event.hard_mode ? 1 : 0,
      event.party_size, event.mob_model_id, event.mob_name, event.confidence));
    for (const drop of event.drops) {
      statements.push(env.DB.prepare(
        "INSERT OR IGNORE INTO event_drops (event_id, item_model_id, item_name, item_type, rarity) VALUES (?, ?, ?, ?, ?)"
      ).bind(event.event_id, drop.item_model_id, drop.item_name, drop.item_type, drop.rarity));
    }
  }
  await env.DB.batch(statements);
  return json({ accepted: body.events.length }, 202);
}

async function rates(request, env) {
  const url = new URL(request.url);
  const windowSize = Math.min(10000, Math.max(100, Number.parseInt(url.searchParams.get("window") ?? "1000", 10) || 1000));
  const minimum = Math.min(windowSize, Math.max(1, Number.parseInt(url.searchParams.get("min_samples") ?? "100", 10) || 100));
  const result = await env.DB.prepare(`
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
  `).bind(windowSize, minimum).all();

  const records = result.results.map(row => ({
    ...row,
    hard_mode: Boolean(row.hard_mode),
    sample_window: windowSize,
    confidence: confidenceLabel(row.eligible_kills),
    ...wilson(row.confirmed_drops, row.eligible_kills)
  }));
  return new Response(JSON.stringify({ schema_version: 1, generated_at: new Date().toISOString(), records }, null, 2), {
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "public, max-age=300" }
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "POST" && url.pathname === "/v1/events") return ingest(request, env);
    if (request.method === "GET" && url.pathname === "/v1/rates") return rates(request, env);
    if (request.method === "GET" && url.pathname === "/health") return json({ status: "ok" });
    return json({ error: "Not found" }, 404);
  }
};
