import { createHash } from 'node:crypto';

// same-pc-v1: lets a Windows app tell that a listed desktop's Host runs on
// the PC the app itself runs on, so it refuses that connection (the Host would
// move every window, the app included, onto its virtual display).
// The app derives the same value from its own SMBIOS system UUID and compares.
// One-way and bound to the desktop id: the Host's UUID itself never leaves
// the server, and the tags of two desktops cannot be compared with each other.
const machineUuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/;

// Lowercase hex SHA-256, or null for anything but a well-formed UUID.
export function hostMachineTag(desktopId, uuid) {
  if (typeof desktopId !== 'string' || !desktopId || typeof uuid !== 'string') return null;
  const value = uuid.toLowerCase();
  if (!machineUuid.test(value)) return null;
  return createHash('sha256').update(`multitap-same-pc-v1\n${desktopId}\n${value}`).digest('hex');
}
