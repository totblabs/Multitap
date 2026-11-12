export const clipboardPolicyKeys = Object.freeze(['textToRemote', 'textToLocal', 'filesToRemote', 'filesToLocal']);

// An absent or invalid stored mask never grants clipboard access.
export function clipboardPolicyFromMask(mask) {
  const value = Number.isSafeInteger(mask) && mask >= 0 && mask <= 15 ? mask : 0;
  return Object.fromEntries(clipboardPolicyKeys.map((key, bit) => [key, !!(value & (1 << bit))]));
}
export function clipboardPolicyMask(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).length !== 4 ||
    !clipboardPolicyKeys.every(key => Object.hasOwn(value, key) && typeof value[key] === 'boolean')) throw Error('invalid_clipboard_policy');
  return clipboardPolicyKeys.reduce((mask, key, bit) => mask | (value[key] ? 1 << bit : 0), 0);
}
export const anyClipboardAllowed = policy => clipboardPolicyKeys.some(key => policy?.[key] === true);
export const clipboardPolicyRestricted = (before, after) => clipboardPolicyKeys.some(key => before?.[key] === true && after?.[key] !== true);
export function intersectClipboardPolicy(before, current) {
  return Object.fromEntries(clipboardPolicyKeys.map(key => [key, before?.[key] === true && current?.[key] === true]));
}
// Which administrator permission a transfer format needs: plain and rich text (HTML / RTF
// that accompany the plain text) need the text permission; images, files and
// folder trees are binary content and need the files permission.
export const clipboardFormatPermission = Object.freeze({ text: 'text', richtext: 'text', files: 'files', image: 'files', tree: 'files' });
export function clipboardTransferAllowed(policy, format, from) {
  if (from !== 'client' && from !== 'host') return false;
  const key = Object.hasOwn(clipboardFormatPermission, format) ? clipboardFormatPermission[format] : null;
  return key !== null && policy?.[key + (from === 'client' ? 'ToRemote' : 'ToLocal')] === true;
}
