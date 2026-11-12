/// text-input-v1 (docs/remote-session-protocol.md "Text input"): committed
/// text from an on-screen keyboard or IME, sent as
/// `{"type":"input","event":"text","text":...}` and injected by the Host as
/// Unicode key events, so Korean and emoji arrive as typed whatever the remote
/// keyboard layout or IME mode. Enter, Tab, Backspace and editing keys stay
/// `key` events. Only a connection whose broker granted the protocol (and so
/// a Host that advertised it) may send it; released peers keep keys only.
library;

const textInputProtocol = 'text-input-v1';

/// UTF-16 code units per message (the relay's and the Host's bound).
const textInputMaxUnits = 256;

bool _control(int unit) => unit < 0x20 || (unit >= 0x7f && unit <= 0x9f);

/// Validate one committed-text packet without repairing or logging its text.
/// Native input and delayed clipboard queues must obey the negotiated wire
/// shape as well as the same bounds enforced by the broker and Host.
bool validTextInputMessage(Map<String, dynamic> message) {
  if (message.length != 3 ||
      message['type'] != 'input' ||
      message['event'] != 'text') {
    return false;
  }
  final text = message['text'];
  if (text is! String || text.isEmpty || text.length > textInputMaxUnits) {
    return false;
  }
  final units = text.codeUnits;
  for (var i = 0; i < units.length; i++) {
    final unit = units[i];
    if (_control(unit) || (unit >= 0xdc00 && unit <= 0xdfff)) return false;
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++i >= units.length || units[i] < 0xdc00 || units[i] > 0xdfff) {
        return false;
      }
    }
  }
  return true;
}

/// [text] as text-input-v1 payloads: control characters and unpaired
/// surrogates removed, split into chunks of at most [textInputMaxUnits]
/// UTF-16 units without separating a surrogate pair.
List<String> textInputChunks(String text) {
  final chunks = <String>[];
  final current = StringBuffer();
  var units = 0;
  for (final rune in text.runes) {
    if (rune < 0x10000 &&
        (_control(rune) || (rune >= 0xd800 && rune <= 0xdfff))) {
      continue;
    }
    final size = rune >= 0x10000 ? 2 : 1;
    if (units + size > textInputMaxUnits) {
      chunks.add(current.toString());
      current.clear();
      units = 0;
    }
    current.writeCharCode(rune);
    units += size;
  }
  if (units > 0) chunks.add(current.toString());
  return chunks;
}

/// The text-input-v1 input messages for [text] (never sampled for latency).
List<Map<String, dynamic>> textInputMessages(String text) => [
  for (final chunk in textInputChunks(text))
    {'type': 'input', 'event': 'text', 'text': chunk},
];
