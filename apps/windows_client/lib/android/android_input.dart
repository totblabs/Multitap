class AndroidRemoteKey {
  const AndroidRemoteKey(this.vk, {this.extended = false});
  final int vk;
  final bool extended;
}

/// Ordinary Windows keys only. No secure-attention or USB forwarding contract.
const androidKeyChords = <String, List<AndroidRemoteKey>>{
  'windows': [AndroidRemoteKey(0x5b, extended: true)],
  'altTab': [AndroidRemoteKey(0xa4), AndroidRemoteKey(0x09)],
  'paste': [AndroidRemoteKey(0xa2), AndroidRemoteKey(0x56)],
  'copy': [AndroidRemoteKey(0xa2), AndroidRemoteKey(0x43)],
  'selectAll': [AndroidRemoteKey(0xa2), AndroidRemoteKey(0x41)],
  'undo': [AndroidRemoteKey(0xa2), AndroidRemoteKey(0x5a)],
  'hangul': [AndroidRemoteKey(0x15)],
  'tab': [AndroidRemoteKey(0x09)],
  'shiftTab': [AndroidRemoteKey(0xa0), AndroidRemoteKey(0x09)],
  'escape': [AndroidRemoteKey(0x1b)],
  'enter': [AndroidRemoteKey(0x0d)],
  'backspace': [AndroidRemoteKey(0x08)],
  'delete': [AndroidRemoteKey(0x2e, extended: true)],
  'left': [AndroidRemoteKey(0x25, extended: true)],
  'up': [AndroidRemoteKey(0x26, extended: true)],
  'right': [AndroidRemoteKey(0x27, extended: true)],
  'down': [AndroidRemoteKey(0x28, extended: true)],
  'pageUp': [AndroidRemoteKey(0x21, extended: true)],
  'pageDown': [AndroidRemoteKey(0x22, extended: true)],
};

/// One explicit key combination from the session menu: the remote input is
/// reset first, the keys are pressed and released in order, and input is
/// released again, so a combination can never leave a key held.
class AndroidInputActions {
  AndroidInputActions({
    required this.ready,
    required this.acquired,
    required this.capture,
    required this.send,
  });
  final bool Function() ready;
  final bool Function() acquired;
  final Future<void> Function(bool) capture;
  final void Function(Map<String, dynamic>) send;
  int _revision = 0;
  bool _busy = false;
  bool get busy => _busy;

  void invalidate() {
    _revision++;
  }

  Future<bool> chord(String action) async {
    final keys = androidKeyChords[action];
    if (_busy || keys == null || !ready()) return false;
    final revision = ++_revision;
    _busy = true;
    try {
      await capture(false);
      if (!ready() || revision != _revision) return false;
      await capture(true);
      if (!ready() || revision != _revision || !acquired()) return false;
      for (final down in [true, false]) {
        for (final key in down ? keys : keys.reversed) {
          if (!ready() || revision != _revision) return false;
          send({
            'type': 'input',
            'event': 'key',
            'vk': key.vk,
            'scan': 0,
            'extended': key.extended,
            'down': down,
          });
        }
      }
      return true;
    } finally {
      // A focus loss can invalidate an in-flight acquisition. Always release it.
      try {
        await capture(false);
      } finally {
        _busy = false;
      }
    }
  }
}
