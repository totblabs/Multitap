import 'dart:async';

import 'package:flutter/services.dart';

import 'client_diagnostic_codes.dart';
import 'usb_redirection.dart';

/// The Windows runner's USB capture (windows/runner/usb_capture.cpp): lists
/// this PC's USB devices, captures one through UsbDk (a signed filter driver
/// that takes the device away from its own Windows driver while it is
/// captured and gives it back when the capture ends) and runs the VM's
/// requests on it. UsbDk is installed only from [installDriver], which the
/// user reaches through an explicit action in the USB panel; the runner then
/// verifies the installer's signature before it runs.
///
/// Channel `multitap/usb`:
///   environment            -> {driver: ready|missing|unsupported}
///   list                   -> [{id, name, subtitle, vidPid, kind, neededByPc, chips, arrivalMs, memoryKey,
///                               busyElsewhere, descriptors, speed, busy, otherWindow?}]
///   prepare {id, generation} -> {ready, checked, blocker} (storage check, no explicit dismount)
///   cancelPrepare {id, generation} -> null (between steps; original call still completes)
///   watch / unwatch        -> null; from the runner devices_changed while watching
///   capture {id, window?}  -> handle (PlatformException: usb_device_busy, usb_device_redirected,
///                              usb_capture_failed; details {error}: UsbDk's Win32 error)
///   transfer {handle, token, endpoint, type, length, setup?, data}
///                          -> {status, data}
///   cancel {handle, token}
///   release {handle}      -> {urbs, kib, stalls, failed, timeouts, slow, firstClass, firstStatus, slowClass, firstUsbd}
///   returned {id, storage} -> {state, why}: after a release, did Windows start the device again (0 unknown, 1 yes, 2 no);
///                             why: for 2, what was missing (1 no node, 2 not started, 3 no disk, 100 + problem code)
///   install                -> null (PlatformException: usbdk_signature_invalid, usbdk_install_failed, usbdk_elevation_cancelled,
///                              usbdk_restart_required, usbdk_missing)
///   holder {id}            -> {held}: does another remote window of this client hold it
///   handOver {id, window}  -> {asked}: asks that window to give it back
/// and from the runner: gone {handle} when a captured device left the bus,
/// hand_over {id, window} when another window asks for a device this one holds.
/// Every remote window has its own channel and engine; [window] is this
/// window's VM name, which the other windows show for what it holds.
class NativeUsbBackend extends UsbBackend {
  NativeUsbBackend([MethodChannel? channel, this.window = ''])
    : _channel = channel ?? const MethodChannel('multitap/usb') {
    _channel.setMethodCallHandler(_fromNative);
  }
  final MethodChannel _channel;
  final String window;
  final _captures = <int, _NativeCapture>{};
  final _preparing = <String, int>{};
  int _prepareGeneration = 0;
  final _changes = StreamController<void>.broadcast();
  final _handOvers = StreamController<({String id, String window})>.broadcast();

  @override
  Stream<void> get changes => _changes.stream;

  @override
  Stream<({String id, String window})> get handOvers => _handOvers.stream;

  @override
  Future<bool> handOver(UsbLocalDevice device) async {
    try {
      final value = await _channel.invokeMapMethod<String, Object?>(
        'handOver',
        {'id': device.id, 'window': window},
      );
      return value?['asked'] == true;
    } on PlatformException {
      return false;
    }
  }

  @override
  Future<bool> heldElsewhere(UsbLocalDevice device) async {
    try {
      final value = await _channel.invokeMapMethod<String, Object?>('holder', {
        'id': device.id,
      });
      return value?['held'] == true;
    } on PlatformException {
      return false; // the capture says it if it is still held
    }
  }

  @override
  Future<void> watch(bool on) async {
    try {
      await _channel.invokeMethod<void>(on ? 'watch' : 'unwatch');
    } on PlatformException {
      /* Without live updates the panel still has its refresh button. */
    }
  }

