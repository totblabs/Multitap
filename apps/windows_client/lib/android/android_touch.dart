import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/widgets.dart';
import '../l10n/l10n.dart';

/// How a finger drives the remote mouse (see [AndroidTouchInput]).
///
/// [trackpad] (포인터, the default): the finger moves a cursor like a laptop
/// touchpad. [direct] (터치): the finger touches the remote picture: real
/// Windows touch where the session has it, otherwise the mouse at the finger.
enum AndroidTouchMode {
  direct,
  trackpad;

  const AndroidTouchMode();
  String get label => switch (this) {
    AndroidTouchMode.direct => l10n.touch,
    AndroidTouchMode.trackpad => l10n.pointer,
  };

  static AndroidTouchMode parse(String? value) =>
      value == direct.name ? direct : trackpad;
}
/// Where the remote picture is drawn on this device. [fitted] is the whole
/// picture at 100 % inside [area] (BoxFit.contain); [rect] is where it is
/// drawn now, [scale] ≥ 1 times [fitted]. Only this device's presentation
/// changes: zooming never changes the remote resolution.
@immutable
class AndroidPictureView {
  const AndroidPictureView({
    required this.area,
    required this.fitted,
    this.scale = 1,
    Rect? rect,
  }) : rect = rect ?? fitted;

  static const maxScale = 5.0;

  /// [source] fitted into [area], centred, at 100 %. With [pixelScale] (≥ 1)
  /// the picture is drawn at exactly that many device pixels per remote
  /// pixel when it fits (sharp, no fractional resampling); otherwise it is
  /// contained. The position is snapped to device pixels.
  factory AndroidPictureView.fit(
    Rect area,
    Size source, {
    double devicePixelRatio = 1,
    int pixelScale = 0,
  }) {
    if (source.isEmpty || area.isEmpty) {
      return AndroidPictureView(area: area, fitted: area);
    }
    final dpr = devicePixelRatio.isFinite && devicePixelRatio > 0
        ? devicePixelRatio
        : 1.0;
    var size = applyBoxFit(BoxFit.contain, source, area.size).destination;
    if (pixelScale > 0) {
      final exact = source * (pixelScale / dpr);
      // Exact only when the picture really is this screen's pixels: one the
      // encoder sent smaller (its box or pixel budget) is scaled up to fill,
      // never shown 1:1 in the middle of the screen (Fold 8 unfolded,
      // 2026-10-26: 1248×942 of a 2448×1848 output used a quarter of it).
      final fills =
          exact.width >= size.width - 2 / dpr ||
          exact.height >= size.height - 2 / dpr;
      if (fills &&
          exact.width <= area.width + .01 &&
          exact.height <= area.height + .01) {
        size = exact;
      }
    }
    double snap(double value) => (value * dpr).roundToDouble() / dpr;
    return AndroidPictureView(
      area: area,
      fitted:
          Offset(
            snap(area.left + (area.width - size.width) / 2),
            snap(area.top + (area.height - size.height) / 2),
          ) &
          size,
    );
  }

  /// Device pixels per remote pixel when this is a whole number at rest
  /// (drawn without smoothing), else null.
  int? wholePixelScale(Size source, double devicePixelRatio) {
    if (zoomed || source.isEmpty || rect.topLeft != fitted.topLeft) return null;
    final w = rect.width * devicePixelRatio, h = rect.height * devicePixelRatio;
    final k = (w / source.width).round();
    if (k < 1 ||
        (w - source.width * k).abs() > .5 ||
        (h - source.height * k).abs() > .5) {
      return null;
    }
    return k;
  }

  final Rect area, fitted, rect;
  final double scale;

  bool get zoomed => scale > 1.001;

  /// The picture is not where it rests (zoomed in or moved above the
  /// on-screen keyboard), or part of it is under the keyboard, so a
  /// one-finger drag can move it.
  bool movable(Rect visible) =>
      zoomed ||
      (rect.topLeft - fitted.topLeft).distance > .5 ||
      rect.bottom > visible.bottom + .5;

  /// Part of the picture cannot be seen in [visible] (zoomed in, or taller
  /// than the room above the keyboard), so 이동 has something to show
  /// (ToTBDesk TOOLBAR-2.3.3: nothing to move at 전체 보기 without the
  /// keyboard).
  bool canPan(Rect visible) =>
      rect.width > visible.width + .5 || rect.height > visible.height + .5;

  /// The picture point under [position], 0..1; null outside the picture.
  Offset? normalize(Offset position) {
    if (rect.isEmpty || !rect.inflate(.5).contains(position)) return null;
    return clampNormal(
      Offset(
        (position.dx - rect.left) / rect.width,
        (position.dy - rect.top) / rect.height,
      ),
    );
  }

  /// The picture point under [position], clamped to its edge (a drag that
  /// leaves the picture keeps its button at the edge).
  Offset normalizeClamped(Offset position) => rect.isEmpty
      ? const Offset(.5, .5)
      : clampNormal(
          Offset(
            (position.dx - rect.left) / rect.width,
            (position.dy - rect.top) / rect.height,
          ),
        );

  static Offset clampNormal(Offset value) =>
      Offset(value.dx.clamp(0.0, 1.0), value.dy.clamp(0.0, 1.0));

  /// Where [normalized] is drawn on this device.
  Offset point(Offset normalized) =>
      rect.topLeft +
      Offset(normalized.dx * rect.width, normalized.dy * rect.height);

