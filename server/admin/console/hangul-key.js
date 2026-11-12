// Windows can deliver the physical Korean toggle as a lone keyup. noVNC drops
// orphan releases; repair only this identified key, using its public tap API.
export function createHangulKeyAdapter({getRfb, canSend, platform = globalThis.navigator?.userAgentData?.platform || globalThis.navigator?.platform || ''}) {
  let pairedDown = false;
  const windows = /^Win/i.test(platform);
  function reset() { pairedDown = false; }
  function handle(event) {
    const canvas = event.target;
    const missingCode = !event.code || event.code === 'Unidentified';
    const unidentified = !event.key || event.key === 'Unidentified';
    const hangul = event.key === 'HangulMode' ||
      (event.code === 'Lang1' && unidentified) ||
      (missingCode && unidentified && event.keyCode === 0x15);
    if (!windows || (!hangul && event.code !== 'Lang1') || !event.isTrusted || !['keydown','keyup'].includes(event.type) ||
        canvas?.tagName !== 'CANVAS' || canvas.ownerDocument?.activeElement !== canvas) return false;
    const rfb = getRfb();
    if (!rfb || !canSend() || rfb.viewOnly) { reset(); return false; }
    if (!hangul) { pairedDown = event.type === 'keydown'; return false; }
    if (event.type === 'keydown') {
      pairedDown = true;
      return false;
    }
    if (pairedDown) { pairedDown = false; return false; }
    if (event.repeat) return false;
    event.preventDefault(); event.stopImmediatePropagation();
    rfb.sendKey(0xff31, 'Lang1');
    return true;
  }
  return {handle, reset};
}