  @override
  Future<UsbPrepareResult> prepare(UsbLocalDevice device) async {
    final generation = ++_prepareGeneration;
    _preparing[device.id] = generation;
    try {
      final value = await _channel.invokeMapMethod<String, Object?>('prepare', {
        'id': device.id,
        'generation': generation,
      });
      if (value == null) return const UsbPrepareResult(checked: false);
      final blocker = value['blocker'];
      return UsbPrepareResult(
        ready: value['ready'] != false,
        checked: value['checked'] != false,
        // A program name Windows gave; shown on this PC only, bounded.
        blocker: blocker is String && blocker.length <= 120 ? blocker : '',
      );
    } on PlatformException {
      return const UsbPrepareResult(checked: false);
    } finally {
      if (_preparing[device.id] == generation) _preparing.remove(device.id);
    }
  }

  @override
  void cancelPrepare(UsbLocalDevice device) {
    final generation = _preparing[device.id];
    if (generation == null) return;
    unawaited(
      _channel
          .invokeMethod<void>('cancelPrepare', {
            'id': device.id,
            'generation': generation,
          })
          .catchError((Object _) {}),
    );
  }

  Future<void> dispose() async {
    _channel.setMethodCallHandler(null);
    unawaited(_changes.close());
    unawaited(_handOvers.close());
    for (final capture in _captures.values.toList()) {
      try {
        await capture.release();
      } catch (_) {
        /* Session reports uncertain return. */
      }
    }
  }

  Future<Object?> _fromNative(MethodCall call) async {
    if (call.method == 'gone') {
      final handle = (call.arguments as Map?)?['handle'];
      if (handle is int) _captures[handle]?.markGone();
    } else if (call.method == 'devices_changed') {
      if (!_changes.isClosed) _changes.add(null);
    } else if (call.method == 'hand_over') {
      final args = call.arguments as Map?;
      final id = args?['id'], window = args?['window'];
      if (id is String && !_handOvers.isClosed) {
        _handOvers.add((id: id, window: _text(window, 120)));
      }
    }
    return null;
  }

  @override
  Future<UsbEnvironment> environment() async {
    final value = await _channel.invokeMapMethod<String, Object?>(
      'environment',
    );
    return UsbEnvironment(switch (value?['driver']) {
      'ready' => UsbDriverState.ready,
      'unsupported' => UsbDriverState.unsupported,
      _ => UsbDriverState.missing,
    });
  }

  @override
  Future<List<UsbLocalDevice>> list() async {
    final value = await _channel.invokeListMethod<Object?>('list') ?? const [];
    final out = <UsbLocalDevice>[];
    for (final item in value) {
      if (item is! Map ||
          item['id'] is! String ||
          item['name'] is! String ||
          item['descriptors'] is! Uint8List ||
          !{'low', 'full', 'high'}.contains(item['speed'])) {
        continue;
      }
      out.add(
        UsbLocalDevice(
          id: item['id'] as String,
          name: item['name'] as String,
          descriptors: item['descriptors'] as Uint8List,
          speed: item['speed'] as String,
          busy: item['busy'] == true,
          subtitle: _text(item['subtitle'], 160),
          vidPid: _text(item['vidPid'], 9),
          kind: UsbKind.parse(item['kind']),
          neededByPc: item['neededByPc'] == true,
          chips: [
            for (final chip
                in item['chips'] is List ? item['chips'] as List : const [])
              if (chip is String && chip.isNotEmpty && chip.length <= 20) chip,
          ].take(4).toList(),
          arrivalMs: item['arrivalMs'] is int ? item['arrivalMs'] as int : 0,
          memoryKey: _text(item['memoryKey'], 80),
          busyElsewhere: item['busyElsewhere'] == true,
          noReadableDrive: item['noReadableDrive'] == true,
          unreadable: item['unreadable'] == true,
          // Present only when another remote window holds it; an oversized
          // name is dropped, the fact is kept.
          otherWindow: item['otherWindow'] is String
              ? _text(item['otherWindow'], 120)
              : null,
        ),
      );
    }
    return out;
  }