  AndroidPictureView _at(double scale, Offset topLeft, Rect visible) {
    final size = fitted.size * scale;
    double axis(
      double value,
      double length,
      double start,
      double end,
      double visibleStart,
      double visibleEnd,
      double rest,
    ) {
      // Larger than the screen: any part can be brought into view. Smaller:
      // it rests centred, and may only move up to clear the keyboard (as
      // far as centred in the room above it).
      final upper = length <= end - start ? rest : start;
      var lower = math.min(upper, visibleEnd - length);
      if (length <= visibleEnd - visibleStart) {
        lower = math.min(
          lower,
          visibleStart + (visibleEnd - visibleStart - length) / 2,
        );
      }
      return value.clamp(lower, upper);
    }

    // At 100 % the picture rests where [fit] put it (pixel-snapped).
    final resting = scale == 1 && size == fitted.size;
    final left = axis(
      topLeft.dx,
      size.width,
      area.left,
      area.right,
      visible.left,
      visible.right,
      resting ? fitted.left : area.left + (area.width - size.width) / 2,
    );
    final top = axis(
      topLeft.dy,
      size.height,
      area.top,
      area.bottom,
      visible.top,
      visible.bottom,
      resting ? fitted.top : area.top + (area.height - size.height) / 2,
    );
    return AndroidPictureView(
      area: area,
      fitted: fitted,
      scale: scale,
      rect: Offset(left, top) & size,
    );
  }

  /// Moved by [delta], kept inside the screen ([visible] excludes the
  /// keyboard and its key row).
  AndroidPictureView panned(Offset delta, {Rect? visible}) =>
      _at(scale, rect.topLeft + delta, visible ?? area);

  /// Zoomed to [next] so that the picture point [anchor] (0..1, may lie
  /// outside) is drawn at [focal].
  AndroidPictureView zoomTo(
    double next,
    Offset anchor,
    Offset focal, {
    Rect? visible,
  }) {
    final s = next.clamp(1.0, maxScale);
    final size = fitted.size * s;
    return _at(
      s,
      focal - Offset(anchor.dx * size.width, anchor.dy * size.height),
      visible ?? area,
    );
  }

  /// The picture point under [position] without clamping (pinch anchor).
  Offset anchorAt(Offset position) => rect.isEmpty
      ? const Offset(.5, .5)
      : Offset(
          (position.dx - rect.left) / rect.width,
          (position.dy - rect.top) / rect.height,
        );

  /// Back to 100 %, resting centred (or above the keyboard).
  AndroidPictureView reset({Rect? visible}) =>
      _at(1, fitted.topLeft, visible ?? area);

  /// Moves just enough that [normalized] lies inside [visible] with
  /// [margin] around it (the cursor or typing point stays in view).
  AndroidPictureView reveal(
    Offset normalized,
    Rect visible, {
    double margin = 40,
  }) {
    final p = point(normalized);
    final inner = visible.deflate(
      math.min(margin, math.min(visible.width, visible.height) / 3),
    );
    var dx = 0.0, dy = 0.0;
    if (p.dx < inner.left) dx = inner.left - p.dx;
    if (p.dx > inner.right) dx = inner.right - p.dx;
    if (p.dy < inner.top) dy = inner.top - p.dy;
    if (p.dy > inner.bottom) dy = inner.bottom - p.dy;
    return _at(scale, rect.topLeft + Offset(dx, dy), visible);
  }

  /// [fractions] (picture fractions: left, top, right, bottom) as drawn on
  /// this device.
  Rect fractionRect(Rect fractions) =>
      Rect.fromPoints(point(fractions.topLeft), point(fractions.bottomRight));

  /// Moves just enough that the picture rectangle [fractions] (0..1) lies in
  /// [visible] with a margin around it (the caret or a tapped point stays in
  /// view above the keyboard), unchanged while it is inside that band. With
  /// [recenter] a caret that left the band is brought to the middle of the
  /// room vertically (and well inside horizontally), so the picture does not
  /// move again for every following line or character.
  AndroidPictureView revealRect(
    Rect fractions,
    Rect visible, {
    bool recenter = false,
    double vMargin = 56,
    double hMargin = 24,
  }) {
    final r = fractionRect(fractions);
    final inner = Rect.fromLTRB(
      visible.left + math.min(hMargin, visible.width / 3),
      visible.top + math.min(vMargin, visible.height / 3),
      visible.right - math.min(hMargin, visible.width / 3),
      visible.bottom - math.min(vMargin, visible.height / 3),
    );
    var dx = 0.0, dy = 0.0;
    if (r.bottom > inner.bottom || r.top < inner.top) {
      dy = recenter
          ? visible.center.dy - r.center.dy
          : (r.top < inner.top ? inner.top - r.top : inner.bottom - r.bottom);
      // A tall rectangle keeps its top in view.
      if (r.height > inner.height) dy = inner.top - r.top;
    }
    if (r.right > inner.right || r.left < inner.left) {
      final overshoot = recenter ? visible.width * .25 : 0.0;
      dx = r.left < inner.left
          ? inner.left - r.left + overshoot
          : inner.right - r.right - overshoot;
      if (r.width > inner.width) dx = inner.left - r.left;
    }
    if (dx == 0 && dy == 0) return _at(scale, rect.topLeft, visible);
    return _at(scale, rect.topLeft + Offset(dx, dy), visible);
  }

  /// This view with the phone keyboard (and its key row) covering the area
  /// below [visible] (ToTBDesk KEYBOARD-2.3.2): the zoom is kept and the
  /// picture moves up by what the keyboard covers, but never so far that
  /// its bottom edge leaves the bottom of [visible]; a picture that fits
  /// above the keyboard is centred there.
  AndroidPictureView lifted(Rect visible) {
    final covered = area.bottom - visible.bottom;
    if (covered <= .5) return _at(scale, rect.topLeft, visible);
    final height = rect.height;
    final top = height <= visible.height
        ? visible.top + (visible.height - height) / 2
        : math.max(visible.bottom - height, rect.top - covered);
    return _at(scale, Offset(rect.left, top), visible);
  }

  @override
  bool operator ==(Object other) =>
      other is AndroidPictureView &&
      other.area == area &&
      other.fitted == fitted &&
      other.rect == rect &&
      other.scale == scale;

  @override
  int get hashCode => Object.hash(area, fitted, rect, scale);
}

