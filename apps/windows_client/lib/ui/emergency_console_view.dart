import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/emergency_console_session.dart';
import '../data/console_keyboard.dart';
import '../data/console_pointer.dart';
import '../data/session_windows.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'session_status_panel.dart';
import 'session_toolbar.dart';
import '../l10n/l10n.dart';

/// Local input is used only while this exact console picture has focus.
/// Windows uses the existing exclusive keyboard owner; VM display/driver
/// configuration is never changed by this view.
class EmergencyConsoleView extends StatefulWidget {
  const EmergencyConsoleView({
    super.key,
    required this.name,
    required this.create,
    required this.windowActions,
    this.keyboard,
    this.decodeRgba,
  });
  final String name;
  final EmergencyConsoleSession Function() create;
  final SessionWindowActions windowActions;

  /// View-owned fake for tests; it never installs an OS hook.
  final ConsoleKeyboardCapture? keyboard;

  /// Memory-only codec seam for deterministic first/late-image fixtures.
  final Future<ui.Image> Function(Uint8List bytes, int width, int height)?
  decodeRgba;
  @override
  State<EmergencyConsoleView> createState() => _EmergencyConsoleViewState();
}

class _EmergencyConsoleViewState extends State<EmergencyConsoleView>
    with WidgetsBindingObserver {
  final _focus = FocusNode(debugLabel: 'emergency-console');
  EmergencyConsoleSession? _session;
  ui.Image? _image, _cursorImage;
  int _imageRevision = -1, _cursorRevision = -1;
  bool _original = false, _decoding = false, _decodeScheduled = false;
  bool _closing = false, _confirming = false, _hovered = false;
  bool _picturePressed = false;
  Offset _pointer = Offset.zero;
  final _pressed = <PhysicalKeyboardKey, int>{};
  late final ConsoleKeyboardCapture _keyboard;
  int _inputGeneration = 0, _inputIntent = 0;
  bool _activatingKeyboard = false, _keyboardFailure = false;
  EmergencyConsoleSession? _paintedSession;
  Size? _paintedSize;
  late final ConsolePointerPacer _pointerPacer;
  (double, double, double)? _pointerGeometry;
  int _pointerEdgeDepth = 0;

  bool get _pictureCurrent {
    final session = _session, image = _image;
    return session != null &&
        session.connected &&
        identical(_paintedSession, session) &&
        image != null &&
        _paintedSize ==
            Size(session.rfb.width.toDouble(), session.rfb.height.toDouble()) &&
        image.width == session.rfb.width &&
        image.height == session.rfb.height;
  }

  bool get _pictureReady => !_closing && !_confirming && _pictureCurrent;

  @override
  void initState() {
    super.initState();
    _pointerPacer = ConsolePointerPacer((x, y, mask) {
      if (_pictureReady && (_focus.hasFocus || _pointerEdgeDepth > 0)) {
        _session!.rfb.pointer(x, y, mask);
      }
    });
    _keyboard =
        widget.keyboard ??
        (!kIsWeb && defaultTargetPlatform == TargetPlatform.windows
            ? NativeConsoleKeyboard()
            : NoConsoleKeyboardCapture());
    _keyboard.onKey = (symbol, down) {
      if (_pictureReady && _focus.hasFocus && _session?.connected == true) {
        _session!.rfb.key(symbol, down);
      }
    };
    _keyboard.onPhysicalKey = (event) {
      if (_pictureReady && _focus.hasFocus && _session?.connected == true) {
        _session!.rfb.physicalKey(
          event.physical,
          event.keysym,
          event.down,
          event.qemuKeyCode,
        );
      }
    };
    _keyboard.onReset = () {
      // Acquiring the keyboard may reset keys while a pointer-down is held.
      // The independent pointer edge must not become an early mouse-up.
      _session?.rfb.releaseKeyboard();
      _pressed.clear();
    };
    _keyboard.onCaptureLost = () {
      _release();
      _focus.unfocus();
    };
    _keyboard.addListener(_keyboardChanged);
    WidgetsBinding.instance.addObserver(this);
    widget.windowActions.bind(this, confirm: _requestClose, retire: _retire);
    _connect();
  }

  void _connect() {
    if (_closing) return;
    final previous = _session;
    previous?.removeListener(_changed);
    previous?.dispose();
    _release();
    _focus.unfocus();
    _paintedSession = null;
    _paintedSize = null;
    _image?.dispose();
    _cursorImage?.dispose();
    _image = null;
    _cursorImage = null;
    _imageRevision = -1;
    _cursorRevision = -1;
    _session = widget.create()..addListener(_changed);
    if (!kIsWeb && defaultTargetPlatform == TargetPlatform.windows) {
      _session!.requestNativeKeyboardProtocol();
    }
    // Reserve the view's native mode without capturing any keyboard yet.
    // The independent view fence outlives focus-specific hook leases.
    unawaited(_keyboard.acquire(++_inputGeneration));
    unawaited(_session!.start());
  }

  void _changed() {
    if (!mounted || _closing) return;
    if (_session?.closed == true ||
        (_paintedSession != null && !_pictureCurrent)) {
      _paintedSession = null;
      _paintedSize = null;
      _release();
      _focus.unfocus();
    }
    setState(() {});
    if (!_decodeScheduled && !_decoding) {
      _decodeScheduled = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _decodeScheduled = false;
        if (mounted && !_closing) unawaited(_decode());
      });
    }
  }

  Future<ui.Image> _rgba(Uint8List rgba, int width, int height) {
    final decoder = widget.decodeRgba;
    if (decoder != null) return decoder(rgba, width, height);
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

  Future<void> _decode() async {
    final session = _session;
    if (session == null || session.closed || _decoding) return;
    final rfb = session.rfb;
    _decoding = true;
    try {
      if (rfb.frameValid && _imageRevision != rfb.frameRevision) {
        final revision = rfb.frameRevision;
        final pixels = rfb.pixels, width = rfb.width, height = rfb.height;
        final image = await _rgba(Uint8List.fromList(pixels), width, height);
        if (!mounted ||
            _closing ||
            !identical(session, _session) ||
            session.closed ||
            !rfb.frameValid ||
            !identical(pixels, rfb.pixels) ||
            width != rfb.width ||
            height != rfb.height) {
          // Even a same-size DesktopSize replaces the source buffer. A late
          // decode from that old picture must never open its paint/input fence.
          image.dispose();
          return;
        }
        final old = _image;
        setState(() {
          _image = image;
          _imageRevision = revision;
        });
        WidgetsBinding.instance.addPostFrameCallback((_) {
          old?.dispose();
          // Framework image/paint fence only, not a GPU scanout assertion.
          if (mounted &&
              !_closing &&
              identical(session, _session) &&
              identical(image, _image) &&
              session.connected &&
              image.width == rfb.width &&
              image.height == rfb.height) {
            setState(() {
              _paintedSession = session;
              _paintedSize = Size(
                image.width.toDouble(),
                image.height.toDouble(),
              );
            });
          }
        });
      }
      if (_cursorRevision != rfb.cursorRevision) {
        final cursor = rfb.cursor;
        final revision = rfb.cursorRevision;
        final image = cursor == null || cursor.hidden
            ? null
            : await _rgba(cursor.rgba, cursor.width, cursor.height);
        if (!mounted ||
            _closing ||
            !identical(session, _session) ||
            session.closed) {
          image?.dispose();
          return;
        }
        final old = _cursorImage;
        setState(() {
          _cursorImage = image;
          _cursorRevision = revision;
        });
        WidgetsBinding.instance.addPostFrameCallback((_) => old?.dispose());
      }
    } finally {
      _decoding = false;
      final current = _session?.rfb;
      if (mounted &&
          !_closing &&
          current != null &&
          ((current.frameValid && _imageRevision != current.frameRevision) ||
              _cursorRevision != current.cursorRevision)) {
        _changed();
      }
    }
  }

  void _keyboardChanged() {
    if (_keyboard.supported && _pressed.isNotEmpty) {
      for (final symbol in _pressed.values.toSet()) {
        _session?.rfb.key(symbol, false);
      }
      _pressed.clear();
    }
    if (mounted && !_closing) setState(() {});
  }

  Future<void> _activateKeyboard() async {
    final session = _session;
    if (!_pictureReady ||
        !_focus.hasFocus ||
        session?.connected != true ||
        _keyboard.captured ||
        _activatingKeyboard) {
      return;
    }
    _activatingKeyboard = true;
    final intent = ++_inputIntent, generation = ++_inputGeneration;
    try {
      final claimed = await _keyboard.acquire(generation);
      if (!mounted ||
          _closing ||
          _confirming ||
          !_pictureReady ||
          intent != _inputIntent ||
          !_focus.hasFocus ||
          !identical(session, _session) ||
          session!.closed) {
        return;
      }
      final captured = claimed && await _keyboard.capture(true);
      if (mounted && !_closing && intent == _inputIntent) {
        setState(() => _keyboardFailure = !captured);
      }
    } finally {
      if (intent == _inputIntent) _activatingKeyboard = false;
    }
  }

  void _release() {
    ++_inputIntent;
    _activatingKeyboard = false;
    _session?.rfb.releaseInput();
    _pressed.clear();
    _picturePressed = false;
    _pointerPacer.reset();
    unawaited(_keyboard.release());
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state != AppLifecycleState.resumed) {
      _release();
      _focus.unfocus();
    }
  }

  KeyEventResult _key(FocusNode _, KeyEvent event) {
    if (!_pictureReady || _session?.connected != true || !_focus.hasFocus) {
      return KeyEventResult.ignored;
    }
    if (_keyboard.supported || _activatingKeyboard) {
      return KeyEventResult
          .handled; // Hook is the sole producer; no duplicate key.
    }
    final up = event is KeyUpEvent;
    final keysym = up
        ? _pressed.remove(event.physicalKey)
        : _pressed[event.physicalKey] ?? consoleKeysym(event.logicalKey);
    if (keysym == null) return KeyEventResult.ignored;
    if (!up) _pressed[event.physicalKey] = keysym;
    _session!.rfb.key(keysym, !up);
    return KeyEventResult.handled;
  }

  Future<void> _requestClose() async {
    if (_closing || _confirming) return;
    _release();
    _focus.unfocus();
    _confirming = true;
    try {
      final connected = _session?.connected == true;
      if (connected &&
          !await showAppConfirm(
            context,
            icon: Icons.link_off_rounded,
            title: l10n.disconnectConsole2,
            subtitle: widget.name,
            message: l10n.onlyConsoleConnectionEndsDesktop,
            confirmLabel: l10n.disconnect,
          )) {
        return;
      }
      if (mounted) await _retire();
    } finally {
      _confirming = false;
    }
  }

  Future<void> _retire() async {
    if (_closing) return;
    _closing = true;
    _release();
    _focus.unfocus();
    await _session?.close();
    await widget.windowActions.closeWindow();
  }

  void _attention() {
    if (!_pictureReady) return;
    _release();
    final rfb = _session?.rfb;
    if (rfb == null || !(_session?.connected ?? false)) return;
    for (final key in [0xffe3, 0xffe9, 0xffff]) {
      rfb.key(key, true);
    }
    for (final key in [0xffff, 0xffe9, 0xffe3]) {
      rfb.key(key, false);
    }
    _focus.requestFocus();
  }

  @override
  void dispose() {
    _closing = true;
    _release();
    widget.windowActions.unbind(this);
    WidgetsBinding.instance.removeObserver(this);
    _session?.removeListener(_changed);
    _session?.dispose();
    _keyboard.removeListener(_keyboardChanged);
    _keyboard.dispose();
    _pointerPacer.dispose();
    _focus.dispose();
    _image?.dispose();
    _cursorImage?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final session = _session;
    return Scaffold(
      backgroundColor: AppPalette.stage,
      body: Stack(
        fit: StackFit.expand,
        children: [
          if (session == null ||
              session.closed ||
              _image == null ||
              !session.rfb.frameValid)
            SessionStatusPanel(
              machine: widget.name,
              title: 'Console',
              busy: session?.closed != true,
              message: session?.closed == true
                  ? consoleFailureMessage(session?.failureCode)
                  : l10n.waitingConsoleScreenMayNot,
              onBack: () => unawaited(_requestClose()),
              backLabel: l10n.closeRemoteWindow,
              onRetry: session?.canRetry == true
                  ? () => setState(_connect)
                  : null,
            )
          else
            LayoutBuilder(
              builder: (context, constraints) {
                final image = _image!;
                final dpr = MediaQuery.devicePixelRatioOf(context);
                final scale = consolePictureScale(
                  constraints.biggest,
                  Size(image.width.toDouble(), image.height.toDouble()),
                  dpr,
                  original: _original,
                );
                if (scale <= 0) return const SizedBox.shrink();
                final size = Size(image.width * scale, image.height * scale);
                final geometry = (
                  scale,
                  constraints.maxWidth,
                  constraints.maxHeight,
                );
                if (_pointerGeometry != geometry) {
                  _pointerPacer.reset();
                  _pointerGeometry = geometry;
                }
                final picture = SizedBox(
                  key: const Key('console-picture'),
                  width: size.width,
                  height: size.height,
                  child: _picture(
                    session,
                    scale,
                    scale * dpr < 0.999999
                        ? FilterQuality.medium
                        : FilterQuality.none,
                  ),
                );
                return _original
                    ? SingleChildScrollView(
                        scrollDirection: Axis.horizontal,
                        child: SingleChildScrollView(child: picture),
                      )
                    : Stack(
                        children: [
                          Positioned(
                            left:
                                (((constraints.maxWidth - size.width) / 2) *
                                        dpr)
                                    .floorToDouble() /
                                dpr,
                            top:
                                (((constraints.maxHeight - size.height) / 2) *
                                        dpr)
                                    .floorToDouble() /
                                dpr,
                            width: size.width,
                            height: size.height,
                            child: picture,
                          ),
                        ],
                      );
              },
            ),
          SessionToolbar(
            title: widget.name,
            modeLabel: 'Console',
            connected: session?.connected == true,
            pointerHeld: _picturePressed,
            inputCaptured: _pictureReady && _focus.hasFocus,
            onInteract: () {
              _release();
              _focus.unfocus();
            },
            onDisconnect: () => unawaited(_requestClose()),
            contentKey: session,
            actions: [
              SessionToolbarMenu.command(
                key: const Key('console-fit'),
                label: _original ? l10n.fitWindow : l10n.originalSize,
                icon: Icons.fit_screen_rounded,
                onInteract: _release,
                onPressed: () => setState(() => _original = !_original),
              ),
              SessionToolbarMenu.command(
                key: const Key('console-secure-attention'),
                label: l10n.sendCtrlAltDelete,
                icon: Icons.keyboard_rounded,
                onInteract: _release,
                onPressed: _pictureReady ? _attention : null,
              ),
              SessionToolbarMenu.command(
                key: const Key('console-info'),
                label: l10n.aboutConsole,
                icon: Icons.info_outline_rounded,
                onInteract: _release,
                onPressed: () => unawaited(
                  showAppNotice(
                    context,
                    title: 'Console',
                    message:
                        l10n.emergencyProxmoxConsoleScreenOff(_keyboardFailure ? l10n.couldNotConnectKeyboardInput : ''),
                  ),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _picture(
    EmergencyConsoleSession session,
    double scale,
    FilterQuality sampling,
  ) {
    final cursor = session.rfb.cursor;
    final hidden = cursor?.hidden == true;
    final actualCursor =
        hidden ||
        (_cursorImage != null && _cursorRevision == session.rfb.cursorRevision);
    void pointer(PointerEvent event, {int? pulse}) {
      if (!_pictureReady || !identical(session, _session)) return;
      _pointer = event.localPosition;
      if (!_focus.hasFocus && event is! PointerDownEvent) {
        setState(() {});
        return;
      }
      final buttons =
          ((event.buttons & kPrimaryMouseButton) != 0 ? 1 : 0) |
          ((event.buttons & kMiddleMouseButton) != 0 ? 2 : 0) |
          ((event.buttons & kSecondaryMouseButton) != 0 ? 4 : 0);
      final x = (_pointer.dx / scale).floor(),
          y = (_pointer.dy / scale).floor();
      if (event is PointerMoveEvent || event is PointerHoverEvent) {
        _pointerPacer.motion(x, y, buttons);
      } else {
        // First click may establish Focus only after this dispatch. The edge
        // itself is current-picture input, while deferred motion needs focus.
        ++_pointerEdgeDepth;
        try {
          _pointerPacer.edge(x, y, buttons | (pulse ?? 0));
          if (pulse != null) _pointerPacer.edge(x, y, buttons);
        } finally {
          --_pointerEdgeDepth;
        }
      }
      setState(() {});
    }

    return Focus(
      focusNode: _focus,
      onKeyEvent: _key,
      onFocusChange: (focused) {
        if (!focused) {
          _release();
        } else {
          unawaited(_activateKeyboard());
        }
      },
      child: MouseRegion(
        cursor: actualCursor
            ? SystemMouseCursors.none
            : SystemMouseCursors.basic,
        onEnter: (_) => setState(() => _hovered = true),
        onExit: (_) => setState(() => _hovered = false),
        child: Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (event) {
            _focus.requestFocus();
            _picturePressed = true;
            pointer(event);
            unawaited(_activateKeyboard());
          },
          onPointerUp: (event) {
            _picturePressed = event.buttons != 0;
            pointer(event);
          },
          onPointerMove: pointer,
          onPointerHover: pointer,
          onPointerCancel: (_) => _release(),
          onPointerSignal: (event) {
            if (event is PointerScrollEvent) {
              pointer(
                event,
                pulse: event.scrollDelta.dy < 0
                    ? 8
                    : event.scrollDelta.dy > 0
                    ? 16
                    : event.scrollDelta.dx < 0
                    ? 32
                    : 64,
              );
            }
          },
          child: ClipRect(
            child: Stack(
              fit: StackFit.expand,
              children: [
                RawImage(
                  image: _image,
                  fit: BoxFit.fill,
                  filterQuality: sampling,
                ),
                if (_hovered &&
                    !hidden &&
                    actualCursor &&
                    cursor != null &&
                    _cursorImage != null)
                  Positioned(
                    left: _pointer.dx - cursor.hotX * scale,
                    top: _pointer.dy - cursor.hotY * scale,
                    width: cursor.width * scale,
                    height: cursor.height * scale,
                    child: IgnorePointer(
                      child: RawImage(
                        image: _cursorImage,
                        filterQuality: sampling,
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

int? consoleKeysym(LogicalKeyboardKey key) {
  final mapped = <LogicalKeyboardKey, int>{
    LogicalKeyboardKey.backspace: 0xff08,
    LogicalKeyboardKey.tab: 0xff09,
    LogicalKeyboardKey.enter: 0xff0d,
    LogicalKeyboardKey.numpadEnter: 0xff0d,
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
    LogicalKeyboardKey.numLock: 0xff7f,
    LogicalKeyboardKey.scrollLock: 0xff14,
    LogicalKeyboardKey.hangulMode: 0xff31,
    LogicalKeyboardKey.lang1: 0xff31,
    LogicalKeyboardKey.hanjaMode: 0xff34,
    LogicalKeyboardKey.lang2: 0xff34,
    LogicalKeyboardKey.f1: 0xffbe,
    LogicalKeyboardKey.f2: 0xffbf,
    LogicalKeyboardKey.f3: 0xffc0,
    LogicalKeyboardKey.f4: 0xffc1,
    LogicalKeyboardKey.f5: 0xffc2,
    LogicalKeyboardKey.f6: 0xffc3,
    LogicalKeyboardKey.f7: 0xffc4,
    LogicalKeyboardKey.f8: 0xffc5,
    LogicalKeyboardKey.f9: 0xffc6,
    LogicalKeyboardKey.f10: 0xffc7,
    LogicalKeyboardKey.f11: 0xffc8,
    LogicalKeyboardKey.f12: 0xffc9,
  }[key];
  if (mapped != null) return mapped;
  final label = key.keyLabel;
  if (label.runes.length != 1) return null;
  var rune = label.runes.single;
  if (rune >= 65 && rune <= 90) rune += 32; // Shift state is sent separately.
  return rune < 256
      ? rune
      : rune <= 0x10ffff
      ? 0x01000000 | rune
      : null;
}

/// Original means one guest pixel per physical device pixel. Fit may shrink
/// that size but never silently magnifies a low-resolution console framebuffer.
double consolePictureScale(
  Size viewport,
  Size pixels,
  double dpr, {
  bool original = false,
}) {
  if (!dpr.isFinite ||
      dpr <= 0 ||
      !pixels.width.isFinite ||
      !pixels.height.isFinite ||
      pixels.width <= 0 ||
      pixels.height <= 0 ||
      !viewport.width.isFinite ||
      !viewport.height.isFinite ||
      viewport.width <= 0 ||
      viewport.height <= 0) {
    return 0;
  }
  final native = 1 / dpr;
  return original
      ? native
      : math.min(
          native,
          math.min(
            viewport.width / pixels.width,
            viewport.height / pixels.height,
          ),
        );
}
