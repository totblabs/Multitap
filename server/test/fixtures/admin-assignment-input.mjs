// Model an identity-aware admin caller: capture immutable IDs BEFORE sending.
// Dedicated missing/stale identity tests intentionally do not use this helper.
import { randomUUID, randomBytes } from 'node:crypto';
export function enrollFixtureHosts(store, ids, actor = 'fixture') {
  for (const id of ids) {
    const known = store.db.prepare('SELECT pve_machine_uuid FROM desktops WHERE id=?').get(id);
    const uuid = known?.pve_machine_uuid ?? randomUUID(), code = randomBytes(32).toString('hex');
    store.issueEnrollment(id, uuid, code, actor);
    store.consumeEnrollment(code, uuid, { hostname: 'synthetic-registered-host', version: '1.0.0', tokenHash: randomBytes(32).toString('hex') });
  }
}
export function captureAssignmentInput(store, path, input) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) return input;
  const bind = row => {
    if (!row || typeof row !== 'object' || Array.isArray(row) || Object.hasOwn(row, 'expectedAccountId')) return row;
    const account = store.findUser(row.username);
    return account ? { ...row, expectedAccountId: account.id } : row;
  };
  if (/\/assignments(?:\/batch)?$/.test(path)) return bind(input);
  if (/\/desktops\/[^/]+\/permissions$/.test(path) && Array.isArray(input.permissions)) return { ...input, permissions: input.permissions.map(bind) };
  return input;
}
