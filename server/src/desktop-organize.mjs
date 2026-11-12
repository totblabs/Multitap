// Proxmox tags and pool of a VM, used to categorize the client's desktop list.
// They are read from Proxmox only (never edited in Multitap).
export const maxTags = 32;
export const maxTagLength = 64;
export const maxPoolLength = 64;
const tagPattern = /^[a-z0-9_][a-z0-9_\-+.]*$/i;

// Proxmox stores tags as one string separated by ';' (older/other tools also
// use ',' or spaces). Accepts that string or an array of strings. Trims, drops
// empty and invalid entries, removes case-insensitive duplicates (the first
// spelling stays) and keeps at most maxTags. Anything else yields [].
export function normalizeTags(value) {
  const parts = typeof value === 'string' ? value.split(/[;,\s]+/) : Array.isArray(value) ? value : [];
  const seen = new Set(), tags = [];
  for (const part of parts) {
    if (typeof part !== 'string') continue;
    const tag = part.trim();
    if (!tag || tag.length > maxTagLength || !tagPattern.test(tag)) continue;
    const key = tag.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key); tags.push(tag);
    if (tags.length >= maxTags) break;
  }
  return tags;
}

// A pool id: a non-empty string without control characters, at most 64 chars.
export function normalizePool(value) {
  if (typeof value !== 'string') return null;
  const pool = value.trim();
  return pool && pool.length <= maxPoolLength && !/\p{Cc}/u.test(pool) ? pool : null;
}

// Stored form (a JSON array in desktops.pve_tags); tolerant of damaged text.
export function storedTags(text) {
  try { return normalizeTags(JSON.parse(text)); } catch { return []; }
}
