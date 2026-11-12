import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'product_log.dart';
import 'usb_redirection.dart';

/// 마우스 잠금 over USB (docs/usb-redirection.md). With the option on
/// and USB granted, this session lends the VM Multitap's own virtual USB HID
/// mouse; while the window holds the mouse, this PC's Raw Input feeds it
/// (ToTB Link's smooth mode), so a game reads a USB mouse instead of injected
/// input. The runner builds the reports (windows/runner/game_mouse_usb.h);
/// this side attaches the device over the session's usb-v1 channel and passes
/// frames both ways. Anything missing (no USB grant, no VM controller, an
/// older Host) leaves the injected movement of relative-mouse-v1.
///
/// The device and configuration descriptors usb.attach carries: report-protocol
/// HID (not the boot mouse the policy denies), byte for byte ToTB Link's mouse.
/// The Host knows the device by exactly these bytes (the shared vector
/// "multitap game mouse" in server/test/fixtures/usb-descriptor-vectors.json).
final gameMouseDescriptors = Uint8List.fromList(const [
  // device: USB 2.0, 64-byte endpoint 0, 1209:0001, strings 1-3, one configuration
  18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64, 0x09, 0x12, 0x01, 0x00, //
  0x00, 0x01, 1, 2, 3, 1,
  // configuration: one interface, bus powered, remote wakeup, 100 mA
  9, 0x02, 34, 0, 1, 1, 0, 0xA0, 50,
  // interface 0: HID, no boot subclass, one endpoint
  9, 0x04, 0, 0, 1, 0x03, 0x00, 0x00, 0,
  // HID 1.11, one 79-byte report descriptor
  9, 0x21, 0x11, 0x01, 0, 1, 0x22, 79, 0,
  // interrupt IN 0x81, 7-byte reports, every millisecond
  7, 0x05, 0x81, 0x03, 7, 0, 1,
]);

/// The runner's side (windows/runner/game_mouse_usb_channel.h).
abstract interface class UsbGameMouseNative {
  Future<void> attached(int device);
  Future<void> detached();

  /// One Host frame; `ok`, `older` (a Host that does not answer the device
  /// itself) or `invalid`.
  Future<String> frame(Uint8List bytes);

  /// [send]: a frame to the relay; [confirmed]: a Host that reads ahead.
  void listen({
    required void Function(Uint8List frame) send,
    required void Function() confirmed,
  });
  void dispose();
}

class ChannelUsbGameMouse implements UsbGameMouseNative {
  static const _channel = MethodChannel('multitap/game-mouse-usb');

  @override
  Future<void> attached(int device) => _call('attached', device);
  @override
  Future<void> detached() => _call('detached');

  @override
  Future<String> frame(Uint8List bytes) async {
    try {
      return await _channel.invokeMethod<String>('frame', bytes) ?? 'invalid';
    } on MissingPluginException {
      return 'invalid';
    } on PlatformException {
      return 'invalid';
    }
  }

  Future<void> _call(String method, [Object? argument]) async {
    try {
      await _channel.invokeMethod<void>(method, argument);
    } on MissingPluginException {
      // Widget tests and previews have no runner.
    } on PlatformException {
      // The device simply stays unused.
    }
  }

  @override
  void listen({
    required void Function(Uint8List frame) send,
    required void Function() confirmed,
  }) {
    _channel.setMethodCallHandler((call) async {
      final arguments = call.arguments;
      if (call.method == 'send' && arguments is Uint8List) send(arguments);
      if (call.method == 'confirmed') confirmed();
    });
  }

  @override
  void dispose() => _channel.setMethodCallHandler(null);
}

/// The virtual mouse of one session window.
class UsbGameMouse extends ChangeNotifier implements UsbVirtualDevice {
  UsbGameMouse({required this.usb, UsbGameMouseNative? native})
    : _native = native ?? ChannelUsbGameMouse() {
    _native.listen(send: _sendFrame, confirmed: _confirmed);
    usb.addListener(_sync);
  }

  final UsbSession usb;
  final UsbGameMouseNative _native;
  bool _wanted = false, _attached = false, _readAhead = false;
  // An older Host or a refusal: not offered again in this session unless the
  // user turns the option off and on.
  bool _older = false, _refused = false, _disposed = false;
  int? _id;
  int _generation = 0;

  /// The VM reads the device through a Host that answers it itself: the game
  /// mouse sends real USB reports.
  bool get active => _attached && _readAhead && !_older && !usb.closed;

  /// Still on its way: wanted, nothing ruled it out, the VM has not confirmed
  /// it yet (the channel may still be connecting).
  bool get attaching =>
      _wanted && !_older && !_refused && !_disposed && !usb.closed && !active;

  /// Why it is not used, for the log only (a fixed code), or null.
  String? reason;

  /// The user's 마우스 잠금 choice, while the session can use it.
  set enabled(bool on) {
    if (on && !_wanted) _refused = false;
    _wanted = on;
    _sync();
  }

  void _sync() {
    if (_disposed) return;
    final id = _id;
    if (_wanted && id == null && !_older && !_refused && usb.ready) {
      _id = usb.attachVirtual(this, gameMouseDescriptors);
      if (_id != null) {
        _generation++;
        ProductLog.instance.record(LogEvent.usb, {
          'stage': 7,
          'ok': true,
          'usbClass': 3,
        });
      }
    } else if (id != null && (!_wanted || usb.closed)) {
      // A closing channel says goodbye for every device itself.
      if (usb.closed) {
        reason = usb.error ?? 'usb_session_closed';
      } else {
        usb.detachVirtual(id);
      }
      _gone();
    }
  }

  void _gone() {
    final changed = active;
    _generation++;
    _id = null;
    _attached = _readAhead = false;
    unawaited(_native.detached());
    if (changed) notifyListeners();
  }

  @override
  void attached() {
    final id = _id;
    if (_disposed || id == null) return;
    _attached = true;
    unawaited(_native.attached(id));
  }

  @override
  void ended(String reason) {
    if (_disposed || _id == null) return;
    this.reason = reason;
    _refused = reason != 'usb_session_closed';
    _gone();
  }

  @override
  void frame(Uint8List bytes) {
    final id = _id;
    final generation = _generation;
    if (_disposed || id == null) return;
    unawaited(
      _native.frame(bytes).then((seen) {
        // A toggle may have retired this device while its native reply was
        // pending. Only that exact attachment may be faulted or detached.
        if (seen == 'ok' ||
            _disposed ||
            generation != _generation ||
            _id != id) {
          return;
        }
        // An older Host forwards the device's control requests: it cannot read
        // ahead, so the game mouse keeps injected movement for this session.
        _older = seen == 'older';
        reason = _older ? 'usb_host_older' : 'usb_invalid_message';
        usb.detachVirtual(id);
        _gone();
      }),
    );
  }

  void _sendFrame(Uint8List frame) {
    if (_disposed || _id == null || !_attached) return;
    usb.sendVirtualFrame(frame);
  }

  void _confirmed() {
    if (_disposed || !_attached) return;
    _readAhead = true;
    reason = null;
    notifyListeners();
  }

  @override
  void dispose() {
    if (_disposed) return;
    _generation++;
    usb.removeListener(_sync);
    final id = _id;
    if (id != null) {
      usb.detachVirtual(id);
      unawaited(_native.detached());
    }
    _disposed = true;
    _native.dispose();
    super.dispose();
  }
}