/// Where the picture goes while the phone keyboard is open, for any remote
/// view that lays out an [AndroidPictureView] above the keyboard (ToTBDesk
/// KEYBOARD-2.3.2, TOOLBAR-2.3.3, ZOOM-2.3.1):
///
/// * Opening: the picture keeps its zoom and moves up by what the keyboard
///   and its key row cover ([AndroidPictureView.lifted]); the point being
///   typed at stays in view. While the keyboard slides in or changes height
///   the placement follows it.
/// * Following what is typed: with a Host that reports the text caret
///   (caret-v1, [caret]) the picture keeps the caret in view above the
///   keyboard as it moves (down a line, along a long line, into another
///   field); it moves only when the caret leaves a comfort band, then
///   re-centres, so it does not jitter line by line. Without a caret (an
///   older Host, an application that shows none) a tap while the
///   keyboard is open is followed instead ([tapped]).
/// * While it is open the picture can still be moved (move, pinch, the
///   cursor at the edge); after a move by hand ([manual]) the cursor no
///   longer pulls it back and caret following is suspended until the caret
///   next jumps out of view ([caret]) or a field is tapped ([tapped]).
/// * Closing: the picture returns exactly where it was before the keyboard
///   opened, unless it was zoomed meanwhile (that zoom is kept, inside the
///   screen).
class AndroidKeyboardCamera {
  AndroidPictureView? _rest;
  Offset _typing = const Offset(.5, .5);
  // The Host's caret rectangle (picture fractions) and, for the fallback, a
  // tapped point not yet revealed once.
  Rect? _caret;
  Offset? _tap;
  Rect? _lastVisible;
  bool _settled = false, _manual = false, _following = true, _jumped = false;

  /// A caret that moves by more than these fractions of the picture in one
  /// report is a jump (another field), not typing.
  static const _jumpY = .05, _jumpX = .15;

  /// Something covers the bottom of the picture area (keyboard, key row).
  bool get open => _rest != null;

  /// Moved by hand (move, pinch, zoom buttons) since the keyboard opened.
  bool get manual => _manual;

  /// The caret (or a tapped point) pulls the picture; false after a move by
  /// hand until a clear caret jump out of view or a tap.
  bool get following => _following;

  /// The Host's caret in picture fractions, null without a caret.
  Rect? get caretFraction => _caret;

  /// The picture moved after the keyboard opened: following the cursor,
  /// or [byHand]. It is then only kept inside the screen, no longer placed
  /// for typing.
  void moved({bool byHand = false}) {
    if (_rest == null) return;
    _settled = true;
    if (byHand) {
      _manual = true;
      _following = false;
      _jumped = false;
    }
  }

  /// A new connection or picture: nothing to return to.
  void reset() {
    _closeKeyboard();
    _caret = null;
  }

  void _closeKeyboard() {
    _rest = null;
    _tap = null;
    _lastVisible = null;
    _settled = _manual = _jumped = false;
    _following = true;
  }

  /// The Host's caret rectangle as fractions of the picture (left, top,
  /// right, bottom), or null: the focused application has none, or reports
  /// stopped. Call [AndroidKeyboardCamera.place] again afterwards.
  void caret(Rect? fractions) {
    final previous = _caret;
    _caret = fractions;
    if (fractions == null || _following || _rest == null) return;
    // Suspended by hand: only a clear jump may resume (checked in [place]
    // against what is visible).
    if (previous == null ||
        (fractions.center.dy - previous.center.dy).abs() > _jumpY ||
        (fractions.center.dx - previous.center.dx).abs() > _jumpX) {
      _jumped = true;
    }
  }

  /// A tap on the picture point [point] (0..1) while the keyboard is open:
  /// the user chose a field (or somewhere to type). The picture reveals it
  /// and follows again; the caret of the previous field is stale until the
  /// Host reports the new one.
  void tapped(Offset point) {
    if (_rest == null) return;
    _typing = point;
    _tap = point;
    _caret = null;
    _following = true;
    _manual = false;
    _jumped = false;
  }

  Rect _target() => _caret ?? Rect.fromPoints(_typing, _typing);

  /// [view] laid out for [visible] (the area above the keyboard and its key
  /// row; the whole area when they are closed). [typing]: the picture point
  /// (0..1) being typed at, taken when the keyboard opens. [fresh]: [view]
  /// was just fitted anew (a new picture size or screen), so it is where the
  /// picture now rests.
  AndroidPictureView place(
    AndroidPictureView view,
    Rect visible,
    Offset typing, {
    bool fresh = false,
  }) {
    final covered = visible.bottom < view.area.bottom - .5;
    if (!covered) {
      final rest = _rest;
      _closeKeyboard();
      if (rest != null &&
          rest.area == view.area &&
          rest.fitted == view.fitted &&
          (rest.scale - view.scale).abs() < 1e-6) {
        return rest.panned(Offset.zero, visible: visible);
      }
      return view.panned(Offset.zero, visible: visible);
    }
    if (_rest == null || fresh) {
      if (_rest == null) _typing = typing;
      _rest = view;
      _settled = _manual = _jumped = false;
      _following = true;
      _lastVisible = null;
    }
    // Moved by hand: a clear caret jump out of the visible picture resumes.
    final caret = _caret;
    if (!_following && _jumped && caret != null) {
      final shown = view.fractionRect(caret);
      if (!visible.overlaps(shown)) _following = true;
    }
    _jumped = false;
    final same = _lastVisible == visible;
    _lastVisible = visible;
    if (!same && !_settled) {
      // The keyboard opened, slid or changed height: place from the rest.
      return _rest!.lifted(visible).revealRect(_target(), visible);
    }
    final next = view.panned(Offset.zero, visible: visible);
    if (!_following) return next;
    final tap = _tap;
    if (_caret != null) {
      return next.revealRect(_caret!, visible, recenter: true);
    }
    if (tap != null) {
      _tap = null;
      return next.revealRect(Rect.fromPoints(tap, tap), visible);
    }
    return next;
  }

