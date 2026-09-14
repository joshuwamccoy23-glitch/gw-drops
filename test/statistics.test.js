import test from "node:test";
import assert from "node:assert/strict";
import { confidenceLabel, wilson } from "../collector/statistics.js";
import { validateBatch } from "../collector/validation.js";

test("Wilson interval contains the observed estimate", () => {
  const result = wilson(7, 2418);
  assert.ok(result.confidence_low_pct < result.rate_pct);
  assert.ok(result.confidence_high_pct > result.rate_pct);
  assert.ok(Math.abs(result.rate_pct - 0.289495) < 0.00001);
});

test("confidence labels enforce the publication floor", () => {
  assert.equal(confidenceLabel(99), "insufficient");
  assert.equal(confidenceLabel(100), "low");
  assert.equal(confidenceLabel(500), "moderate");
  assert.equal(confidenceLabel(2000), "high");
});

test("validates a minimal confirmed observation", () => {
  assert.equal(validateBatch({
    schema_version: 1,
    install_id: "01234567-89ab-cdef-0123-456789abcdef",
    events: [{
      event_id: "fedcba98-7654-3210-fedc-ba9876543210",
      observed_at: 1789412400,
      map_id: 72,
      hard_mode: true,
      party_size: 8,
      mob_model_id: 1234,
      mob_name: "Obsidian Behemoth",
      confidence: "confirmed",
      drops: []
    }]
  }), null);
});
