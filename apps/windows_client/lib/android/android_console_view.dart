import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/broker_api.dart';
import '../data/emergency_console_session.dart';
import '../ui/app_dialog.dart';
import '../ui/app_theme.dart';
import '../ui/app_toast.dart';
import '../ui/session_menu.dart';
import 'android_session_keepalive.dart';
import 'android_toolbar.dart';
import 'android_tv.dart';
import 'android_tv_remote.dart';
import '../l10n/l10n.dart';

typedef ConsoleImageDecoder = Future<ui.Image> Function(
  Uint8List rgba,
  int width,
  int height,
);

Future<ui.Image> _decode(Uint8List rgba, int width, int height) {
  final result = Completer<ui.Image>();
  ui.decodeImageFromPixels(
    rgba,
    width,
    height,
    ui.PixelFormat.rgba8888,
    result.complete,
  );
  return result.future;
}

/// Owns exactly one console attempt, its input, image and keepalive lease.
/// Reconnect obtains a new unstarted attempt through the launcher callback.
class AndroidConsoleView extends StatefulWidget {
  const AndroidConsoleView({
    super.key,
    required this.session,
    required this.desktopName,
    this.isTelevision = false,
    this.onReconnect,
    this.keepalive,
    this.decodeImage = _decode,
  });
  final EmergencyConsoleSession session;
  final String desktopName;
  final bool isTelevision;
  final Future<EmergencyConsoleSession> Function()? onReconnect;
  final AndroidSessionKeepaliveOwner? keepalive;
  final ConsoleImageDecoder decodeImage;

  @override
  State<AndroidConsoleView> createState() => _AndroidConsoleViewState();
}

