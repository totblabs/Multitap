// Per-assignment (user x VM) device permissions, stored as one bit mask in
// assignments.rdp_redirections (the column keeps the name it was created
// with, so existing databases stay as they are). `drives`, `printers` and
// `usbDevices` each open their channel; `smartCards` and `serialPorts` are the
// USB classes that need a permission of their own. The bits 16 and 32 belong
// to no permission: they are never read, and a saved policy leaves them as
// the row has them. Clipboard keeps its own assignments.clipboard_permissions
// policy.
const bits = Object.freeze({ drives: 1, printers: 2, usbDevices: 4, smartCards: 8, serialPorts: 64 });
export const devicePolicyKeys = Object.freeze(Object.keys(bits));
export const devicePolicyBits = devicePolicyKeys.reduce((mask, key) => mask | bits[key], 0);
const maximumMask = 127; // the column's CHECK

// An absent or invalid stored mask never grants a permission.
export function devicePolicyFromMask(mask) {
  const value = Number.isSafeInteger(mask) && mask >= 0 && mask <= maximumMask ? mask : 0;
  return Object.fromEntries(devicePolicyKeys.map(key => [key, !!(value & bits[key])]));
}
export function devicePolicyMask(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).length !== devicePolicyKeys.length ||
    !devicePolicyKeys.every(key => Object.hasOwn(value, key) && typeof value[key] === 'boolean')) throw Error('invalid_device_policy');
  return devicePolicyKeys.reduce((mask, key) => mask | (value[key] ? bits[key] : 0), 0);
}
