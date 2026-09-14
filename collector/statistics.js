export function wilson(successes, trials, z = 1.959963984540054) {
  if (!Number.isInteger(successes) || !Number.isInteger(trials) || successes < 0 || trials <= 0 || successes > trials) {
    return null;
  }
  const p = successes / trials;
  const z2 = z * z;
  const denominator = 1 + z2 / trials;
  const center = (p + z2 / (2 * trials)) / denominator;
  const margin = z * Math.sqrt((p * (1 - p) + z2 / (4 * trials)) / trials) / denominator;
  return {
    rate_pct: p * 100,
    confidence_low_pct: Math.max(0, center - margin) * 100,
    confidence_high_pct: Math.min(1, center + margin) * 100
  };
}

export function confidenceLabel(trials) {
  if (trials < 100) return "insufficient";
  if (trials < 500) return "low";
  if (trials < 2000) return "moderate";
  return "high";
}