  @override
  Future<UsbCapture> capture(UsbLocalDevice device) async {
    try {
      final handle = await _channel.invokeMethod<int>('capture', {
        'id': device.id,
        if (window.isNotEmpty) 'window': window,
      });
      if (handle == null) throw const UsbBackendException('usb_capture_failed');
      final capture = _NativeCapture(this, handle);
      _captures[handle] = capture;
      return capture;
    } on PlatformException catch (error) {
      final details = error.details;
      final native = details is Map ? details['error'] : null;
      throw UsbBackendException(
        _code(error.code, 'usb_capture_failed'),
        error: native is int && native > 0 && native <= 0xffffffff ? native : 0,
      );
    }
  }

  final _returnDetail = <String, int>{};

  @override
  Future<int> returned(UsbLocalDevice device) async {
    _returnDetail.remove(device.id);
    try {
      final value = await _channel.invokeMapMethod<String, Object?>(
        'returned',
        {'id': device.id, 'storage': device.kind == UsbKind.storage},
      );
      final state = value?['state'], why = value?['why'];
      if (state == 2 && why is int && why > 0 && why < 1000) {
        _returnDetail[device.id] = why;
      }
      return state is int && state >= 0 && state <= 2 ? state : 0;
    } on PlatformException {
      return 0;
    }
  }

  @override
  int returnDetail(UsbLocalDevice device) => _returnDetail[device.id] ?? 0;

  @override
  Future<void> installDriver() async {
    try {
      await _channel.invokeMethod<void>('install');
    } on PlatformException catch (error) {
      throw UsbBackendException(_code(error.code, 'usbdk_install_failed'));
    }
  }

  static String _text(Object? value, int max) =>
      value is String && value.length <= max ? value : '';

  /// Only a code the product knows crosses; native text never does. The
  /// local-only codes are the user declining the Windows administrator prompt
  /// of the driver installation and a device UsbDk already gives to someone
  /// else (shown, reported as another diagnostic code).
  static String _code(String code, String fallback) =>
      (code.startsWith('usb_') || code.startsWith('usbdk_')) &&
          (diagnosticCodes.contains(code) ||
              code == usbdkElevationCancelled ||
              code == usbDeviceRedirected)
      ? code
      : fallback;
}

class _NativeCapture implements UsbCapture {
  _NativeCapture(this._backend, this._handle);
  final NativeUsbBackend _backend;
  final int _handle;
  final _gone = Completer<void>();
  bool _released = false;
  Future<UsbStats>? _releaseFuture;

  void markGone() {
    if (!_gone.isCompleted) _gone.complete();
  }

  @override
  Future<void> get gone => _gone.future;

  @override
  Future<UsbTransferResult> transfer(UsbTransfer transfer) async {
    final value = await _backend._channel.invokeMapMethod<String, Object?>(
      'transfer',
      {
        'handle': _handle,
        'token': transfer.token,
        'endpoint': transfer.endpoint,
        'type': transfer.type,
        'length': transfer.length,
        'setup': transfer.setup,
        'data': transfer.data,
      },
    );
    final status = value?['status'];
    final data = value?['data'];
    if (status is! int) return UsbTransferResult(usbStatusIo, Uint8List(0));
    return UsbTransferResult(status, data is Uint8List ? data : Uint8List(0));
  }

  @override
  void cancel(int token) {
    if (_released) return;
    unawaited(
      _backend._channel
          .invokeMethod<void>('cancel', {'handle': _handle, 'token': token})
          .catchError((Object _) {}),
    );
  }

  @override
  Future<UsbStats> release() => _releaseFuture ??= _releaseOnce();

  Future<UsbStats> _releaseOnce() async {
    _released = true;
    try {
      final value = await _backend._channel.invokeMapMethod<String, Object?>(
        'release',
        {'handle': _handle},
      );
      int number(String key) {
        final raw = value?[key];
        return raw is int && raw >= 0 && raw <= 0xffffffff ? raw : 0;
      }

      return UsbStats(
        urbs: number('urbs'),
        kib: number('kib'),
        stalls: number('stalls'),
        failed: number('failed'),
        timeouts: number('timeouts'),
        slow: number('slow'),
        firstClass: number('firstClass'),
        firstStatus: number('firstStatus'),
        slowClass: number('slowClass'),
        firstUsbd: number('firstUsbd'),
      );
    } catch (_) {
      throw const UsbBackendException(usbReturnUnknown);
    } finally {
      _backend._captures.remove(_handle);
    }
  }
}
