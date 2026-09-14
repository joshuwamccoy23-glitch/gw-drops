CREATE TABLE IF NOT EXISTS kill_events (
    event_id TEXT PRIMARY KEY,
    install_id TEXT NOT NULL,
    observed_at INTEGER NOT NULL,
    received_at INTEGER NOT NULL,
    map_id INTEGER NOT NULL,
    hard_mode INTEGER NOT NULL,
    party_size INTEGER NOT NULL,
    mob_model_id INTEGER NOT NULL,
    mob_name TEXT NOT NULL,
    confidence TEXT NOT NULL CHECK (confidence IN ('confirmed', 'ambiguous'))
);

CREATE TABLE IF NOT EXISTS event_drops (
    event_id TEXT NOT NULL REFERENCES kill_events(event_id) ON DELETE CASCADE,
    item_model_id INTEGER NOT NULL,
    item_name TEXT NOT NULL,
    item_type INTEGER NOT NULL,
    rarity INTEGER NOT NULL,
    PRIMARY KEY (event_id, item_model_id, item_name)
);

CREATE TABLE IF NOT EXISTS vendor_events (
    transaction_id TEXT PRIMARY KEY,
    install_id TEXT NOT NULL,
    observed_at INTEGER NOT NULL,
    received_at INTEGER NOT NULL,
    map_id INTEGER NOT NULL,
    vendor_type TEXT NOT NULL,
    transaction_type TEXT NOT NULL,
    item_model_id INTEGER NOT NULL,
    item_name TEXT NOT NULL,
    unit_price INTEGER NOT NULL,
    quantity INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS kill_events_install_received ON kill_events(install_id, received_at);
CREATE INDEX IF NOT EXISTS kill_events_mob ON kill_events(map_id, hard_mode, mob_model_id, received_at DESC);
CREATE INDEX IF NOT EXISTS event_drops_item ON event_drops(item_model_id, item_name);
CREATE INDEX IF NOT EXISTS vendor_events_item ON vendor_events(item_model_id, vendor_type, observed_at DESC);

CREATE TABLE IF NOT EXISTS auction_listings (
    listing_id TEXT PRIMARY KEY,
    seller_install_id TEXT NOT NULL,
    seller_name TEXT NOT NULL,
    listing_type TEXT NOT NULL CHECK (listing_type IN ('sell', 'buy')),
    item_name TEXT NOT NULL,
    item_model_id INTEGER NOT NULL,
    quantity INTEGER NOT NULL,
    unit_price INTEGER NOT NULL,
    notes TEXT NOT NULL,
    modifiers TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('active', 'cancelled'))
);

CREATE INDEX IF NOT EXISTS auction_listings_active ON auction_listings(status, expires_at, item_name);
