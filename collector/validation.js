const MAX_BATCH = 1000;
const MAX_DROPS = 50;

const integer = (value, minimum, maximum) => Number.isInteger(value) && value >= minimum && value <= maximum;
const text = (value, maximum) => typeof value === "string" && value.length > 0 && value.length <= maximum;

export function validateBatch(body) {
  if (!body || (body.schema_version !== 1 && body.schema_version !== 2) || !text(body.install_id, 64) || !/^[a-f0-9-]{16,64}$/i.test(body.install_id)) {
    return "Invalid envelope";
  }

  const events = Array.isArray(body.events) ? body.events : [];
  const vendorEvents = Array.isArray(body.vendor_events) ? body.vendor_events : [];

  if (events.length === 0 && vendorEvents.length === 0) {
    return "No events in batch";
  }

  if (events.length > MAX_BATCH || vendorEvents.length > MAX_BATCH) {
    return "Batch exceeds maximum size";
  }

  for (const event of events) {
    if (!text(event.event_id, 64) || !/^[a-f0-9-]{16,64}$/i.test(event.event_id)) return "Invalid event ID";
    if (!integer(event.observed_at, 1, 4102444800)) return "Invalid timestamp";
    if (!integer(event.map_id, 1, 10000)) return "Invalid map";
    if (typeof event.hard_mode !== "boolean") return "Invalid mode";
    if (!integer(event.party_size, 1, 12)) return "Invalid party size";
    if (!integer(event.mob_model_id, 1, 1000000) || !text(event.mob_name, 128)) return "Invalid mob";
    if (!['confirmed', 'ambiguous'].includes(event.confidence)) return "Invalid confidence";
    if (!Array.isArray(event.drops) || event.drops.length > MAX_DROPS) return "Invalid drops";
    for (const drop of event.drops) {
      if (!integer(drop.item_model_id, 1, 10000000) || !text(drop.item_name, 160)) return "Invalid item";
      if (!integer(drop.item_type, 0, 255) || !integer(drop.rarity, 0, 255)) return "Invalid item metadata";
    }
  }

  for (const v of vendorEvents) {
    if (!text(v.transaction_id, 64) || !/^[a-f0-9-]{16,64}$/i.test(v.transaction_id)) return "Invalid transaction ID";
    if (!integer(v.observed_at, 1, 4102444800)) return "Invalid vendor timestamp";
    if (!integer(v.map_id, 1, 10000)) return "Invalid vendor map";
    if (!text(v.vendor_type, 64)) return "Invalid vendor type";
    if (!['sell', 'buy', 'quote', 'quote_sell', 'quote_buy'].includes(v.transaction_type.toLowerCase())) return "Invalid transaction type";
    if (!integer(v.item_model_id, 1, 10000000) || !text(v.item_name, 160)) return "Invalid vendor item";
    if (!integer(v.unit_price, 0, 100000000)) return "Invalid vendor unit price";
    if (!integer(v.quantity, 1, 10000)) return "Invalid vendor quantity";
  }

  return null;
}
