// Proxmox tags and pool of a VM, used to categorize the client's desktop list.
// They are read from Proxmox only (never edited in Multitap). Below them, the
// desktop groups each account makes for itself.
import { randomBytes } from 'node:crypto';
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

// 데스크톱 그룹: each account's own named sections of its desktop list. They
// are kept on the server (store.mjs desktop_groups, a desktop's group on its
// assignment row), so every PC and phone of the account shows the same.
export const maxGroups = 30;
export const maxGroupNameLength = 40;
export const desktopGroupErrors = Object.freeze(['invalid_groups', 'invalid_group', 'invalid_group_name', 'duplicate_group_name', 'too_many_groups', 'unknown_group']);
// Ids are made here, never taken from a client.
export const newGroupId = () => 'g_' + randomBytes(6).toString('hex');

// A group name: trimmed, 1-40 characters, no control characters (line breaks
// and broken UTF-16 included). Throws invalid_group_name.
export function groupName(value) {
  if (typeof value !== 'string' || !value.isWellFormed()) throw new Error('invalid_group_name');
  const name = value.normalize('NFC').trim(), length = [...name].length;
  if (length < 1 || length > maxGroupNameLength || /[\p{Cc}\u2028\u2029]/u.test(name)) throw new Error('invalid_group_name');
  return name;
}
// Names are unique per account, ignoring case.
export const groupNameKey = name => name.toLowerCase().normalize('NFC');

// The whole list a client sends to replace its groups: [{id?, name}] in the
// order it wants. An id may be left out (or null) for a new group. Checks the
// shape, the count, every name and their uniqueness; whether each id is the
// account's is the store's part (unknown_group).
export function groupEntries(value) {
  if (!Array.isArray(value) || value.some(entry => !entry || typeof entry !== 'object' || Array.isArray(entry) ||
      Object.keys(entry).some(key => key !== 'id' && key !== 'name') || (entry.id != null && typeof entry.id !== 'string'))) throw new Error('invalid_groups');
  if (value.length > maxGroups) throw new Error('too_many_groups');
  const entries = value.map(entry => ({ id: entry.id ?? null, name: groupName(entry.name) }));
  if (new Set(entries.map(entry => groupNameKey(entry.name))).size !== entries.length) throw new Error('duplicate_group_name');
  const ids = entries.map(entry => entry.id).filter(id => id !== null);
  if (new Set(ids).size !== ids.length) throw new Error('invalid_groups');
  return entries;
}