class _AndroidConsoleViewState extends State<AndroidConsoleView>
    with WidgetsBindingObserver {
  late EmergencyConsoleSession _session;
  late AndroidSessionKeepaliveOwner _keepalive;
  final _focus = FocusNode(debugLabel: 'console picture');
  final _bar = GlobalKey<AndroidSessionBarState>();
  final _tv = GlobalKey<AndroidTvRemoteState>();
  final _hold = AndroidPointerHold();
  final _popups = SessionPopups();
  ui.Image? _image, _cursor;
  int _epoch = 0, _painted = -1, _cursorPainted = -1;
  int _sourceWidth = 0, _sourceHeight = 0;
  bool _decoding = false, _cursorDecoding = false;
  Timer? _decodeTimer;
  Object? _decodeFlight;
  bool _foreground = true, _closing = false, _allowPop = false;
  bool _tvPanel = false, _modal = false, _retrying = false, _original = false;
  bool _positionKnown = false;
  String? _retryFailure;
  Offset _position = const Offset(.5, .5), _pan = Offset.zero;
  Offset? _touchDown;
  int _buttons = 0;
  EmergencyConsoleSession? _mouseOwner;
  final _pointers = <int>{};
  final _hardwareKeys = <PhysicalKeyboardKey, int>{};
  PointerDeviceKind _lastKind = PointerDeviceKind.touch;
  Rect _picture = Rect.zero;
  AndroidTvPointerMode _tvMode = AndroidTvPointerMode.pointer;

  bool get _available =>
      mounted &&
      _foreground &&
      !_closing &&
      !_retrying &&
      !_allowPop &&
      _session.connected &&
      _image != null &&
      _image!.width == _session.rfb.width &&
      _image!.height == _session.rfb.height;
  bool get _input =>
      _available &&
      !_tvPanel &&
      !_modal &&
      ModalRoute.of(context)?.isCurrent != false;

  @override
  void initState() {
    super.initState();
    _session = widget.session;
    _keepalive =
        widget.keepalive ??
        AndroidSessionKeepaliveOwner(
          onDisconnect: () async {
            await _session.close();
          },
        );
    _foreground =
        WidgetsBinding.instance.lifecycleState == null ||
        WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed;
    WidgetsBinding.instance.addObserver(this);
    _focus.addListener(_focusChanged);
    _attach();
  }

  void _attach() {
    _session.addListener(_changed);
    unawaited(_keepalive.start());
    unawaited(_session.start());
    _changed();
  }

  @override
  void didUpdateWidget(AndroidConsoleView old) {
    super.didUpdateWidget(old);
    if (!identical(old.session, widget.session)) {
      _replace(widget.session);
    }
  }

  void _replace(EmergencyConsoleSession next) {
    _cancelDecode();
    _release();
    _popups.closeAll();
    _tvPanel = false;
    final old = _session;
    old.removeListener(_changed);
    old.dispose();
    _keepalive.stop();
    ++_epoch;
    _image?.dispose();
    _image = null;
    _cursor?.dispose();
    _cursor = null;
    _painted = _cursorPainted = -1;
    _decoding = _cursorDecoding = false;
    _positionKnown = false;
    _pan = Offset.zero;
    _retryFailure = null;
    _session = next;
    _attach();
  }

  void _changed() {
    if (!mounted) return;
    if (_session.closed) {
      _cancelDecode();
      _release();
      _keepalive.stop();
      _popups.closeAll();
    }
    final rfb = _session.rfb;
    if (rfb.width != _sourceWidth || rfb.height != _sourceHeight) {
      _release();
      _sourceWidth = rfb.width;
      _sourceHeight = rfb.height;
      _positionKnown = false;
      _pan = Offset.zero;
      _popups.closeAll();
    }
    if (!_session.closed &&
        rfb.frameValid &&
        !_decoding &&
        rfb.frameRevision != _painted) {
      final session = _session, epoch = _epoch, revision = rfb.frameRevision;
      final width = rfb.width, height = rfb.height;
      _decoding = true;
      final flight = Object();
      _decodeFlight = flight;
      _decodeTimer = Timer(const Duration(seconds: 5), () {
        if (!mounted ||
            epoch != _epoch ||
            !identical(session, _session) ||
            !identical(flight, _decodeFlight) ||
            session.closed) {
          return;
        }
        _cancelDecode();
        _retryFailure = 'console_timeout';
        _release();
        unawaited(session.close());
      });
      final pixels = Uint8List.fromList(rfb.pixels);
      Future<ui.Image>.sync(
        () => widget.decodeImage(pixels, rfb.width, rfb.height),
      ).then(
        (image) {
          if (!mounted ||
              epoch != _epoch ||
              !identical(session, _session) ||
              !identical(flight, _decodeFlight) ||
              session.closed) {
            image.dispose();
            return;
          }
          _cancelDecode();
          if (!session.rfb.frameValid ||
              session.rfb.width != width ||
              session.rfb.height != height) {
            image.dispose();
            _decoding = false;
            _changed();
            return;
          }
          _image?.dispose();
          _image = image;
          _painted = revision;
          _decoding = false;
          setState(() {});
          _changed();
        },
        onError: (Object _) {
          if (!mounted ||
              epoch != _epoch ||
              !identical(session, _session) ||
              !identical(flight, _decodeFlight)) {
            return;
          }
          _cancelDecode();
          _retryFailure = 'console_invalid_frame';
          unawaited(session.close());
        },
      );
    }
    if (!_cursorDecoding && rfb.cursorRevision != _cursorPainted) {
      final shape = rfb.cursor;
      _cursorPainted = rfb.cursorRevision;
      _cursor?.dispose();
      _cursor = null;
      if (shape != null && !shape.hidden && !_session.closed) {
        final session = _session, epoch = _epoch, revision = rfb.cursorRevision;
        _cursorDecoding = true;
        widget
            .decodeImage(
              Uint8List.fromList(shape.rgba),
              shape.width,
              shape.height,
            )
            .then(
              (image) {
                if (!mounted ||
                    epoch != _epoch ||
                    !identical(session, _session) ||
                    session.closed) {
                  image.dispose();
                  return;
                }
                _cursorDecoding = false;
                if (revision != session.rfb.cursorRevision) {
                  image.dispose();
                  _changed();
                  return;
                }
                _cursor = image;
                setState(() {});
              },
              onError: (Object _) {
                if (mounted &&
                    epoch == _epoch &&
                    identical(session, _session)) {
                  _cursorDecoding = false;
                }
              },
            );
      }
    }
    setState(() {});
  }

  void _focusChanged() {
    if (!_focus.hasFocus) _release();
  }

  void _cancelDecode() {
    _decodeTimer?.cancel();
    _decodeTimer = null;
    _decodeFlight = null;
    _decoding = false;
  }

  void _release() {
    _tv.currentState?.cancel();
    _session.rfb.releaseInput();
    _hardwareKeys.clear();
    _buttons = 0;
    _touchDown = null;
    _mouseOwner = null;
    for (final pointer in _pointers) {
      _hold.up(pointer);
    }
    _pointers.clear();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _foreground = state == AppLifecycleState.resumed;
    if (!_foreground) {
      _release();
      _popups.closeAll();
    }
    if (mounted) setState(() {});
  }

  Offset? _mapped(Offset local) {
    if (_picture.isEmpty || !_picture.contains(local)) return null;
    return Offset(
      ((local.dx - _picture.left) / _picture.width).clamp(0, 1),
      ((local.dy - _picture.top) / _picture.height).clamp(0, 1),
    );
  }

  void _pointer(Offset normalized, int buttons) {
    if (!_input) return;
    _position = normalized;
    _positionKnown = true;
    _buttons = buttons;
    _session.rfb.pointer(
      (normalized.dx * (_session.rfb.width - 1)).round(),
      (normalized.dy * (_session.rfb.height - 1)).round(),
      buttons,
    );
    setState(() {});
  }

  void _click(Offset point, bool right) {
    if (!_input) return;
    _pointer(point, right ? 4 : 1);
    _pointer(point, 0);
  }

  void _scroll(Offset point, double delta) {
    if (!_input || delta == 0) return;
    final mask = delta > 0 ? 16 : 8;
    final held = _buttons;
    for (var n = 0; n < (delta.abs() / 120).ceil().clamp(1, 8); n++) {
      _pointer(point, held | mask);
      _pointer(point, held);
    }
  }

  KeyEventResult _key(FocusNode node, KeyEvent event) {
    if (!_input) return KeyEventResult.ignored;
    final key = event.logicalKey;
    final existing = _hardwareKeys[event.physicalKey];
    if (event is KeyUpEvent && existing != null) {
      _session.rfb.key(existing, false);
      _hardwareKeys.remove(event.physicalKey);
      return KeyEventResult.handled;
    }
    // Secure attention is exclusively the explicit toolbar command.
    if (key == LogicalKeyboardKey.delete &&
        HardwareKeyboard.instance.isControlPressed &&
        HardwareKeyboard.instance.isAltPressed) {
      return KeyEventResult.handled;
    }
    final sym = _keysym(key, event.character);
    if (sym == null) return KeyEventResult.ignored;
    if (event is! KeyUpEvent) {
      _hardwareKeys[event.physicalKey] = sym;
      _session.rfb.key(sym, true);
    }
    return KeyEventResult.handled;
  }

  static int? _keysym(LogicalKeyboardKey key, String? character) {
    final special = <LogicalKeyboardKey, int>{
      LogicalKeyboardKey.enter: 0xff0d,
      LogicalKeyboardKey.numpadEnter: 0xff0d,
      LogicalKeyboardKey.backspace: 0xff08,
      LogicalKeyboardKey.tab: 0xff09,
      LogicalKeyboardKey.escape: 0xff1b,
      LogicalKeyboardKey.delete: 0xffff,
      LogicalKeyboardKey.insert: 0xff63,
      LogicalKeyboardKey.home: 0xff50,
      LogicalKeyboardKey.end: 0xff57,
      LogicalKeyboardKey.pageUp: 0xff55,
      LogicalKeyboardKey.pageDown: 0xff56,
      LogicalKeyboardKey.arrowLeft: 0xff51,
      LogicalKeyboardKey.arrowUp: 0xff52,
      LogicalKeyboardKey.arrowRight: 0xff53,
      LogicalKeyboardKey.arrowDown: 0xff54,
      LogicalKeyboardKey.shiftLeft: 0xffe1,
      LogicalKeyboardKey.shiftRight: 0xffe2,
      LogicalKeyboardKey.controlLeft: 0xffe3,
      LogicalKeyboardKey.controlRight: 0xffe4,
      LogicalKeyboardKey.altLeft: 0xffe9,
      LogicalKeyboardKey.altRight: 0xffea,
      LogicalKeyboardKey.metaLeft: 0xffeb,
      LogicalKeyboardKey.metaRight: 0xffec,
      LogicalKeyboardKey.capsLock: 0xffe5,
    };
    if (special.containsKey(key)) return special[key];
    final f = [
      LogicalKeyboardKey.f1,
      LogicalKeyboardKey.f2,
      LogicalKeyboardKey.f3,
      LogicalKeyboardKey.f4,
      LogicalKeyboardKey.f5,
      LogicalKeyboardKey.f6,
      LogicalKeyboardKey.f7,
      LogicalKeyboardKey.f8,
      LogicalKeyboardKey.f9,
      LogicalKeyboardKey.f10,
      LogicalKeyboardKey.f11,
      LogicalKeyboardKey.f12,
    ].indexOf(key);
    if (f >= 0) return 0xffbe + f;
    final text =
        character != null &&
            character.runes.length == 1 &&
            character.runes.single >= 32 &&
            character.runes.single != 127
        ? character
        : key.keyLabel.toLowerCase();
    if (text.runes.length != 1) return null;
    final rune = text.runes.single;
    if (rune < 32 || rune == 127) return null;
    return rune <= 255 ? rune : 0x01000000 | rune;
  }

  void _combo(List<int> keys) {
    if (!_available) return;
    _release();
    final session = _session;
    for (final key in keys) {
      session.rfb.key(key, true);
    }
    for (final key in keys.reversed) {
      session.rfb.key(key, false);
    }
  }

  Future<void> _keyboard() async {
    if (!_available || _modal) return;
    final session = _session;
    _release();
    setState(() => _modal = true);
    final track = _popups.tracker();
    final hardwareKeyboard = AndroidTvScope.hasHardwareKeyboard(context);
    String? value;
    try {
      value = await showDialog<String>(
        context: context,
        builder: (_) => track(
          AndroidTvScope(
            television: widget.isTelevision,
            hardwareKeyboard: hardwareKeyboard,
            child: _ConsoleTextDialog(isTelevision: widget.isTelevision),
          ),
        ),
      );
    } finally {
      if (mounted) setState(() => _modal = false);
    }
    if (!mounted ||
        !_available ||
        !identical(session, _session) ||
        value == null) {
      return;
    }
    if (value.length > 8192) {
      showAppToast(
        context,
        l10n.textTooLongSendEnter,
        kind: AppToastKind.failed,
      );
      return;
    }
    for (final rune in value.runes) {
      final key = rune == 10
          ? 0xff0d
          : rune <= 255
          ? rune
          : 0x01000000 | rune;
      session.rfb.key(key, true);
      session.rfb.key(key, false);
    }
    _focus.requestFocus();
  }

  Future<void> _close() async {
    if (_closing || _allowPop) return;
    _release();
    _popups.closeAll();
    setState(() => _closing = true);
    final answer = await showDialog<bool>(
      context: context,
      builder: (context) {
        final p = AppPalette.of(context);
        return AppDialog(
          title: l10n.disconnectConsole,
          icon: Icons.link_off_rounded,
          body: [AppDialog.message(context, l10n.vmsPowerWindowsWorkStay)],
          actions: [
            TextButton(
              key: const Key('console-close-cancel'),
              onPressed: () => Navigator.pop(context, false),
              style: AppButtons.quiet(p),
              child: Text(l10n.cancel),
            ),
            FilledButton(
              key: const Key('console-close-confirm'),
              onPressed: () => Navigator.pop(context, true),
              style: AppButtons.filled(p),
              child: Text(l10n.disconnect),
            ),
          ],
        );
      },
    );
    if (!mounted) return;
    if (answer != true) {
      setState(() => _closing = false);
      return;
    }
    final session = _session;
    await session.close();
    if (!mounted || !identical(session, _session)) return;
    setState(() => _allowPop = true);
    Navigator.of(context).pop();
  }

  Future<void> _retry() async {
    if (_retrying ||
        _closing ||
        !_session.canRetry ||
        widget.onReconnect == null) {
      return;
    }
    final old = _session;
    _release();
    setState(() {
      _retrying = true;
      _retryFailure = null;
    });
    try {
      await old.close();
      if (!mounted || _closing || _allowPop || !identical(old, _session)) {
        return;
      }
      final next = await widget.onReconnect!();
      if (!mounted || _closing || _allowPop || !identical(old, _session)) {
        next.dispose();
        return;
      }
      _replace(next);
    } on BrokerException catch (error) {
      if (mounted && identical(old, _session)) _retryFailure = error.code;
    } catch (_) {
      if (mounted && identical(old, _session)) {
        _retryFailure = 'console_transport_closed';
      }
    } finally {
      if (mounted) setState(() => _retrying = false);
    }
  }

  List<SessionToolbarMenu> _actions() => [
    SessionToolbarMenu.command(
      label: l10n.keyboard,
      tooltip: l10n.consoleKeyboard,
      key: const Key('console-keyboard-menu'),
      icon: Icons.keyboard_rounded,
      onInteract: _release,
      onPressed: _available ? () => unawaited(_keyboard()) : null,
    ),
    SessionToolbarMenu(
      label: l10n.screen,
      tooltip: l10n.fitScreenOriginalSize,
      icon: Icons.fit_screen_rounded,
      onInteract: _release,
      items: [
        for (final original in [false, true])
          SessionMenuItem(
            original ? l10n.originalSize : l10n.fitScreen,
            checked: _original == original,
            onSelected: () {
              _release();
              setState(() {
                _original = original;
                _pan = Offset.zero;
              });
            },
          ),
      ],
    ),
    SessionToolbarMenu(
      label: l10n.keyCombos,
      tooltip: l10n.keyCombos,
      icon: Icons.keyboard_command_key_rounded,
      onInteract: _release,
      items: [
        SessionMenuItem(
          'Ctrl+Alt+Del',
          key: const Key('console-secure-attention'),
          onSelected: _available
              ? () => _combo([0xffe3, 0xffe9, 0xffff])
              : null,
        ),
        SessionMenuItem(
          'Alt+Tab',
          onSelected: _available ? () => _combo([0xffe9, 0xff09]) : null,
        ),
        SessionMenuItem(
          l10n.windowsKey,
          onSelected: _available ? () => _combo([0xffeb]) : null,
        ),
      ],
    ),
    if (widget.isTelevision)
      SessionToolbarMenu(
        label: l10n.remote,
        tooltip: l10n.remoteControl,
        icon: Icons.settings_remote_rounded,
        onInteract: _release,
        items: [
          for (final mode in AndroidTvPointerMode.values)
            SessionMenuItem(
              mode.label,
              checked: mode == _tvMode,
              onSelected: () {
                _release();
                setState(() => _tvMode = mode);
              },
            ),
        ],
      ),
  ];

  Widget _canvas(Size size) {
    final rfb = _session.rfb, image = _image;
    final source = Size(rfb.width.toDouble(), rfb.height.toDouble());
    final scale = source.isEmpty
        ? 1.0
        : _original
        ? 1.0
        : math.min(size.width / source.width, size.height / source.height);
    final shown = source * scale;
    final left = shown.width <= size.width
        ? (size.width - shown.width) / 2
        : _pan.dx.clamp(size.width - shown.width, 0.0);
    final top = shown.height <= size.height
        ? (size.height - shown.height) / 2
        : _pan.dy.clamp(size.height - shown.height, 0.0);
    _picture = Rect.fromLTWH(left, top, shown.width, shown.height);
    final shape = rfb.cursor;
    return Listener(
      key: const Key('console-input-surface'),
      onPointerDown: (event) {
        _lastKind = event.kind;
        if (!_input) return;
        _bar.currentState?.fold();
        _hold.down(event.pointer);
        _pointers.add(event.pointer);
        _focus.requestFocus();
        if (event.kind == PointerDeviceKind.mouse) {
          _mouseOwner = _session;
          final point = _mapped(event.localPosition);
          if (point != null) _pointer(point, _mouseMask(event.buttons));
        }
      },
      onPointerMove: (event) {
        if (event.kind == PointerDeviceKind.mouse &&
            (event.buttons == 0 || identical(_mouseOwner, _session))) {
          final point = _mapped(event.localPosition);
          if (point != null) _pointer(point, _mouseMask(event.buttons));
        }
      },
      onPointerHover: (event) {
        final point = _mapped(event.localPosition);
        if (point != null) _pointer(point, 0);
      },
      onPointerUp: (event) {
        _hold.up(event.pointer);
        _pointers.remove(event.pointer);
        if (event.kind == PointerDeviceKind.mouse && _input) {
          _pointer(_position, 0);
        }
      },
      onPointerCancel: (event) {
        _hold.up(event.pointer);
        _pointers.remove(event.pointer);
        _release();
      },
      onPointerSignal: (event) {
        if (event is PointerScrollEvent) {
          final point = _mapped(event.localPosition);
          if (point != null) _scroll(point, event.scrollDelta.dy);
        }
      },
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTapDown: (details) => _touchDown = _mapped(details.localPosition),
        onTap: () {
          if (_lastKind != PointerDeviceKind.mouse && _touchDown != null) {
            _click(_touchDown!, false);
          }
        },
        onLongPress: () {
          if (_lastKind != PointerDeviceKind.mouse && _touchDown != null) {
            _click(_touchDown!, true);
          }
        },
        onScaleStart: (_) {
          if (_lastKind != PointerDeviceKind.mouse && _touchDown != null) {
            _pointer(_touchDown!, 1);
          }
        },
        onScaleUpdate: (details) {
          if (_lastKind == PointerDeviceKind.mouse || _touchDown == null) {
            return;
          }
          if (details.pointerCount > 1 && _original) {
            _session.rfb.releaseInput();
            _buttons = 0;
            setState(() => _pan += details.focalPointDelta);
          } else {
            final point = _mapped(details.localFocalPoint);
            if (point != null) _pointer(point, 1);
          }
        },
        onScaleEnd: (_) {
          if (_input && _buttons != 0) _pointer(_position, 0);
        },
        child: ClipRect(
          child: Stack(
            fit: StackFit.expand,
            children: [
              const ColoredBox(color: Colors.black),
              if (image != null && !_picture.isEmpty)
                Positioned.fromRect(
                  rect: _picture,
                  child: RawImage(
                    key: const Key('console-frame'),
                    image: image,
                    fit: BoxFit.fill,
                    filterQuality: FilterQuality.none,
                  ),
                ),
              if (_cursor != null &&
                  shape != null &&
                  !shape.hidden &&
                  _positionKnown &&
                  _input)
                Positioned(
                  left:
                      _picture.left +
                      (_position.dx * (rfb.width - 1)).round() * scale -
                      shape.hotX * scale,
                  top:
                      _picture.top +
                      (_position.dy * (rfb.height - 1)).round() * scale -
                      shape.hotY * scale,
                  width: shape.width * scale,
                  height: shape.height * scale,
                  child: IgnorePointer(
                    child: RawImage(
                      key: const Key('console-cursor'),
                      image: _cursor,
                      filterQuality: FilterQuality.none,
                    ),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }

  int _mouseMask(int buttons) =>
      (buttons & kPrimaryMouseButton != 0 ? 1 : 0) |
      (buttons & kMiddleMouseButton != 0 ? 2 : 0) |
      (buttons & kSecondaryMouseButton != 0 ? 4 : 0);

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final failed = _session.closed || _retryFailure != null;
    final blockedRetry = {
      'authorization_revoked',
      'console_permission_denied',
      'desktop_not_found',
      'session_expired',
      'console_identity_changed',
      'console_auth_failed',
      'console_unsupported',
    }.contains(_retryFailure);
    return PopScope(
      canPop: _allowPop,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) unawaited(_close());
      },
      child: Scaffold(
        resizeToAvoidBottomInset: false,
        body: Focus(
          focusNode: _focus,
          autofocus: true,
          onKeyEvent: _key,
          child: LayoutBuilder(
            builder: (context, bounds) => Stack(
              children: [
                Positioned.fill(
                  child: AndroidTvRemote(
                    key: _tv,
                    enabled: widget.isTelevision && _input,
                    session: _session,
                    position: () => _position,
                    source: () => Size(
                      _session.rfb.width.toDouble(),
                      _session.rfb.height.toDouble(),
                    ),
                    canControl: () => _input,
                    onMove: (point) => _pointer(point, _buttons),
                    onClick: _click,
                    onScroll: (point, delta) =>
                        _scroll(point, -delta.toDouble()),
                    onMenu: () {
                      _release();
                      setState(() => _tvPanel = true);
                    },
                    mode: _tvMode,
                    onDragStart: (point) {
                      final session = _session;
                      _pointer(point, 1);
                      return (point, {required cancelled}) {
                        if (identical(session, _session) && !session.closed) {
                          session.rfb.pointer(
                            (point.dx * (session.rfb.width - 1)).round(),
                            (point.dy * (session.rfb.height - 1)).round(),
                            0,
                          );
                          _buttons = 0;
                        }
                      };
                    },
                    child: _canvas(bounds.biggest),
                  ),
                ),
                if (!_session.connected || _image == null || failed)
                  Positioned.fill(
                    child: ColoredBox(
                      color: const Color(0xB3000000),
                      child: Center(
                        child: SingleChildScrollView(
                          padding:
                              MediaQuery.viewPaddingOf(context) +
                              const EdgeInsets.all(24),
                          child: ConstrainedBox(
                            constraints: const BoxConstraints(maxWidth: 420),
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                if (!failed) const CircularProgressIndicator(),
                                const SizedBox(height: 16),
                                Text(
                                  failed
                                      ? consoleFailureMessage(
                                          _retryFailure ?? _session.failureCode,
                                        )
                                      : _retrying
                                      ? l10n.reconnecting
                                      : l10n.waitingConsoleScreen,
                                  textAlign: TextAlign.center,
                                  style: uiText(size: 15, color: Colors.white),
                                ),
                                const SizedBox(height: 16),
                                Wrap(
                                  alignment: WrapAlignment.center,
                                  spacing: 8,
                                  runSpacing: 8,
                                  children: [
                                    if (failed &&
                                        _session.canRetry &&
                                        !blockedRetry &&
                                        widget.onReconnect != null)
                                      FilledButton(
                                        key: const Key('console-retry'),
                                        onPressed: _retrying || _closing
                                            ? null
                                            : () => unawaited(_retry()),
                                        style: AppButtons.filled(p),
                                        child: Text(l10n.reconnect),
                                      ),
                                    TextButton(
                                      key: const Key('console-close'),
                                      onPressed: _closing
                                          ? null
                                          : () => unawaited(_close()),
                                      style: AppButtons.quiet(p),
                                      child: Text(l10n.close),
                                    ),
                                  ],
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
                // The tools stay while the console waits for a picture or its
                // size changes (a Host park ending, Windows signing in), as the
                // PC's toolbar does; what needs a picture is disabled until
                // then (_available, checked by every action and by _keyboard).
                if (_session.connected && _image != null && !_closing)
                  Positioned(
                    right: MediaQuery.viewPaddingOf(context).right + 12,
                    bottom: MediaQuery.viewPaddingOf(context).bottom + 12,
                    child: AndroidKeyboardButton(
                      key: const Key('console-keyboard-floating'),
                      keyboardOpen: false,
                      onPressed: () => unawaited(_keyboard()),
                    ),
                  ),
                if (!_closing && !_allowPop)
                  AndroidSessionBar(
                    key: _bar,
                    // The amber bubble anywhere above the floating keyboard
                    // button's row.
                    bubbleArea: MediaQuery.viewPaddingOf(context).deflateRect(
                      Rect.fromLTRB(
                        0,
                        0,
                        bounds.maxWidth,
                        bounds.maxHeight - 68,
                      ),
                    ),
                    title: widget.desktopName,
                    modeLabel: l10n.console,
                    actions: _actions(),
                    onInteract: _release,
                    onDisconnect: () => unawaited(_close()),
                    pointerHeld: _hold.held,
                    popups: _popups,
                    onOpenChanged: (open) {
                      if (open) _release();
                    },
                  ),
                if (_tvPanel)
                  AndroidTvLocalPanel(
                    onClose: () => setState(() => _tvPanel = false),
                    child: AndroidToolbarPanel(
                      title: l10n.consoleTools,
                      children: [
                        TextButton.icon(
                          onPressed: () {
                            setState(() => _tvPanel = false);
                            unawaited(_keyboard());
                          },
                          icon: const Icon(Icons.keyboard_rounded),
                          label: Text(l10n.keyboard),
                        ),
                        TextButton(
                          onPressed: () {
                            setState(() {
                              _original = !_original;
                              _tvPanel = false;
                              _pan = Offset.zero;
                            });
                          },
                          child: Text(_original ? l10n.fitScreen : l10n.originalSize),
                        ),
                        for (final mode in AndroidTvPointerMode.values)
                          TextButton(
                            onPressed: () {
                              setState(() {
                                _tvMode = mode;
                                _tvPanel = false;
                              });
                            },
                            child: Text(mode.label),
                          ),
                        TextButton(
                          onPressed: _available
                              ? () {
                                  setState(() => _tvPanel = false);
                                  _combo([0xffe3, 0xffe9, 0xffff]);
                                }
                              : null,
                          child: const Text('Ctrl+Alt+Del'),
                        ),
                      ],
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  @override
  void dispose() {
    ++_epoch;
    _cancelDecode();
    WidgetsBinding.instance.removeObserver(this);
    _session.removeListener(_changed);
    _release();
    _session.dispose();
    _keepalive.stop();
    _popups.closeAll();
    _focus.removeListener(_focusChanged);
    _focus.dispose();
    _hold.dispose();
    _image?.dispose();
    _cursor?.dispose();
    super.dispose();
  }
}

class _ConsoleTextDialog extends StatefulWidget {
  const _ConsoleTextDialog({required this.isTelevision});
  final bool isTelevision;
  @override
  State<_ConsoleTextDialog> createState() => _ConsoleTextDialogState();
}

class _ConsoleTextDialogState extends State<_ConsoleTextDialog> {
  final text = TextEditingController();
  @override
  void dispose() {
    text.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return AppDialog(
      title: l10n.consoleKeyboard,
      icon: Icons.keyboard_rounded,
      body: [
        AndroidTvTextEntry(
          controller: text,
          label: l10n.textType,
          obscureText: true,
          maxLength: 1024,
          onSubmitted: (value) => Navigator.pop(context, value),
          builder: (focus, readOnly, edit) => TextField(
            key: const Key('console-text-input'),
            controller: text,
            focusNode: focus,
            readOnly: readOnly,
            obscureText: true,
            onTap: edit,
            autofocus: !widget.isTelevision,
            maxLength: 1024,
            maxLines: 1,
            enableSuggestions: false,
            autocorrect: false,
            decoration: InputDecoration(labelText: l10n.textType),
            onSubmitted: (value) => Navigator.pop(context, value),
          ),
        ),
      ],
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: Text(l10n.cancel),
        ),
        FilledButton(
          key: const Key('console-text-send'),
          onPressed: () => Navigator.pop(context, text.text),
          style: AppButtons.filled(p),
          child: Text(l10n.send),
        ),
      ],
    );
  }
}
