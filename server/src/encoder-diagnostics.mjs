const object = value => value && typeof value === 'object' && !Array.isArray(value);
const u32 = value => Number.isSafeInteger(value) && value > 0 && value <= 0xffffffff;
const gpuCodes = code => Number.isSafeInteger(code) && (code === 0 || (code >= 32 && code <= 44));
const keys = ['protocol', 'backend', 'initialized', 'hardwareVerified', 'adapterLuid', 'vendorId', 'deviceId', 'adapterName', 'gpuFallbackCode', 'softwareFallbackCode'];
const vendors = { nvenc: 0x10de, amf: 0x1002, 'qsv-mft': 0x8086 };
const name = value => typeof value === 'string' && value.length > 0 && value.length <= 128 && Buffer.byteLength(value) <= 512 && !/[\x00-\x1f\x7f-\x9f\u2028\u2029\ud800-\udfff]/u.test(value);
export function safeEncoderInfo(value) {
  if (!object(value) || Object.keys(value).length !== keys.length || !keys.every(k => Object.hasOwn(value, k)) ||
      value.protocol !== 'h264-encoder-v1' || !['x264', 'mft', ...Object.keys(vendors)].includes(value.backend) ||
      typeof value.initialized !== 'boolean' || typeof value.hardwareVerified !== 'boolean' || !gpuCodes(value.gpuFallbackCode) ||
      !Number.isSafeInteger(value.softwareFallbackCode) || value.softwareFallbackCode < 0 || value.softwareFallbackCode > 7) return null;
  if ((value.vendorId !== null && !u32(value.vendorId)) || (value.deviceId !== null && !u32(value.deviceId)) ||
      (value.adapterName !== null && !name(value.adapterName)) || (value.adapterLuid !== null &&
      (typeof value.adapterLuid !== 'string' || !/^[0-9a-f]{8}:[0-9a-f]{8}$/.test(value.adapterLuid) || value.adapterLuid === '00000000:00000000'))) return null;
  const vendor = vendors[value.backend], identity = [value.adapterLuid, value.vendorId, value.deviceId, value.adapterName].some(v => v !== null);
  if ((!vendor || !value.initialized) && identity) return null;
  if (vendor && value.vendorId !== null && value.vendorId !== vendor) return null;
  if (value.hardwareVerified && (!value.initialized || !vendor || value.adapterLuid === null || value.vendorId !== vendor)) return null;
  return Object.fromEntries(keys.map(k => [k, value[k]]));
}
export function safeHostEncoderDiagnostic(value) {
  if (!object(value) || value.protocol !== 'host-encoder-diagnostics-v1' || typeof value.observed !== 'boolean' || !Array.isArray(value.tracks)) return null;
  if (!value.observed) return value.observationAgeMs === null && value.tracks.length === 0 ? { protocol: value.protocol, observed: false, observationAgeMs: null, tracks: [] } : null;
  if (!Number.isSafeInteger(value.observationAgeMs) || value.observationAgeMs < 0 || value.observationAgeMs > 5000 || value.tracks.length < 1 || value.tracks.length > 2) return null;
  const ids = new Set(), tracks = [];
  for (const row of value.tracks) {
    const info = safeEncoderInfo(row?.info);
    if (!object(row) || Object.keys(row).length !== 3 || !['0', '1'].includes(row.id) || ids.has(row.id) || !Number.isSafeInteger(row.epoch) || row.epoch < 0 || row.epoch > 0xffffffff || !info) return null;
    ids.add(row.id); tracks.push({ id: row.id, epoch: row.epoch, info });
  }
  return { protocol: value.protocol, observed: true, observationAgeMs: value.observationAgeMs, tracks };
}
