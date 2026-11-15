// speaker-mute-v1 (Host apps/windows_host/src/speaker_mute.hpp): a physical
// PC's own speakers are silent while it is streamed. The Host cannot tell a
// physical PC from a VM itself; the broker can (desktop kind 'pc'). So a Host
// that knows the feature advertises it, and the broker adds the marker to the
// session.start of a stream of a physical PC, nothing else. A VM's session
// never carries it, and a Host that does not advertise it never sees the
// field. Clients take no part: they neither ask for it nor see it. Never a
// reason to refuse or end a connection.
export const speakerMuteProtocol = 'speaker-mute-v1';

// Host capability: exactly the marker; anything else is omitted.
export function speakerMuteCapability(value) {
  return value?.speakerMuteProtocol === speakerMuteProtocol;
}

// The session.start field of a stream session: only for a physical PC whose
// Host advertises it. `session` carries localPc, mode and its host.
export function speakerMuteStartFields(session) {
  return session?.localPc === true && session.mode === 'stream' && speakerMuteCapability(session.host?.capabilities) ? { speakerMuteProtocol } : {};
}