  /// the whole picture at rest; with the keyboard open it is
  /// placed for typing again (ToTBDesk: fitting resets the keyboard camera).
  AndroidPictureView fit(AndroidPictureView view, Rect visible, Offset typing) {
    final rest = view.reset();
    if (visible.bottom >= view.area.bottom - .5) return rest;
    _rest = rest;
    _typing = typing;
    _tap = null;
    _settled = _manual = _jumped = false;
    _following = true;
    _lastVisible = visible;
    return rest.lifted(visible).revealRect(_target(), visible);
  }
}

/// What the gesture did, for this device's own feedback (haptics, a ring
/// where the click lands). Nothing here reaches the remote.
enum AndroidTouchCue {
  /// A tap was recognised (felt at once; its click follows after the
  /// double-tap window).
  tap,

  /// A left click was sent (counted; already felt as [tap]).
  click,
  rightClick,

  /// A still press began; held for the long-press time it right-clicks
  /// ([rightClick]) unless [pressEnd] comes first.
  press,

  /// That press moved, lifted or met another finger before its right click.
  pressEnd,

  /// A real touch contact began (touch-input-v1); Windows interprets it.
  contact,

  /// A drag started by holding still after a tap (felt as such; counted as
  /// a drag start).
  hold,

  /// A drag started by moving after a tap.
  dragStart,
  dragEnd,
  keyboard,

  /// A still, quick four-finger tap: 포인터 ↔ 터치.
  touchMode,
  wheel,
  pinch,

  /// A pinch reached the whole picture (전체 보기) or the largest zoom: one
  /// tick, felt once per gesture (ToTBDesk ZOOM-2.3.1).
  zoomLimit,
}

/// How far the cursor goes per finger travel in pointer mode.
enum AndroidPointerSpeed {
  precise(.75, false),
  standard(1, false),
  fast(1.5, true);

  const AndroidPointerSpeed(this.gain, this.acceleration);
  String get label => switch (this) {
    AndroidPointerSpeed.precise => l10n.precise,
    AndroidPointerSpeed.standard => l10n.standard,
    AndroidPointerSpeed.fast => l10n.fast,
  };

  final double gain;

  /// Faster finger movement takes the cursor further (빠름).
  final bool acceleration;

  static AndroidPointerSpeed parse(String? value) =>
      values.firstWhere((s) => s.name == value, orElse: () => standard);
}

/// One real touch contact for a session that can inject Windows touch
/// (touch-input-v1). Positions are 0..1 over the remote picture.
enum AndroidContactPhase { down, move, up, cancel }

/// Real Windows touch contacts, where the session has them (touch-input-v1;
/// wired by the session owner). Without it, 터치 mode is mouse emulation.
abstract interface class AndroidTouchContacts {
  /// The session granted real touch input right now.
  bool get available;
  void contact(int id, Offset point, AndroidContactPhase phase);
}

enum _Phase {
  idle,
  pending,
  moving,
  held,
  dragPending,
  drag,
  two,
  twoScroll,
  pinch,
  three,
  four,
  native,
  ignore,
}

/// Turns finger gestures into remote mouse messages (`move`, `button`
/// left/right, positioned `wheel`: the messages the Windows client sends
/// too), real touch contacts where the session has them, and local zoom of
/// the picture. It knows nothing about a particular session or protocol
/// beyond those mouse messages, so a remote view can host it with its own
/// [AndroidPictureView].
///
/// The model is the one ToTBDesk users tuned and accepted (DRAG-2.3.10,
/// TUNING-2.4.0/2.4.5, WINDOWS-INPUT-2.3.0):
///
/// 포인터 ([AndroidTouchMode.trackpad], the default): one finger moves the
/// cursor (pointer speed 정밀/표준/빠름); a tap clicks at the cursor after the
/// double-tap window (~0.3 s, felt at once); a quick second tap is a double
/// click; tap then press again and hold or move = left drag, and the first
/// click is never sent (Windows would read a double click); long press or a
/// two-finger tap = right click; two fingers moving = wheel; spreading = zoom
/// of this picture (never below fit); three-finger tap = keyboard;
/// four-finger tap = 포인터 ↔ 터치.
///
/// 터치 ([AndroidTouchMode.direct]): real touch contacts where available;
/// otherwise mouse emulation at the finger: tap = click there (same window),
/// one-finger swipe = wheel, double tap and hold/move = left drag from the
/// second press, long press / two-finger tap = right click, two fingers =
/// wheel, spread = zoom, three-finger tap = keyboard, four-finger tap =
/// 포인터 ↔ 터치.
///
/// Every pressed button is released when its finger lifts or is cancelled.
class AndroidTouchInput {
  AndroidTouchInput({
    required this.view,
    required this.visible,
    required this.onView,
    required this.send,
    this.onCursor,
    this.onCue,
    this.mode = AndroidTouchMode.trackpad,
    this.speed = AndroidPointerSpeed.standard,
    this.source,
    this.contacts,
    this.follow,
    this.longPress = const Duration(milliseconds: 500),
    this.doubleTap = const Duration(milliseconds: 300),
    this.slop = 8,
  });

  /// The current presentation and the part of the screen not covered by the
  /// keyboard.
  final AndroidPictureView Function() view;
  final Rect Function() visible;
  final ValueChanged<AndroidPictureView> onView;

  /// One remote input message, in order. Moves are not coalesced here.
  final void Function(Map<String, dynamic> event) send;

  /// The remote pointer moved to this picture point (drawn cursor).
  final ValueChanged<Offset>? onCursor;

  /// [at]: the picture point (0..1) where the cue's click or contact lands,
  /// so feedback drawn there follows zoom and pan.
  final void Function(AndroidTouchCue cue, Offset? at)? onCue;
  AndroidTouchMode mode;
  AndroidPointerSpeed speed;

  /// The remote picture size in remote pixels (wheel step); optional.
  final Size? Function()? source;

  /// Real touch contacts of the current session, when it has them.
  final AndroidTouchContacts? Function()? contacts;

  /// Whether a zoomed (or lifted) picture follows the pointer-mode cursor
  /// to keep it in view; false after the picture was moved by hand above
  /// the keyboard ([AndroidKeyboardCamera.manual]). Null: always.
  final bool Function()? follow;
  final Duration longPress;

