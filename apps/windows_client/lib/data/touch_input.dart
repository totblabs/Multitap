/// touch-input-v1 (docs/remote-session-protocol.md "Touch input"): the
/// contacts of a touch screen (the Android app's direct touch mode) sent as
/// `{"type":"input","event":"touch","contacts":[...]}` and injected by the
/// Host as Windows touch input, so applications get real pans, pinch-zoom and
/// press-and-hold instead of an emulated mouse. Only a connection whose
/// broker granted the protocol (a Host that advertised it) may send it;
/// released peers keep mouse/key input only.
library;

const touchInputProtocol = 'touch-input-v1';

/// Contacts per frame (the relay's and the Host's bound); the Host also
/// holds at most this many at once.
const touchInputMaxContacts = 10;

/// Largest contact id (ids are 0..2^31-1, e.g. Flutter's PointerEvent.pointer
/// or the platform pointer id).
const touchInputMaxId = 0x7fffffff;

enum RemoteTouchPhase { down, move, up, cancel }

/// One contact of a touch frame. [x] and [y] are 0..1 over the streamed
/// picture (the remote source rectangle, the same space as mouse input).
/// [pressure] is 0..1; [width] and [height] (both or neither) are the contact
/// size as fractions of the picture's width and height, > 0 and <= 1.
class RemoteTouchContact {
  const RemoteTouchContact({
    required this.id,
    required this.phase,
    required this.x,
    required this.y,
    this.pressure,
    this.width,
    this.height,
  });
  final int id;
  final RemoteTouchPhase phase;
  final double x, y;
  final double? pressure, width, height;

  bool get valid {
    bool unit(double value) => value.isFinite && value >= 0 && value <= 1;
    bool size(double? value) =>
        value != null && value.isFinite && value > 0 && value <= 1;
    return id >= 0 &&
        id <= touchInputMaxId &&
        unit(x) &&
        unit(y) &&
        (pressure == null || unit(pressure!)) &&
        (width == null) == (height == null) &&
        (width == null || (size(width) && size(height)));
  }

  Map<String, Object> toJson() => {
    'id': id,
    'phase': phase.name,
    'x': x,
    'y': y,
    'pressure': ?pressure,
    if (width != null && height != null) ...{'w': width!, 'h': height!},
  };
}

/// The touch-input-v1 message for one frame, or null when the frame is empty,
/// has more than [touchInputMaxContacts] contacts, repeats an id or holds an
/// invalid contact (the relay would end the session for it).
///
/// A frame lists the contacts that changed: `down` once when a finger lands,
/// `move` while it moves (or to say it is still there), `up` when it lifts
/// and `cancel` when the gesture is abandoned. The Host keeps the others
/// where they were, refreshes held contacts itself and ignores move/up/cancel
/// of contacts it already released (an `input.reset` releases all of them).
Map<String, dynamic>? touchInputMessage(List<RemoteTouchContact> contacts) {
  if (contacts.isEmpty || contacts.length > touchInputMaxContacts) return null;
  final ids = <int>{};
  for (final contact in contacts) {
    if (!contact.valid || !ids.add(contact.id)) return null;
  }
  return {
    'type': 'input',
    'event': 'touch',
    'contacts': [for (final contact in contacts) contact.toJson()],
  };
}
