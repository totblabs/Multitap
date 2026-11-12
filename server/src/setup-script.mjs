import { validatePublicOrigin } from './config.mjs';

// Keep the source template unconfigured. Each broker installation supplies its
// canonical origin when serving its own enrollment script.
export function renderProxmoxSetup(template, publicOrigin, transportOptions) {
  const origin = validatePublicOrigin(publicOrigin, transportOptions);
  const marker = "BROKER_ORIGIN = ''";
  if (typeof template !== 'string' || template.split(marker).length !== 2 || !/^BROKER_ORIGIN = ''\r?$/m.test(template)) throw new Error('invalid_proxmox_setup_template');
  return template.replace(marker, 'BROKER_ORIGIN = ' + JSON.stringify(origin));
}