  /// How long a tap's click waits for a second press (Android's double-tap
  /// timeout); a second press inside it can become a drag.
  final Duration doubleTap;

  /// Movement (logical px) below which a touch is still a tap or hold.
  final double slop;

  static const _doubleTapRadius = 32.0;
  static const _directDoubleTapRadius = 24.0;
  static const _edge = 36.0;

  /// The remote pointer position (0..1) this device last put it at.
  Offset cursor = const Offset(.5, .5);

  _Phase _phase = _Phase.idle;
  final _pointers = <int, Offset>{};
  final _starts = <int, Offset>{};
  int? _first;
  Offset _start = Offset.zero, _last = Offset.zero;
  Duration _downAt = Duration.zero, _lastAt = Duration.zero;
  Offset? _press;
  Offset _tapCursor = const Offset(.5, .5);
  Timer? _hold, _dragTimer, _pendingClick;
  Offset? _pendingClickAt, _heldClickAt;
  Duration? _lastTapUp;
  Offset _lastTapScreen = Offset.zero;
  bool _twoTap = false, _threeTap = false, _fourTap = false, _button = false;
  bool _pinchAtFit = false, _pinchAtMax = false;
  Duration _twoAt = Duration.zero, _threeAt = Duration.zero;
  Duration _fourAt = Duration.zero;
  double _pinchDistance = 1, _pinchScale = 1, _pinchLast = 1;
  double _wheel = 0, _velocity = 0;
  Offset _pinchAnchor = Offset.zero, _centroid = Offset.zero;
  Offset _scrollRef = Offset.zero, _flingAt = const Offset(.5, .5);
  Duration _scrollAt = Duration.zero;
  Timer? _fling;
  final _native = <int>{};

  bool get active => _phase != _Phase.idle;

  /// The left button is held by a drag.
  bool get dragging => _button;

  /// A tap's click is waiting out the double-tap window.
  bool get clickPending => _pendingClick != null;

  /// A two-finger flick is still scrolling.
  bool get flinging => _fling != null;

  bool get _direct => mode == AndroidTouchMode.direct;

  // --- output ---------------------------------------------------------------

  void _move(Offset point) {
    cursor = point;
    onCursor?.call(point);
    send({'type': 'input', 'event': 'move', 'x': point.dx, 'y': point.dy});
  }

  void _buttonEvent(String button, bool down, Offset point) {
    cursor = point;
    onCursor?.call(point);
    send({
      'type': 'input',
      'event': 'button',
      'button': button,
      'down': down,
      'x': point.dx,
      'y': point.dy,
    });
  }

  void _click(String button, Offset point) {
    _buttonEvent(button, true, point);
    _buttonEvent(button, false, point);
  }

  /// Sends a tap's click that is still waiting (another input follows: a
  /// key, a toolbar command, a hardware mouse), so it lands first.
  void flushPendingClick() {
    final at = _pendingClickAt;
    _pendingClick?.cancel();
    _pendingClick = null;
    _pendingClickAt = null;
    if (at == null) return;
    _click('left', at);
    onCue?.call(AndroidTouchCue.click, null);
  }

  void _discardPendingClick() {
    _pendingClick?.cancel();
    _pendingClick = null;
    _pendingClickAt = null;
    _lastTapUp = null;
  }

  /// Forgets the gesture and a waiting click without sending anything (focus
  /// loss, a menu, a mode or size change, a new session). The session's own
  /// input reset releases the remote side.
  void reset() {
    _stopFling();
    _cancelHold();
    _dragTimer?.cancel();
    _dragTimer = null;
    _discardPendingClick();
    _heldClickAt = null;
    _cancelNative();
    _pointers.clear();
    _starts.clear();
    _first = null;
    _press = null;
    _button = false;
    _wheel = 0;
    _phase = _Phase.idle;
  }

  void dispose() => reset();

  // --- helpers ----------------------------------------------------------------

  /// Stops a long press that has not right-clicked yet.
  void _cancelHold() {
    final hold = _hold;
    _hold = null;
    if (hold == null || !hold.isActive) return;
    hold.cancel();
    onCue?.call(AndroidTouchCue.pressEnd, null);
  }

  Offset _centre() {
    var sum = Offset.zero;
    for (final p in _pointers.values) {
      sum += p;
    }
    return sum / _pointers.length.toDouble();
  }

  double _spread() {
    final points = _pointers.values.take(2).toList();
    return points.length < 2 ? 1 : (points[0] - points[1]).distance;
  }

  bool _still() {
    for (final entry in _pointers.entries) {
      final start = _starts[entry.key];
      if (start != null && (entry.value - start).distance > slop) return false;
    }
    return true;
  }

  /// Finger travel for one wheel notch: what 60 remote pixels (about three
  /// lines) measure on screen at the current zoom, 12–96 dp.
  double get _notch {
    final size = source?.call(), rect = view().rect;
    if (size == null || size.width <= 0 || rect.isEmpty) return 40;
    return (60 * rect.width / size.width).clamp(12.0, 96.0);
  }

  void _wheelBy(double dy, Offset at) {
    // Quarter notches (30) as the finger goes: the page follows the finger.
    _wheel += dy / _notch * 120;
    if (_wheel.abs() < 30) return;
    final delta = (_wheel ~/ 30 * 30).clamp(-1200, 1200);
    _wheel -= delta;
    cursor = at;
    onCursor?.call(at);
    onCue?.call(AndroidTouchCue.wheel, null);
    send({
      'type': 'input',
      'event': 'wheel',
      'delta': delta,
      'x': at.dx,
      'y': at.dy,
    });
  }

  void _scrollTo(Offset position, Duration time) {
    final dy = position.dy - _scrollRef.dy;
    _scrollRef = position;
    final ms = (time - _scrollAt).inMicroseconds / 1000.0;
    if (ms > 0) {
      _velocity = _velocity * .4 + dy / ms * .6;
      _scrollAt = time;
    }
    _flingAt = _direct ? view().normalizeClamped(position) : cursor;
    _wheelBy(dy, _flingAt);
  }

