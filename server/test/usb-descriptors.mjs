// Builds USB descriptor sets for the usb-v1 tests. The same sets, as hex with
// the expected policy decision, live in fixtures/usb-descriptor-vectors.json;
// the Host (C++) and client (Dart) policy tests read that file, so the three
// implementations are checked against identical bytes.
export function descriptors({ deviceClass = 0, subclass = 0, protocol = 0, vendor = 0x1234, product = 0x5678, maxPacket0 = 64, configs }) {
  const parts = [];
  const configurations = configs.map((config, index) => {
    const body = [];
    for (const item of config.interfaces) {
      body.push(Buffer.from([9, 4, item.number ?? 0, item.alternate ?? 0, item.endpoints.length, item.class, item.subclass ?? 0, item.protocol ?? 0, 0]));
      for (const [address, attributes, maxPacket] of item.endpoints) body.push(Buffer.from([7, 5, address, attributes, maxPacket & 255, maxPacket >> 8, 0]));
      for (const extra of item.extra ?? []) body.push(Buffer.from(extra));
    }
    const joined = Buffer.concat(body), total = 9 + joined.length;
    const interfaces = new Set(config.interfaces.map(item => item.number ?? 0)).size;
    return Buffer.concat([Buffer.from([9, 2, total & 255, total >> 8, interfaces, config.value ?? index + 1, 0, 0x80, 50]), joined]);
  });
  parts.push(Buffer.from([18, 1, 0, 2, deviceClass, subclass, protocol, maxPacket0, vendor & 255, vendor >> 8, product & 255, product >> 8, 0, 1, 0, 0, 0, configs.length]));
  parts.push(...configurations);
  return Buffer.concat(parts);
}

const bulkPair = [[0x81, 2, 512], [0x02, 2, 512]];
const storage = { number: 0, class: 8, subclass: 6, protocol: 0x50, endpoints: bulkPair };
// name -> [spec, expected policy decision]
export const vectors = {
  'mass storage stick': [{ configs: [{ interfaces: [storage] }] }, { allowed: true, requires: ['drives'], classes: [8] }],
  'cdc serial adapter': [{ deviceClass: 2, configs: [{ interfaces: [
    { number: 0, class: 2, subclass: 2, protocol: 1, endpoints: [[0x83, 3, 16]], extra: [[5, 0x24, 0, 0x10, 0x01]] },
    { number: 1, class: 10, endpoints: bulkPair }] }] }, { allowed: true, requires: ['serialPorts'], classes: [2, 10] }],
  'vendor specific serial chip': [{ configs: [{ interfaces: [{ number: 0, class: 255, endpoints: bulkPair }] }] }, { allowed: true, requires: [], classes: [255] }],
  'printer': [{ configs: [{ interfaces: [{ number: 0, class: 7, subclass: 1, protocol: 2, endpoints: bulkPair }] }] }, { allowed: true, requires: ['printers'], classes: [7] }],
  'smart card reader': [{ configs: [{ interfaces: [{ number: 0, class: 11, endpoints: [[0x81, 2, 64], [0x02, 2, 64], [0x83, 3, 8]] }] }] }, { allowed: true, requires: ['smartCards'], classes: [11] }],
  'fido key (report-protocol hid)': [{ configs: [{ interfaces: [{ number: 0, class: 3, endpoints: [[0x81, 3, 64], [0x01, 3, 64]] }] }] }, { allowed: true, requires: [], classes: [3] }],
  'storage plus serial composite': [{ configs: [{ interfaces: [storage, { number: 1, class: 255, endpoints: bulkPair }, { number: 2, class: 10, endpoints: bulkPair }] }] }, { allowed: true, requires: ['drives', 'serialPorts'], classes: [8, 10, 255] }],
  'boot keyboard': [{ configs: [{ interfaces: [{ number: 0, class: 3, subclass: 1, protocol: 1, endpoints: [[0x81, 3, 8]] }] }] }, { allowed: false, reason: 'usb_hid_boot_denied' }],
  'boot mouse': [{ configs: [{ interfaces: [{ number: 0, class: 3, subclass: 1, protocol: 2, endpoints: [[0x81, 3, 4]] }] }] }, { allowed: false, reason: 'usb_hid_boot_denied' }],
  'keyboard with a storage interface': [{ configs: [{ interfaces: [storage, { number: 1, class: 3, subclass: 1, protocol: 1, endpoints: [[0x82, 3, 8]] }] }] }, { allowed: false, reason: 'usb_hid_boot_denied' }],
  'hub': [{ deviceClass: 9, configs: [{ interfaces: [{ number: 0, class: 9, endpoints: [[0x81, 3, 1]] }] }] }, { allowed: false, reason: 'usb_class_denied' }],
  'bluetooth adapter': [{ deviceClass: 0xe0, subclass: 1, protocol: 1, configs: [{ interfaces: [{ number: 0, class: 0xe0, subclass: 1, protocol: 1, endpoints: [[0x81, 3, 16], [0x02, 2, 64], [0x82, 2, 64]] }] }] }, { allowed: false, reason: 'usb_class_denied' }],
  'usb headset (audio, isochronous)': [{ configs: [{ interfaces: [
    { number: 1, alternate: 0, class: 1, subclass: 2, endpoints: [] },
    { number: 1, alternate: 1, class: 1, subclass: 2, endpoints: [[0x01, 0x09, 192]] }] }] }, { allowed: false, reason: 'usb_isochronous_denied' }],
  'webcam (video)': [{ configs: [{ interfaces: [{ number: 0, class: 14, subclass: 2, endpoints: [[0x83, 3, 16]] }] }] }, { allowed: false, reason: 'usb_isochronous_denied' }],
  'vendor device with an isochronous endpoint': [{ configs: [{ interfaces: [{ number: 0, class: 255, endpoints: [[0x81, 1, 128]] }] }] }, { allowed: false, reason: 'usb_isochronous_denied' }],
};

// Sets the builder cannot make, kept as their bytes: the client's virtual game mouse
// (usb_game_mouse.dart) has strings, a HID descriptor before its endpoint and a 1 ms interval.
export const recorded = {
  'multitap game mouse (report-protocol hid, made up by the client)': ['12010002000000400912010000010102030109022200010100a032090400000103000000092111010001224f0007058103070001', { allowed: true, requires: [], classes: [3] }],
};

export function vectorFile() {
  return Object.fromEntries([
    ...Object.entries(vectors).map(([name, [spec, expect]]) => [name, { descriptors: descriptors(spec).toString('hex'), expect }]),
    ...Object.entries(recorded).map(([name, [hex, expect]]) => [name, { descriptors: hex, expect }]),
  ]);
}