  /// After a quick flick the page keeps moving and slows down, like the
  /// phone's own lists; any new touch stops it.
  void _startFling(Duration liftAt) {
    final velocity = _velocity;
    _velocity = 0;
    if (velocity.abs() < .3 ||
        liftAt - _scrollAt > const Duration(milliseconds: 80)) {
      return;
    }
    var speed = velocity.clamp(-6.0, 6.0);
    final at = _flingAt;
    var ticks = 0;
    _fling = Timer.periodic(const Duration(milliseconds: 16), (_) {
      speed *= .92;
      if (speed.abs() < .05 || ++ticks > 90) {
        _stopFling();
        return;
      }
      _wheelBy(speed * 16, at);
    });
  }

  void _stopFling() {
    _fling?.cancel();
    _fling = null;
  }

  void _show(AndroidPictureView current, AndroidPictureView next) {
    if (next != current) onView(next);
  }

  /// Pointer mode: relative cursor movement at the chosen pointer speed.
  void _follow(Offset position, Duration time) {
    final delta = position - _last;
    final elapsed = (time - _lastAt).inMicroseconds / 1000.0;
    _last = position;
    _lastAt = time;
    var gain = speed.gain;
    if (speed.acceleration && elapsed > 0) {
      final v = delta.distance / elapsed;
      // The same constant-speed path keeps its gain when Android divides it
      // into shorter events. _velocity remains the scroll inertia estimator.
      gain *= 1 + 1.5 * ((v - .4) / 1.2).clamp(0.0, 1.0);
    }
    final v = view();
    final size = v.rect.size;
    if (size.isEmpty) return;
    final next = AndroidPictureView.clampNormal(
      cursor +
          Offset(delta.dx * gain / size.width, delta.dy * gain / size.height),
    );
    _move(next);
    // A zoomed picture (or one above the keyboard) follows the cursor.
    if (follow?.call() ?? true) _show(v, v.reveal(next, visible()));
  }

  /// Direct drag: the pointer under the finger; near the edge of a zoomed
  /// picture more of it comes into view.
  void _dragTo(Offset position) {
    final v = view();
    final bounds = visible();
    var shift = Offset.zero;
    if (v.zoomed || v.movable(bounds)) {
      if (position.dx < bounds.left + _edge) shift += const Offset(10, 0);
      if (position.dx > bounds.right - _edge) shift -= const Offset(10, 0);
      if (position.dy < bounds.top + _edge) shift += const Offset(0, 10);
      if (position.dy > bounds.bottom - _edge) shift -= const Offset(0, 10);
    }
    final next = shift == Offset.zero ? v : v.panned(shift, visible: bounds);
    _show(v, next);
    _move(next.normalizeClamped(position));
  }

  void _pinch() {
    final focal = _centre();
    var scale = _pinchScale * _spread() / _pinchDistance;
    // The whole picture is a detent (within 2 %), and the limits are felt
    // once per gesture, as ToTBDesk's pinch does.
    if ((scale - 1).abs() < .02) scale = 1;
    final shrinking = scale < _pinchLast;
    _pinchLast = scale;
    if (scale <= 1 && shrinking && !_pinchAtFit) {
      _pinchAtFit = true;
      onCue?.call(AndroidTouchCue.zoomLimit, null);
    } else if (scale >= AndroidPictureView.maxScale &&
        !shrinking &&
        !_pinchAtMax) {
      _pinchAtMax = true;
      onCue?.call(AndroidTouchCue.zoomLimit, null);
    }
    final current = view();
    _show(
      current,
      current.zoomTo(scale, _pinchAnchor, focal, visible: visible()),
    );
  }

  Offset get _target => _direct ? (_press ?? cursor) : _tapCursor;

  // --- real touch contacts ------------------------------------------------------

  void _contact(int pointer, Offset position, AndroidContactPhase phase) {
    final sink = contacts?.call();
    if (sink == null) return;
    if (phase == AndroidContactPhase.down) {
      final point = view().normalize(position);
      if (point == null) return;
      _native.add(pointer);
      sink.contact(pointer, point, phase);
      onCue?.call(AndroidTouchCue.contact, point);
      return;
    }
    if (!_native.contains(pointer)) return;
    if (phase == AndroidContactPhase.up || phase == AndroidContactPhase.cancel) {
      _native.remove(pointer);
    }
    sink.contact(pointer, view().normalizeClamped(position), phase);
  }

  void _cancelNative() {
    final sink = contacts?.call();
    for (final id in _native) {
      sink?.contact(id, _pointers[id] == null
          ? cursor
          : view().normalizeClamped(_pointers[id]!), AndroidContactPhase.cancel);
    }
    _native.clear();
  }

  // --- gestures ---------------------------------------------------------------

  void down(int pointer, Offset position, Duration time) {
    _stopFling();
    _pointers[pointer] = position;
    _starts[pointer] = position;
    switch (_pointers.length) {
      case 1:
        _first = pointer;
        _start = _last = _scrollRef = position;
        _downAt = _lastAt = _scrollAt = time;
        _velocity = 0;
        _wheel = 0;
        if (_direct && contacts?.call()?.available == true) {
          // Real touch: the remote app interprets the contacts.
          flushPendingClick();
          _phase = _Phase.native;
          _contact(pointer, position, AndroidContactPhase.down);
          return;
        }
        _press = view().normalize(position);
        if (_direct && _press == null) {
          // Outside the picture there is nothing to touch.
          flushPendingClick();
          _phase = _Phase.ignore;
          return;
        }
        final lastUp = _lastTapUp;
        final second =
            _pendingClickAt != null &&
            lastUp != null &&
            time - lastUp <= doubleTap &&
            (position - _lastTapScreen).distance <=
                (_direct
                    ? math.max(3 * slop, _directDoubleTapRadius)
                    : _doubleTapRadius);
        if (second) {
          // Tap, then press again: hold back the first click until this
          // press is a quick tap (double click) or becomes a drag.
          _heldClickAt = _pendingClickAt;
          _pendingClick?.cancel();
          _pendingClick = null;
          _pendingClickAt = null;
          _phase = _Phase.dragPending;
          _dragTimer = Timer(doubleTap, () => _commitDrag(feedback: true));
        } else {
          flushPendingClick();
          _phase = _Phase.pending;
          _hold = Timer(longPress, _held);
        }
        if (_direct) {
          _move(_press!);
        } else {
          _tapCursor = cursor;
        }
        if (_phase == _Phase.pending) {
          onCue?.call(AndroidTouchCue.press, _target);
        }
      case 2:
        if (_phase == _Phase.native) {
          _contact(pointer, position, AndroidContactPhase.down);
          return;
        }
        _cancelHold();
        _dragTimer?.cancel();
        _dragTimer = null;
        // The candidate double tap ended: its first click was a real click.
        final held = _heldClickAt;
        _heldClickAt = null;
        if (held != null) _click('left', held);
        if (_button) _endDrag(cursor);
        _velocity = 0;
        _twoTap = _phase == _Phase.pending || _phase == _Phase.dragPending;
        _phase = _Phase.two;
        _twoAt = time;
        _centroid = _scrollRef = _centre();
        _scrollAt = time;
        _pinchDistance = math.max(1, _spread());
        _pinchScale = _pinchLast = view().scale;
        _pinchAtFit = _pinchAtMax = false;
        _pinchAnchor = view().anchorAt(_centroid);
      case 3:
        // A third finger ends any remote gesture (real touch contacts are
        // cancelled); a still, quick three-finger tap toggles the keyboard
        // in either mode (ToTBDesk WINDOWS-INPUT-2.3.0).
        _cancelNative();
        if (_button) _endDrag(cursor);
        _threeTap =
            (_phase == _Phase.two ||
                _phase == _Phase.pending ||
                _phase == _Phase.native) &&
            _still() &&
            time - _downAt < longPress;
        _threeAt = time;
        _phase = _Phase.three;
      case 4:
        // A fourth finger on a three-finger tap still in progress: the tap
        // switches 포인터 ↔ 터치 instead of the keyboard.
        _fourTap =
            _phase == _Phase.three &&
            _threeTap &&
            _still() &&
            time - _downAt < longPress;
        _threeTap = false;
        _fourAt = time;
        _phase = _Phase.four;
      default:
        _threeTap = _fourTap = false;
        _phase = _Phase.ignore;
    }
  }

  void _held() {
    _hold = null;
    if (_phase != _Phase.pending || _pointers.length != 1) return;
    // Long press: right click now, where the tap would have clicked.
    _phase = _Phase.held;
    final at = _target;
    _click('right', at);
    onCue?.call(AndroidTouchCue.rightClick, at);
  }

  void _commitDrag({required bool feedback}) {
    _dragTimer?.cancel();
    _dragTimer = null;
    if (_phase != _Phase.dragPending || _pointers.length != 1) return;
    // The withheld first click is never sent: Windows would read it as the
    // first half of a double click on what is being dragged.
    _heldClickAt = null;
    final from = _target;
    _phase = _Phase.drag;
    _button = true;
    _buttonEvent('left', true, from);
    onCue?.call(
      feedback ? AndroidTouchCue.hold : AndroidTouchCue.dragStart,
      from,
    );
  }

  void _endDrag(Offset at) {
    _button = false;
    _buttonEvent('left', false, at);
    onCue?.call(AndroidTouchCue.dragEnd, at);
  }

  /// The point where movement past the slop began, so the cursor continues
  /// from there instead of jumping by everything travelled meanwhile.
  Offset _slopEdge(Offset position) {
    final travelled = position - _start;
    final distance = travelled.distance;
    return distance <= slop ? position : _start + travelled * (slop / distance);
  }

  void move(int pointer, Offset position, Duration time) {
    final previous = _pointers[pointer];
    if (previous == null) return;
    _pointers[pointer] = position;
    final moved = (position - _start).distance > slop;
    switch (_phase) {
      case _Phase.native:
        _contact(pointer, position, AndroidContactPhase.move);
      case _Phase.pending:
        if (!moved) return;
        _cancelHold();
        if (_direct) {
          // Mouse emulation: a swipe scrolls (a drag is a double tap and
          // hold), so reading a page never selects text by accident.
          _phase = _Phase.moving;
          _scrollRef = _slopEdge(position);
          _scrollAt = time;
          _scrollTo(position, time);
        } else {
          _phase = _Phase.moving;
          _last = _slopEdge(position);
          _lastAt = time;
          _follow(position, time);
        }
      case _Phase.moving:
        if (pointer != _first) return;
        if (_direct) {
          _scrollTo(position, time);
        } else {
          _follow(position, time);
        }
      case _Phase.held:
        // After a long press the right-click menu is open: the trackpad
        // finger can move to an item (no click on release).
        if (!_direct && pointer == _first) {
          if (_lastAt == _downAt && !moved) return;
          if (_lastAt == _downAt) _last = _slopEdge(position);
          _follow(position, time);
        }
      case _Phase.dragPending:
        if (!moved || pointer != _first) return;
        // Movement confirms the drag (felt once already as the tap).
        _commitDrag(feedback: false);
        if (_direct) {
          _dragTo(position);
        } else {
          _last = _slopEdge(position);
          _lastAt = time;
          _follow(position, time);
        }
      case _Phase.drag:
        if (pointer != _first) return;
        if (_direct) {
          _dragTo(position);
        } else {
          _follow(position, time);
        }
      case _Phase.two:
        final centre = _centre();
        if ((_spread() - _pinchDistance).abs() > slop * 2) {
          _twoTap = false;
          _phase = _Phase.pinch;
          onCue?.call(AndroidTouchCue.pinch, null);
          _pinch();
        } else if ((centre - _centroid).distance > slop) {
          _twoTap = false;
          _phase = _Phase.twoScroll;
          _scrollTo(centre, time);
        }
      case _Phase.twoScroll:
        _scrollTo(_centre(), time);
      case _Phase.pinch:
        _pinch();
      case _Phase.three:
        if (!_still()) _threeTap = false;
      case _Phase.four:
        if (!_still()) _fourTap = false;
      case _Phase.idle || _Phase.ignore:
        return;
    }
  }

  void up(int pointer, Offset position, Duration time) =>
      _end(pointer, position, time, cancelled: false);

  void cancel(int pointer) =>
      _end(pointer, _pointers[pointer] ?? _last, _lastAt, cancelled: true);

  void _end(
    int pointer,
    Offset position,
    Duration time, {
    required bool cancelled,
  }) {
    if (!_pointers.containsKey(pointer)) return;
    _pointers[pointer] = position;
    final count = _pointers.length;
    switch (_phase) {
      case _Phase.native:
        _contact(
          pointer,
          position,
          cancelled ? AndroidContactPhase.cancel : AndroidContactPhase.up,
        );
      case _Phase.pending when count == 1:
        _cancelHold();
        if (!cancelled &&
            time - _downAt < longPress &&
            (position - _start).distance <= slop) {
          // A tap: felt now, clicked once no second press follows.
          final at = _target;
          onCue?.call(AndroidTouchCue.tap, at);
          _pendingClickAt = at;
          _lastTapUp = time;
          _lastTapScreen = _start;
          _pendingClick = Timer(doubleTap, flushPendingClick);
        }
      case _Phase.dragPending when count == 1:
        final heldOrMoved =
            time - _downAt >= doubleTap || (position - _start).distance > slop;
        if (cancelled) {
          _dragTimer?.cancel();
          _dragTimer = null;
          _heldClickAt = null;
        } else if (!heldOrMoved) {
          // A quick second tap: the withheld click and this one, a double
          // click at the first tap's point.
          _dragTimer?.cancel();
          _dragTimer = null;
          final first = _heldClickAt ?? _target;
          _heldClickAt = null;
          _click('left', first);
          _click('left', first);
          _lastTapUp = null;
          onCue?.call(AndroidTouchCue.tap, first);
          onCue?.call(AndroidTouchCue.click, null);
          onCue?.call(AndroidTouchCue.click, null);
        } else {
          _commitDrag(feedback: false);
          _endDrag(_direct ? view().normalizeClamped(position) : cursor);
        }
      case _Phase.drag when pointer == _first:
        // Always released, also when Android cancels the touch.
        _endDrag(_direct ? view().normalizeClamped(position) : cursor);
        _phase = _Phase.ignore;
      case _Phase.moving when count == 1:
        if (_direct && !cancelled) _startFling(time);
      case _Phase.two:
        if (!cancelled && _twoTap && time - _twoAt < longPress && _still()) {
          // Two-finger tap: right click (direct: at the first finger).
          final at = _target;
          _click('right', at);
          onCue?.call(AndroidTouchCue.rightClick, at);
        }
        _twoTap = false;
        _phase = _Phase.ignore;
      case _Phase.twoScroll:
        if (!cancelled) _startFling(time);
        _phase = _Phase.ignore;
      case _Phase.pinch:
        _phase = _Phase.ignore;
      case _Phase.three:
        if (count == 1 &&
            !cancelled &&
            _threeTap &&
            time - _threeAt < longPress) {
          onCue?.call(AndroidTouchCue.keyboard, null);
        }
        if (cancelled) _threeTap = false;
      case _Phase.four:
        if (count == 1 &&
            !cancelled &&
            _fourTap &&
            time - _fourAt < longPress) {
          onCue?.call(AndroidTouchCue.touchMode, null);
        }
        if (cancelled) _fourTap = false;
      default:
        break;
    }
    _pointers.remove(pointer);
    _starts.remove(pointer);
    if (_pointers.isEmpty) {
      _cancelHold();
      _dragTimer?.cancel();
      _dragTimer = null;
      _heldClickAt = null;
      _phase = _Phase.idle;
      _first = null;
      _press = null;
      _wheel = 0;
      _threeTap = _fourTap = false;
      _native.clear();
    }
  }
}

/// Counts of this device's input in one diagnostics window (never content):
/// sent with the streaming statistics so a field test shows what the user
/// did (taps, drags, text commits) next to what the session measured.
class AndroidInputCounters {
  int taps = 0, rightClicks = 0, dragStarts = 0, wheelSteps = 0;
  int pinches = 0, keyboardOpens = 0, keyPresses = 0, mouseButtons = 0;
  int textCommits = 0, textUnits = 0, textKeyChars = 0, textDropped = 0;

  void cue(AndroidTouchCue cue) {
    switch (cue) {
      case AndroidTouchCue.click:
        taps++;
      case AndroidTouchCue.rightClick:
        rightClicks++;
      case AndroidTouchCue.dragStart || AndroidTouchCue.hold:
        dragStarts++;
      case AndroidTouchCue.wheel:
        wheelSteps++;
      case AndroidTouchCue.pinch:
        pinches++;
      default:
        break;
    }
  }

  /// This window's values (the `streamStats` field names), then zero.
  Map<String, Object> take({
    required AndroidTouchMode mode,
    required bool textInput,
  }) {
    final values = <String, Object>{
      'touchMode': mode == AndroidTouchMode.trackpad ? 1 : 0,
      'taps': taps,
      'rightClicks': rightClicks,
      'dragStarts': dragStarts,
      'wheelSteps': wheelSteps,
      'pinches': pinches,
      'keyboardOpens': keyboardOpens,
      'textCommits': textCommits,
      'textUnits': textUnits,
      'textKeyChars': textKeyChars,
      'textDropped': textDropped,
      'keyPresses': keyPresses,
      'mouseButtons': mouseButtons,
      'textInput': textInput,
    };
    taps = rightClicks = dragStarts = wheelSteps = 0;
    pinches = keyboardOpens = keyPresses = mouseButtons = 0;
    textCommits = textUnits = textKeyChars = textDropped = 0;
    return values;
  }
}
