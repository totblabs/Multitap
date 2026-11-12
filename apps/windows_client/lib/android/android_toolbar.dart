import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' show DisplayFeature, DisplayFeatureType, FontFeature;

import 'package:flutter/foundation.dart' show ValueListenable;
import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart' show CustomSemanticsAction;
import 'package:flutter/services.dart';

import '../ui/korean_text.dart';
import '../ui/session_menu.dart';
import '../ui/session_tools.dart';
import 'android_store.dart' show AndroidBubbleEdge;
import '../l10n/l10n.dart';

/// One button of the floating cluster (48 dp), and the cluster's margin from
/// the edges of the area it may use.
const androidBubbleSize = 48.0, androidBubbleMargin = 8.0;

/// The floating cluster lies along the edge it rests on: upright on the
/// left or right edge, across on the top or bottom one.
bool androidBubbleUpright(AndroidBubbleEdge edge) =>
    edge == AndroidBubbleEdge.left || edge == AndroidBubbleEdge.right;

/// [area] less the cluster's margin; where there is only just room for a
/// [size] cluster the margin gives way, so it stays whole inside [area].
Rect androidBubbleInner(Rect area, [Size size = const Size.square(48)]) {
  double margin(double room, double needed) =>
      ((room - needed) / 2).clamp(0.0, androidBubbleMargin);
  final x = margin(area.width, size.width);
  final y = margin(area.height, size.height);
  return Rect.fromLTRB(
    area.left + x,
    area.top + y,
    area.right - x,
    area.bottom - y,
  );
}

/// Where the floating cluster of [length] dp (its buttons in a row) resting
/// on [edge], [along] of the way (0 top or left .. 1), sits inside [area]
/// (the safe part of the screen above the keyboard), and whether it lies
/// upright. A smaller area (the phone keyboard opened) keeps it inside at
/// the same proportion; where the edge is too short for it (landscape with
/// the keyboard open) it turns across at the same edge.
({Rect rect, bool upright}) androidBubbleLayout(
  Rect area,
  AndroidBubbleEdge edge,
  double along, {
  double length = androidBubbleSize,
}) {
  const short = androidBubbleSize;
  var upright = androidBubbleUpright(edge);
  if (upright && area.height < length && area.width >= length) {
    upright = false;
  } else if (!upright && area.width < length && area.height >= length) {
    upright = true;
  }
  final size = upright ? Size(short, length) : Size(length, short);
  final inner = androidBubbleInner(area, size);
  final t = along.clamp(0.0, 1.0);
  final x = inner.left + t * math.max(0.0, inner.width - size.width);
  final y = inner.top + t * math.max(0.0, inner.height - size.height);
  final right = math.max(inner.left, inner.right - size.width);
  final bottom = math.max(inner.top, inner.bottom - size.height);
  final (left, top) = switch (edge) {
    AndroidBubbleEdge.left => (inner.left, y),
    AndroidBubbleEdge.right => (right, y),
    AndroidBubbleEdge.top => (x, inner.top),
    AndroidBubbleEdge.bottom => (x, bottom),
  };
  return (
    rect: Rect.fromLTWH(left, top, size.width, size.height),
    upright: upright,
  );
}

/// Where a single 48 dp bubble rests ([androidBubbleLayout]).
Rect androidBubbleRect(Rect area, AndroidBubbleEdge edge, double along) =>
    androidBubbleLayout(area, edge, along).rect;

/// The edge of [area] nearest a cluster of [length] let go with its centre
/// at [center], and how far along it, for [androidBubbleLayout].
(AndroidBubbleEdge, double) androidBubbleSnap(
  Rect area,
  Offset center, {
  double length = androidBubbleSize,
}) {
  final edges = androidBubbleInner(area);
  final distances = {
    AndroidBubbleEdge.left: center.dx - edges.left,
    AndroidBubbleEdge.right: edges.right - center.dx,
    AndroidBubbleEdge.top: center.dy - edges.top,
    AndroidBubbleEdge.bottom: edges.bottom - center.dy,
  };
  final edge = distances.entries
      .reduce((a, b) => b.value < a.value ? b : a)
      .key;
  final upright = androidBubbleUpright(edge);
  final size = upright
      ? Size(androidBubbleSize, length)
      : Size(length, androidBubbleSize);
  final inner = androidBubbleInner(area, size);
  double along(double at, double from, double span, double half) =>
      span <= 0 ? .5 : ((at - half - from) / span).clamp(0.0, 1.0);
  return (
    edge,
    upright
        ? along(
            center.dy,
            inner.top,
            inner.height - size.height,
            size.height / 2,
          )
        : along(
            center.dx,
            inner.left,
            inner.width - size.width,
            size.width / 2,
          ),
  );
}

/// The part of the screen the session toolbar may use: inside the system bars
/// and the camera cutout, and, on a foldable, beside a hinge or fold that
/// cuts across the top edge (the wider side is used, so nothing is hidden
/// in the hinge). Everything is in the coordinates of [size]. With
/// [bottom] the band is the bottom edge's instead (the tool dock).
/// A phone on its side (shorter than 500 dp): the 화면 panel lays its groups
/// out in two columns without a title row, so it fits the height instead of
/// scrolling in its card.
bool androidPanelBeside(Size size) =>
    size.width > size.height && size.height < 500;

Rect androidToolbarArea(
  Size size,
  EdgeInsets padding,
  List<DisplayFeature> features, {
  bool bottom = false,
}) {
  var left = padding.left, right = size.width - padding.right;
  final top = bottom ? size.height - padding.bottom - 64 : padding.top;
  // Features that reach into the band the dock or bar sits in.
  final blocked = <(double, double)>[];
  for (final feature in features) {
    // Cutouts are kept clear by the system padding; only a hinge or fold
    // that cuts across the edge (taller than wide) splits the area.
    if (feature.type == DisplayFeatureType.cutout) continue;
    final b = feature.bounds;
    if (b.isEmpty ||
        b.top >= top + 64 ||
        b.bottom <= top ||
        b.height < b.width) {
      continue;
    }
    blocked.add((b.left, b.right));
  }
  if (blocked.isEmpty) {
    return Rect.fromLTRB(left, top, math.max(left, right), top + 64);
  }
  blocked.sort((a, b) => a.$1.compareTo(b.$1));
  // The widest free stretch between the blocked ones.
  var best = (left, left);
  var cursor = left;
  for (final (from, to) in blocked) {
    if (from - cursor > best.$2 - best.$1) best = (cursor, from);
    cursor = math.max(cursor, to);
  }
  if (right - cursor > best.$2 - best.$1) best = (cursor, right);
  left = best.$1;
  right = math.max(left, best.$2);
  return Rect.fromLTRB(left, top, right, top + 64);
}

/// Tells the session bar whether a finger that began on the remote picture
/// is still down, so the bar ignores the touch completely until it lifts and
/// a drag across it keeps reaching the remote. Feed it from the picture's
/// pointer events.
class AndroidPointerHold {
  final held = ValueNotifier<bool>(false);
  final _down = <int>{};

  void down(int pointer) {
    _down.add(pointer);
    held.value = true;
  }

  void up(int pointer) {
    _down.remove(pointer);
    held.value = _down.isNotEmpty;
  }

  void dispose() => held.dispose();
}

/// The remote session's tools on Android: one small amber cluster over the
/// picture, holding ⋯ (the session menu) and then the [quick] one-tap
/// buttons (입력 방식, 키보드: always outside the menu; top to bottom ⋯,
/// 입력 방식, 키보드). It can be dragged anywhere in [bubbleArea], snaps to the
/// nearest edge there (upright on the left or right edge, across on the
/// top or bottom one; [onBubbleMoved] remembers where), fades after a few
/// idle seconds and comes back on a touch.
///
/// ⋯ opens the session menu beside the cluster, growing away from the edge
/// it rests on (it opens where the cluster was touched, not in a bottom
/// dock). Its entries are the
/// Windows bar's ([SessionTool]: same names, order and groups), with the
/// measured connection quality on top and 연결 끊기 last in red. A menu's
/// rows, the quality page and the host's [panel] (e.g. 화면) open in its
/// place, at the same spot. While it is open a touch anywhere else closes
/// it and never reaches the remote picture; Back goes one level up; a
/// command folds it after use. The cluster stays: its one-tap buttons keep
/// working, and ⋯ goes back to the menu or closes it.
class AndroidSessionBar extends StatefulWidget {
  const AndroidSessionBar({
    super.key,
    required this.title,
    required this.modeLabel,
    required this.actions,
    required this.onInteract,
    required this.onDisconnect,
    required this.pointerHeld,
    this.bubbleArea,
    this.quick = const [],
    this.bubbleEdge = AndroidBubbleEdge.right,
    this.bubbleAlong = .62,
    this.onBubbleMoved,
    this.latencyMs,
    this.qualityItems,
    this.onQuality,
    this.panel,
    this.panelWidth,
    this.onPanelClosed,
    this.onOpenChanged,
    this.popups,
    this.layoutKey,
  });

  final String title, modeLabel;

  /// The session's commands and menus, in [SessionTool] order.
  final List<SessionToolbarMenu> actions;
  final VoidCallback onInteract, onDisconnect;
  final ValueListenable<bool> pointerHeld;

  /// Where the cluster may sit and its menu may open (the safe area above
  /// the keyboard). Null: no room for it (e.g. a keyboard covers the
  /// picture); an open menu closes.
  final Rect? bubbleArea;

  /// One-tap buttons that float with ⋯, in order after it.
  final List<AndroidQuickButton> quick;

  /// The edge the cluster rests on and how far along it ([androidBubbleRect]).
  final AndroidBubbleEdge bubbleEdge;
  final double bubbleAlong;

  /// The cluster was dragged and snapped to a new place, to be remembered.
  final void Function(AndroidBubbleEdge edge, double along)? onBubbleMoved;

  /// Measured host RTT only; a missing sample is never invented.
  final double? latencyMs;

  /// The quality page's rows after the round trip ([sessionQualityItems]).
  final List<SessionMenuItem> Function()? qualityItems;

  /// Opens the host's own 연결 panel (through [panel]) from the menu's 연결
  /// 품질 row, instead of the list of measured values.
  final VoidCallback? onQuality;

  /// A card the host opens in the menu's place (null: none), from a command
  /// with [SessionToolbarMenu.onPressedAt], at most [panelWidth] wide;
  /// [onPanelClosed] asks the host to close it (its back button, Back, or
  /// ⋯), which shows the menu again.
  final Widget? panel;
  final double? panelWidth;
  final VoidCallback? onPanelClosed;

  /// The menu opened (true) or closed (false), after the frame.
  final ValueChanged<bool>? onOpenChanged;

  /// An open menu page counts as an open popup here: [SessionPopups.closeAll]
  /// takes it back to the menu.
  final SessionPopups? popups;

  /// Where and when the cluster is used (screen size, orientation): a
  /// change closes the menu.
  final Object? layoutKey;

  /// The cluster fades to [fadedOpacity] after this long without a touch.
  static const fadeAfter = Duration(seconds: 3);
  static const fadedOpacity = .4;

  /// The menu's own width, and its pages'.
  static const menuWidth = 288.0, pageWidth = 312.0, defaultPanelWidth = 360.0;

  @override
  State<AndroidSessionBar> createState() => AndroidSessionBarState();
}

class AndroidSessionBarState extends State<AndroidSessionBar> {
  final _menuFocus = FocusNode(
    debugLabel: 'Android connection menu',
    skipTraversal: true,
  );
  final _toggleFocus = FocusNode(debugLabel: 'Android menu toggle');
  bool _open = false, _reportedOpen = false;

  /// The open page: a menu's label, or [_qualityPage]; null: the menu.
  String? _page;
  static const _qualityPage = '';
  VoidCallback? _releasePopup;

  /// Whether the menu is showing.
  bool get isOpen => _open;

  /// Closes the menu (a touch on the picture, 화면 이동).
  void fold() => _collapse();

  /// Opens the menu beside the cluster, showing the host's [panel] when it
  /// has one (a long press on 입력 방식).
  void show() {
    if (!mounted || widget.bubbleArea == null) return;
    widget.onInteract();
    _fadeTimer?.cancel();
    setState(() {
      _closePage();
      _open = true;
      _faded = false;
    });
  }

  /// Back: one level up (a page or the host's panel goes back to the menu,
  /// the menu closes). False when nothing was open.
  bool back() {
    if (!_open) return false;
    if (widget.panel != null) {
      widget.onPanelClosed?.call();
    } else if (_page != null) {
      setState(_closePage);
    } else {
      _collapse();
    }
    return true;
  }

  // The cluster: where it rests, where a finger drags it, and its fade.
  late AndroidBubbleEdge _edge = widget.bubbleEdge;
  late double _along = widget.bubbleAlong;
  Offset? _drag;

  /// From the finger to the cluster's centre while it is dragged, taken
  /// where the finger came down ([_downAt], global).
  Offset _grab = Offset.zero;
  Offset? _downAt;

  /// The cluster keeps the way it lay while it is dragged.
  bool _dragUpright = true;

  double get _length => (widget.quick.length + 1) * androidBubbleSize;

  Size _clusterSize(bool upright) => upright
      ? Size(androidBubbleSize, _length)
      : Size(_length, androidBubbleSize);
  bool _faded = false;
  Timer? _fadeTimer;

  /// The cluster is faded (idle).
  bool get bubbleFaded => _faded;

  @override
  void initState() {
    super.initState();
    _scheduleFade();
  }

  @override
  void didUpdateWidget(AndroidSessionBar oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.layoutKey != oldWidget.layoutKey && _open) {
      _closePage();
      _open = false;
      _scheduleFade();
    }
    if (widget.bubbleEdge != oldWidget.bubbleEdge ||
        widget.bubbleAlong != oldWidget.bubbleAlong) {
      _edge = widget.bubbleEdge;
      _along = widget.bubbleAlong;
    }
  }

  @override
  void dispose() {
    _fadeTimer?.cancel();
    _releasePopup?.call();
    _menuFocus.dispose();
    _toggleFocus.dispose();
    super.dispose();
  }

  void _scheduleFade() {
    _fadeTimer?.cancel();
    _fadeTimer = Timer(AndroidSessionBar.fadeAfter, () {
      if (mounted && !_faded && !_open && _drag == null) {
        setState(() => _faded = true);
      }
    });
  }

  /// A touch on the cluster brings it back; it fades again when left alone.
  void _wake() {
    if (_faded) setState(() => _faded = false);
    if (_open) {
      _fadeTimer?.cancel();
    } else {
      _scheduleFade();
    }
  }

  Offset _local(Offset global) =>
      (context.findRenderObject() as RenderBox).globalToLocal(global);

  /// The cluster follows the finger where it took hold of it; an open menu
  /// closes first.
  void _dragStart(Rect from, bool upright, Offset global) {
    _collapse();
    _wake();
    _fadeTimer?.cancel();
    _grab = from.center - _local(_downAt ?? global);
    _dragUpright = upright;
    setState(() => _drag = from.center);
  }

  void _dragUpdate(Offset global, Rect area) {
    if (_drag == null) return;
    final size = _clusterSize(_dragUpright);
    final inner = androidBubbleInner(area, size);
    final halfX = size.width / 2, halfY = size.height / 2;
    final at = _local(global) + _grab;
    setState(
      () => _drag = Offset(
        at.dx.clamp(
          inner.left + halfX,
          math.max(inner.left + halfX, inner.right - halfX),
        ),
        at.dy.clamp(
          inner.top + halfY,
          math.max(inner.top + halfY, inner.bottom - halfY),
        ),
      ),
    );
  }

  /// Let go: the cluster snaps to the nearest edge, and its place is kept.
  void _dragEnd(Rect area) {
    final drag = _drag;
    if (drag == null) return;
    final (edge, along) = androidBubbleSnap(area, drag, length: _length);
    setState(() {
      _drag = null;
      _edge = edge;
      _along = along;
    });
    widget.onBubbleMoved?.call(edge, along);
    _scheduleFade();
  }

  /// C hamburger: opens the menu, or closes the complete menu/panel.
  /// Android Back and the panel's back arrow retain one-level navigation.
  void _menuButton() {
    if (!_open) {
      widget.onInteract();
      unawaited(HapticFeedback.selectionClick());
      _fadeTimer?.cancel();
      setState(() {
        _open = true;
        _faded = false;
      });
      _menuFocus.requestFocus();
    } else {
      _foldFromToggle();
    }
  }

  void _foldFromToggle() {
    if (widget.panel != null) widget.onPanelClosed?.call();
    _collapse();
  }

  void _collapse() {
    if (!mounted || !_open) return;
    setState(() {
      _closePage();
      _open = false;
    });
    _scheduleFade();
  }

  /// A command was used: the menu closes.
  void _used() => _collapse();

  void _closePage() {
    _page = null;
    final release = _releasePopup;
    _releasePopup = null;
    release?.call();
  }

  void _showPage(String page) {
    widget.onInteract();
    if (widget.panel != null) widget.onPanelClosed?.call();
    setState(() {
      _closePage();
      _page = page;
      _releasePopup = widget.popups?.holdNative(() {
        _releasePopup = null;
        if (mounted && _page != null) setState(() => _page = null);
      });
    });
  }

  Future<void> _command(SessionToolbarMenu menu) async {
    widget.onInteract();
    menu.onInteract();
    unawaited(HapticFeedback.selectionClick());
    final at = menu.onPressedAt;
    if (at != null) {
      // A panel the host opens in the menu's place.
      if (_page != null) setState(_closePage);
      final box = context.findRenderObject() as RenderBox?;
      await at(
        box == null ? Rect.zero : box.localToGlobal(Offset.zero) & box.size,
      );
      return;
    }
    menu.onPressed?.call();
    menu.onUsed?.call();
    _used();
  }

  void _select(SessionMenuItem item) {
    widget.onInteract();
    unawaited(HapticFeedback.selectionClick());
    setState(_closePage);
    item.onSelected?.call();
    if (!item.keepToolbarOpen) _used();
  }

  bool get _hasLatency {
    final latency = widget.latencyMs;
    return latency != null && latency.isFinite && latency > 0;
  }

  String get _latencyText {
    final latency = widget.latencyMs!;
    return latency < 1 ? '<1 ms' : '${latency.toStringAsFixed(0)} ms';
  }

  static TextStyle _style(
    double size, {
    FontWeight weight = FontWeight.w600,
    Color? color,
  }) => TextStyle(
    fontFamily: 'Pretendard',
    fontFamilyFallback: const ['Malgun Gothic'],
    fontSize: size,
    fontWeight: weight,
    color: color,
    height: 1.2,
    fontFeatures: const [FontFeature.tabularFigures()],
  );

  /// The menu: the Windows bar turned on its side. Its rows are the bar's
  /// commands in the bar's order: 연결 품질
  /// with the measured latency and its word, the session's entries (화면,
  /// 설정) and 연결 끊기 last in red; each opens the same panel the Windows
  /// bar opens, in the menu's place. A caption stands over a group only
  /// where the group has rows of its own (not over a single row that is
  /// the group itself).
  Widget _menu(BuildContext context, SessionChrome chrome, double width) {
    final latency = _hasLatency ? widget.latencyMs : null;
    final word = SessionChrome.latencyWord(latency);
    final entries = SessionTool.ordered(widget.actions, (m) => m.label);
    // Commands outside the vocabulary keep their place without a caption.
    int groupOf(SessionToolbarMenu menu) {
      final tool = SessionTool.named(menu.label);
      return tool == null
          ? 0
          : switch (tool.group) {
              1 => 1,
              2 => 2,
              _ => 3,
            };
    }

    void openQuality() {
      widget.onInteract();
      final open = widget.onQuality;
      if (open != null) {
        open();
      } else {
        _showPage(_qualityPage);
      }
    }

    final rows = <Widget>[];
    int? group;
    for (final menu in entries) {
      final tool = SessionTool.named(menu.label);
      final next = groupOf(menu);
      final members = entries.where((other) => groupOf(other) == next);
      final caption = SessionTool.caption(next);
      // 폴더·USB (or the one side of it this connection has) is its group's
      // own row, as 화면 and 설정 are.
      final captioned =
          caption != null &&
          !(members.length == 1 &&
              (members.single.label == caption || next == 2));
      if (next != group && captioned) {
        if (group != null) rows.add(const SizedBox(height: 8));
        rows.add(AndroidMenuCaption(caption));
      }
      group = next;
      final opens = !menu.command || menu.onPressedAt != null;
      // A switch (마우스 잠금), as in the Windows 설정 panel.
      final toggle = tool == SessionTool.gameMouse;
      void run() {
        if (menu.command) {
          unawaited(_command(menu));
        } else {
          menu.onInteract();
          _showPage(menu.label);
        }
      }

      rows.add(
        AndroidBarMenuRow(
          opens: opens,
          toggle: toggle,
          tooltip: menu.tooltip,
          description: !menu.enabled
              ? menu.disabledReason
              : switch (tool) {
                  // What its panel holds, as the Windows panel does.
                  SessionTool.screen when menu.command => l10n.viewScaleResolution,
                  // What is shared now, or what the panel is for.
                  SessionTool.share ||
                  SessionTool.drives ||
                  SessionTool.usb => menu.tooltip,
                  SessionTool.more => l10n.qualityInputHelp,
                  SessionTool.scale => l10n.textScreenSize,
                  SessionTool.settings => l10n.frameRateBitrate,
                  SessionTool.gameMouse => l10n.useExternalMouse,
                  SessionTool.guide => l10n.availableFeatures,
                  _ => null,
                },
          item: SessionMenuItem(
            menu.label,
            key: menu.key,
            icon: menu.icon,
            // A toggle (마우스 잠금) that is on: its switch shows it.
            checked: menu.active && !opens,
            disabledReason: menu.disabledReason,
            onSelected: menu.enabled ? run : null,
          ),
          onSelected: run,
        ),
      );
    }
    return Material(
      key: const Key('session-command-bar'),
      color: chrome.surface,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: chrome.popupEdge),
      ),
      clipBehavior: Clip.antiAlias,
      child: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(6, 2, 6, 6),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                // Which desktop this is, as the Windows 연결 panel's title.
                Expanded(
                  child: Padding(
                    padding: const EdgeInsets.only(left: 10),
                    child: KeepAllText(
                      '${widget.title} · ${widget.modeLabel}',
                      key: const Key('session-menu-title'),
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: _style(
                        13,
                        weight: FontWeight.w600,
                        color: chrome.ink,
                      ),
                    ),
                  ),
                ),
                IconButton(
                  key: const Key('session-toolbar-fold'),
                  tooltip: l10n.close,
                  onPressed: () {
                    widget.onInteract();
                    _collapse();
                  },
                  style: IconButton.styleFrom(
                    fixedSize: const Size(48, 48),
                    minimumSize: const Size(48, 48),
                    foregroundColor: chrome.icon,
                    splashFactory: NoSplash.splashFactory,
                    highlightColor: chrome.pressed,
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(12),
                    ),
                  ),
                  icon: const Icon(Icons.close_rounded, size: 20),
                ),
              ],
            ),
            AndroidMenuStyle.divider(chrome),
            // 연결 품질: the Windows bar's first item (its "9 ms" chip), with
            // the plain word the Windows panel shows.
            AndroidBarMenuRow(
              opens: true,
              tooltip: word == null
                  ? SessionTool.quality.label
                  : '${SessionTool.quality.label} · $word',
              description: !_hasLatency
                  ? l10n.connected
                  : word == null
                  ? _latencyText
                  : '$_latencyText · $word',
              item: SessionMenuItem(
                SessionTool.quality.label,
                key: const Key('session-quality'),
                icon: SessionTool.quality.icon,
                color: chrome.latencyColor(latency),
                onSelected: openQuality,
              ),
              onSelected: openQuality,
            ),
            ...rows,
            AndroidMenuStyle.divider(chrome),
            AndroidBarMenuRow(
              item: SessionMenuItem(
                SessionTool.disconnect.label,
                key: const Key('session-disconnect'),
                icon: SessionTool.disconnect.icon,
                destructive: true,
                onSelected: () {},
              ),
              onSelected: () {
                widget.onInteract();
                widget.onDisconnect();
                _used();
              },
            ),
          ],
        ),
      ),
    );
  }

  /// What the open menu shows now, and how wide it may be.
  (Widget, double)? _card(BuildContext context, SessionChrome chrome) {
    if (!_open) return null;
    void goBack() {
      widget.onInteract();
      back();
    }

    final panel = widget.panel;
    if (panel != null) {
      return (
        _AndroidPanelBack(onBack: goBack, child: panel),
        widget.panelWidth ?? AndroidSessionBar.defaultPanelWidth,
      );
    }
    final page = _page;
    if (page == null) {
      return (
        _menu(context, chrome, AndroidSessionBar.menuWidth),
        AndroidSessionBar.menuWidth,
      );
    }
    final List<SessionMenuItem> items;
    final String title;
    if (page == _qualityPage) {
      title = SessionTool.quality.label;
      items = [
        SessionMenuItem.info(
          l10n.roundTripLatencyHostResponse,
          _hasLatency ? _latencyText : l10n.notMeasuredYet,
        ),
        ...?widget.qualityItems?.call(),
      ];
    } else {
      final menu = widget.actions.where((menu) => menu.label == page);
      if (menu.isEmpty) return null;
      title = page;
      items = menu.first.items;
    }
    return (
      _AndroidPanelBack(
        onBack: goBack,
        child: AndroidToolbarPanel(
          key: const Key('android-session-menu-card'),
          title: title,
          children: [
            for (final item in items)
              AndroidBarMenuRow(item: item, onSelected: () => _select(item)),
          ],
        ),
      ),
      AndroidSessionBar.pageWidth,
    );
  }

  /// The cluster in [area]: ⋯ and the [quick] buttons. A tap uses one of
  /// its buttons, a drag moves all of it and it snaps to the nearest edge
  /// when let go. It takes the whole touch, so nothing on it reaches the
  /// remote picture.
  Widget _bubble(
    SessionChrome chrome,
    Rect area,
    ({Rect rect, bool upright}) layout,
  ) {
    final latency = _hasLatency ? widget.latencyMs : null;
    final drag = _drag;
    final upright = drag == null ? layout.upright : _dragUpright;
    final size = _clusterSize(upright);
    final rect = drag == null
        ? layout.rect
        : Rect.fromCenter(center: drag, width: size.width, height: size.height);
    final reduceMotion = MediaQuery.disableAnimationsOf(context);
    return AnimatedPositioned(
      key: const Key('session-bubble-slot'),
      duration: drag != null || reduceMotion
          ? Duration.zero
          : const Duration(milliseconds: 220),
      curve: Curves.easeOutCubic,
      left: rect.left,
      top: rect.top,
      width: size.width,
      height: size.height,
      child: Listener(
        // Any touch on the cluster brings it back from its fade.
        onPointerDown: (event) {
          _downAt = event.position;
          _wake();
        },
        child: GestureDetector(
          key: const Key('session-bubble-drag'),
          behavior: HitTestBehavior.opaque,
          onPanStart: (details) =>
              _dragStart(rect, upright, details.globalPosition),
          onPanUpdate: (details) => _dragUpdate(details.globalPosition, area),
          onPanEnd: (_) => _dragEnd(area),
          onPanCancel: () => _dragEnd(area),
          child: AnimatedOpacity(
            key: const Key('session-bubble'),
            opacity: _faded ? AndroidSessionBar.fadedOpacity : 1,
            duration: reduceMotion
                ? Duration.zero
                : const Duration(milliseconds: 400),
            child: AndroidBubbleCluster(
              upright: upright,
              dragging: drag != null,
              // On amber the dot takes the darker latency colours.
              dot: latency == null
                  ? null
                  : const SessionChrome.forDark(false).latencyColor(latency),
              buttons: [
                AndroidQuickButton(
                  key: const Key('expand-session-toolbar'),
                  icon: Icons.menu_rounded,
                  label: l10n.menu,
                  accented: true,
                  focusNode: _toggleFocus,
                  active: _open,
                  tooltip: _open
                      ? l10n.closeMenu
                      : latency == null
                      ? l10n.openMenu
                      : l10n.openMenu2(_latencyText),
                  onPressed: _menuButton,
                ),
                ...widget.quick,
              ],
            ),
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    final media = MediaQuery.of(context);
    final area = widget.bubbleArea;
    if (_open && area == null) {
      // No room for the cluster (a keyboard covers the picture): the menu
      // it opened goes with it.
      WidgetsBinding.instance.addPostFrameCallback((_) => _collapse());
    }
    if (_open != _reportedOpen) {
      final open = _open;
      _reportedOpen = open;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _reportedOpen == open) widget.onOpenChanged?.call(open);
      });
    }
    if (area == null) return const SizedBox.expand();
    final layout = androidBubbleLayout(area, _edge, _along, length: _length);
    final card = _card(context, chrome);
    return Focus(
      focusNode: _menuFocus,
      onKeyEvent: (_, event) {
        if (_open && event.logicalKey == LogicalKeyboardKey.escape) {
          if (event is KeyDownEvent) {
            _foldFromToggle();
            _toggleFocus.requestFocus();
          }
          return KeyEventResult.handled;
        }
        return KeyEventResult.ignored;
      },
      child: ValueListenableBuilder<bool>(
        valueListenable: widget.pointerHeld,
        // While a finger that began on the picture is down the menu takes no
        // new touches; the cluster does (a second finger on it stops the
        // remote drag through onInteract).
        builder: (context, held, _) => Stack(
          children: [
            if (card != null) ...[
              // Outside the menu: a touch closes it and goes nowhere else.
              Positioned.fill(
                child: Listener(
                  key: const Key('session-menu-barrier'),
                  behavior: HitTestBehavior.opaque,
                  onPointerDown: (_) => _collapse(),
                ),
              ),
              Positioned.fill(
                child: IgnorePointer(
                  key: const Key('session-toolbar-pointer'),
                  ignoring: held,
                  child: CustomSingleChildLayout(
                    delegate: AndroidMenuPlacement(
                      area: area,
                      cluster: layout.rect,
                      edge: _edge,
                      width: card.$2,
                      features: media.displayFeatures,
                    ),
                    child: SessionTouchScope(
                      // The rows' tooltips read as one chrome.
                      child: TooltipTheme(
                        data: chrome.tooltipTheme,
                        child: DecoratedBox(
                          decoration: BoxDecoration(
                            borderRadius: BorderRadius.circular(18),
                            boxShadow: [
                              BoxShadow(
                                color: chrome.shadow,
                                blurRadius: 24,
                                offset: const Offset(0, 8),
                              ),
                            ],
                          ),
                          child: card.$1,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ],
            _bubble(chrome, area, layout),
          ],
        ),
      ),
    );
  }
}

/// Where the open session menu lies: beside the cluster ([cluster], resting
/// on [edge]), on the side away from that edge, inside [area] less the
/// cluster's margin and on the cluster's side of a hinge or fold. Where
/// that side is too narrow ([minWidth], a narrow phone) or too short
/// ([minHeight]) it takes the larger room on the other axis instead. It
/// starts level with ⋯ (the cluster's first button) and grows towards the
/// middle of the screen, sliding back where it would leave [area]; [width]
/// at most, as tall as it needs up to the room there (then it scrolls).
class AndroidMenuPlacement extends SingleChildLayoutDelegate {
  AndroidMenuPlacement({
    required this.area,
    required this.cluster,
    required this.edge,
    required this.width,
    this.features = const [],
  });
  final Rect area, cluster;
  final AndroidBubbleEdge edge;
  final double width;
  final List<DisplayFeature> features;

  static const gap = 8.0;

  /// ⋯: the cluster's first button.
  Rect get anchor => Rect.fromLTWH(
    cluster.left,
    cluster.top,
    androidBubbleSize,
    androidBubbleSize,
  );

  /// Narrower or shorter than this beside the cluster (a narrow window),
  /// the menu takes the other axis: above or below a cluster on a side
  /// edge, beside one on the top or bottom edge.
  static const minWidth = 260.0, minHeight = 160.0;

  /// The room on the far side of the cluster from [from] (as if it rested
  /// there), inside [area] and on the cluster's side of a hinge or fold.
  Rect _side(AndroidBubbleEdge from) {
    final inner = androidBubbleInner(area, Size.zero);
    var left = inner.left, top = inner.top;
    var right = inner.right, bottom = inner.bottom;
    switch (from) {
      case AndroidBubbleEdge.right:
        right = cluster.left - gap;
      case AndroidBubbleEdge.left:
        left = cluster.right + gap;
      case AndroidBubbleEdge.top:
        top = cluster.bottom + gap;
      case AndroidBubbleEdge.bottom:
        bottom = cluster.top - gap;
    }
    for (final feature in features) {
      if (feature.type == DisplayFeatureType.cutout) continue;
      final b = feature.bounds;
      if (b.isEmpty) continue;
      if (b.height >= b.width) {
        if (b.left >= cluster.right) right = math.min(right, b.left - gap);
        if (b.right <= cluster.left) left = math.max(left, b.right + gap);
      } else {
        if (b.top >= cluster.bottom) bottom = math.min(bottom, b.top - gap);
        if (b.bottom <= cluster.top) top = math.max(top, b.bottom + gap);
      }
    }
    return Rect.fromLTRB(
      left,
      top,
      math.max(left, right),
      math.max(top, bottom),
    );
  }

  /// The side the menu grows from, and the room it has there.
  ({AndroidBubbleEdge from, Rect room}) get placement {
    var from = edge;
    final room = _side(edge);
    if (androidBubbleUpright(edge) && room.width < math.min(width, minWidth)) {
      final above = _side(AndroidBubbleEdge.bottom).height;
      final below = _side(AndroidBubbleEdge.top).height;
      from = above > below ? AndroidBubbleEdge.bottom : AndroidBubbleEdge.top;
    } else if (!androidBubbleUpright(edge)) {
      final leftward = _side(AndroidBubbleEdge.right).width;
      final rightward = _side(AndroidBubbleEdge.left).width;
      // A phone on its side has height to spare only beside the cluster:
      // the menu stands there, whole, rather than scrolling under it.
      final beside =
          room.height < minHeight ||
          (androidPanelBeside(area.size) &&
              math.max(leftward, rightward) >= math.min(width, minWidth));
      if (beside) {
        from = leftward > rightward
            ? AndroidBubbleEdge.right
            : AndroidBubbleEdge.left;
      }
    }
    return (from: from, room: from == edge ? room : _side(from));
  }

  /// The part of [area] the menu may use.
  Rect get room => placement.room;

  @override
  BoxConstraints getConstraintsForChild(BoxConstraints constraints) {
    final room = this.room;
    return BoxConstraints(
      maxWidth: math.min(width, room.width),
      maxHeight: room.height,
    );
  }

  @override
  Offset getPositionForChild(Size size, Size childSize) {
    final (:from, :room) = placement;
    final at = anchor;
    double slide(double start, double min, double max) =>
        start.clamp(min, math.max(min, max)).toDouble();
    double alongY() => slide(
      at.center.dy <= area.center.dy ? at.top : at.bottom - childSize.height,
      room.top,
      room.bottom - childSize.height,
    );
    double alongX() => slide(
      at.center.dx <= area.center.dx ? at.left : at.right - childSize.width,
      room.left,
      room.right - childSize.width,
    );
    return switch (from) {
      AndroidBubbleEdge.right => Offset(room.right - childSize.width, alongY()),
      AndroidBubbleEdge.left => Offset(room.left, alongY()),
      AndroidBubbleEdge.top => Offset(alongX(), room.top),
      AndroidBubbleEdge.bottom => Offset(
        alongX(),
        room.bottom - childSize.height,
      ),
    };
  }

  @override
  bool shouldRelayout(AndroidMenuPlacement oldDelegate) =>
      area != oldDelegate.area ||
      cluster != oldDelegate.cluster ||
      edge != oldDelegate.edge ||
      width != oldDelegate.width ||
      features != oldDelegate.features;
}

/// Gives the cards opened from the session menu ([AndroidToolbarPanel]) a
/// back button that shows the menu again.
class _AndroidPanelBack extends InheritedWidget {
  const _AndroidPanelBack({required this.onBack, required super.child});
  final VoidCallback onBack;

  static VoidCallback? of(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<_AndroidPanelBack>()?.onBack;

  @override
  bool updateShouldNotify(_AndroidPanelBack oldWidget) => false;
}

/// The amber button group and the buttons inside a session panel follow the
/// device's text size up to 130 %. They are fixed targets beside the picture:
/// at 200 % their words break in the middle ("포인 / 터", "다시 연 / 결",
/// "Pointe / r") and the group loses its icons.
/// Titles, captions, notes and menu rows keep the device's full size.
const androidButtonTextScaleCap = 1.3;

/// The device's text size, no larger than [androidButtonTextScaleCap].
TextScaler androidButtonTextScaler(BuildContext context) => MediaQuery
    .textScalerOf(context)
    .clamp(maxScaleFactor: androidButtonTextScaleCap);

/// The session menus' one look on Android, after the Windows session
/// menus: every card ([AndroidToolbarPanel])
/// is the charcoal popup with a title and a hairline, groups get a small
/// muted caption ([AndroidMenuCaption]) or a hairline, and rows
/// ([AndroidBarMenuRow]) are 48 dp with a 20 dp icon, one short label, a
/// hint or value at the right and the chosen one in amber with a check.
abstract final class AndroidMenuStyle {
  /// Rows, captions and choice chips share this inset from the card edge.
  static const inset = 10.0;

  static TextStyle text(
    double size,
    Color color, {
    FontWeight weight = FontWeight.w500,
  }) => TextStyle(
    fontFamily: 'Pretendard',
    fontFamilyFallback: const ['Malgun Gothic'],
    fontSize: size,
    fontWeight: weight,
    color: color,
    height: 1.25,
    fontFeatures: const [FontFeature.tabularFigures()],
  );

  /// The hairline between a card's title and its rows, and between groups.
  static Widget divider(SessionChrome chrome) => Container(
    height: 1,
    margin: const EdgeInsets.symmetric(horizontal: 4, vertical: 6),
    color: chrome.popupLine,
  );
}

/// A group's small muted caption in a session menu card; [note] (e.g. what
/// the Host applied) follows it, fainter.
class AndroidMenuCaption extends StatelessWidget {
  const AndroidMenuCaption(this.label, {super.key, this.note});
  final String label;
  final String? note;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(
        AndroidMenuStyle.inset,
        8,
        AndroidMenuStyle.inset,
        4,
      ),
      child: Text.rich(
        TextSpan(
          children: [
            TextSpan(text: label),
            if (note != null)
              TextSpan(
                text: '  $note',
                style: TextStyle(
                  fontWeight: FontWeight.w600,
                  color: chrome.faint,
                ),
              ),
          ],
        ),
        style: AndroidMenuStyle.text(12, chrome.muted, weight: FontWeight.w600),
      ),
    );
  }
}

/// One row of a session menu card: a [SessionMenuItem] by finger (48 dp).
/// A heading is a caption (an empty one a hairline), a value without an
/// action is a measured value, and a greyed row says why on a tap.
class AndroidBarMenuRow extends StatelessWidget {
  const AndroidBarMenuRow({
    super.key,
    required this.item,
    required this.onSelected,
    this.opens = false,
    this.toggle = false,
    this.tooltip,
    this.description,
  });
  final SessionMenuItem item;
  final VoidCallback onSelected;

  /// The row turns something on and off: a switch at its end showing
  /// [SessionMenuItem.checked], instead of the chosen row's check.
  final bool toggle;

  /// The row opens a page or panel (a chevron at its end), like a Windows
  /// bar button with a drop-down.
  final bool opens;

  /// What it does in a few more words (a long press), as the Windows bar
  /// button's hover text.
  final String? tooltip;
  final String? description;

  bool get enabled => item.onSelected != null && !item.heading;

  /// A key combination ("Ctrl+V", "Win") is drawn as a key cap.
  static final _keys = RegExp(r'^(Win|Esc|Tab|[A-Z][a-z]*(\+[A-Za-z0-9]+)+)$');

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    const inset = AndroidMenuStyle.inset;
    if (item.heading) {
      return item.label.isEmpty
          ? AndroidMenuStyle.divider(chrome)
          : AndroidMenuCaption(item.label);
    }
    final value = item.value;
    if (value != null && item.onSelected == null && item.icon == null) {
      return Padding(
        key: item.key,
        padding: const EdgeInsets.symmetric(horizontal: inset, vertical: 10),
        child: Row(
          children: [
            Expanded(
              child: KeepAllText(
                item.label,
                style: AndroidMenuStyle.text(13.5, chrome.muted),
              ),
            ),
            const SizedBox(width: 12),
            KeepAllText(
              value,
              style: AndroidMenuStyle.text(
                13.5,
                chrome.ink,
                weight: FontWeight.w600,
              ),
            ),
          ],
        ),
      );
    }
    final chosen = item.checked && enabled && !toggle;
    final ink = !enabled
        ? chrome.faint
        : item.destructive
        ? chrome.danger
        : chrome.ink;
    final iconColor = !enabled
        ? chrome.faint
        : item.destructive
        ? chrome.danger
        : chosen
        ? chrome.accent
        : item.color ?? chrome.muted;
    final label = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        KeepAllText(
          item.label,
          style: AndroidMenuStyle.text(
            14,
            ink,
            weight: chosen ? FontWeight.w600 : FontWeight.w600,
          ),
        ),
        if (description != null) ...[
          const SizedBox(height: 3),
          KeepAllText(
            description!,
            style: AndroidMenuStyle.text(
              12,
              enabled ? chrome.muted : chrome.faint,
            ),
          ),
        ],
      ],
    );
    final Widget? hint = value == null
        ? null
        : _keys.hasMatch(value)
        ? Container(
            padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 3),
            decoration: BoxDecoration(
              color: chrome.popupHighlight,
              borderRadius: BorderRadius.circular(6),
              border: Border.all(color: chrome.popupLine),
            ),
            child: Text(
              value,
              style: AndroidMenuStyle.text(
                11.5,
                enabled ? chrome.muted : chrome.faint,
                weight: FontWeight.w600,
              ),
            ),
          )
        : KeepAllText(
            value,
            style: AndroidMenuStyle.text(
              12.5,
              enabled ? chrome.muted : chrome.faint,
            ),
          );
    // Large text or a narrow card: the hint goes under the label instead of
    // squeezing it.
    final large = MediaQuery.textScalerOf(context).scale(14) > 17.5;
    Widget content(bool stacked) => Material(
      color: chosen ? chrome.accent.withValues(alpha: .1) : Colors.transparent,
      borderRadius: BorderRadius.circular(12),
      child: InkWell(
        key: item.key,
        borderRadius: BorderRadius.circular(12),
        highlightColor: chrome.popupHighlight,
        splashFactory: NoSplash.splashFactory,
        onTap: enabled ? onSelected : null,
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: 48),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: inset, vertical: 6),
            child: Row(
              children: [
                SizedBox(
                  width: 24,
                  child: item.icon == null
                      ? null
                      : Icon(item.icon, size: 20, color: iconColor),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: stacked
                      ? Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          mainAxisSize: MainAxisSize.min,
                          children: [label, const SizedBox(height: 3), ?hint],
                        )
                      : label,
                ),
                if (hint != null && !stacked) ...[
                  const SizedBox(width: 10),
                  hint,
                ],
                // The chosen one: an amber check, as on Windows; a row that
                // opens a page: a chevron; a switch for one that turns
                // something on (the whole row is its target).
                if (toggle)
                  Padding(
                    padding: const EdgeInsets.only(left: 8),
                    child: ExcludeSemantics(
                      child: IgnorePointer(
                        child: Switch(
                          key: const Key('android-session-row-switch'),
                          value: item.checked,
                          onChanged: enabled ? (_) {} : null,
                          materialTapTargetSize:
                              MaterialTapTargetSize.shrinkWrap,
                          // The card's own colours in either app theme.
                          thumbColor: WidgetStateProperty.resolveWith(
                            (states) => states.contains(WidgetState.selected)
                                ? _amberInk
                                : enabled
                                ? chrome.muted
                                : chrome.faint,
                          ),
                          trackColor: WidgetStateProperty.resolveWith(
                            (states) => states.contains(WidgetState.selected)
                                ? chrome.accent
                                : chrome.popupHighlight,
                          ),
                          trackOutlineColor: WidgetStateProperty.resolveWith(
                            (states) => states.contains(WidgetState.selected)
                                ? Colors.transparent
                                : chrome.popupLine,
                          ),
                        ),
                      ),
                    ),
                  )
                else
                  SizedBox(
                    width: 30,
                    child: chosen
                        ? Align(
                            alignment: Alignment.centerRight,
                            child: Icon(
                              Icons.check_rounded,
                              size: 19,
                              color: chrome.accent,
                            ),
                          )
                        : opens
                        ? Align(
                            alignment: Alignment.centerRight,
                            child: Icon(
                              Icons.chevron_right_rounded,
                              size: 20,
                              color: enabled ? chrome.muted : chrome.faint,
                            ),
                          )
                        : null,
                  ),
              ],
            ),
          ),
        ),
      ),
    );
    final row = hint == null
        ? content(false)
        : LayoutBuilder(
            builder: (context, constraints) =>
                content(large || constraints.maxWidth < 270),
          );
    final reason = item.disabledReason;
    final hover = tooltip;
    return Semantics(
      button: true,
      enabled: enabled,
      selected: item.checked,
      child: !enabled && reason != null
          ? Tooltip(
              message: '${hover ?? item.label} · $reason',
              triggerMode: TooltipTriggerMode.tap,
              child: row,
            )
          : hover != null && hover != item.label
          ? Tooltip(message: hover, child: row)
          : row,
    );
  }
}

/// Three rising bars; [level] of them lit in [color] (the quality chip).
class AndroidSignalBars extends StatelessWidget {
  const AndroidSignalBars({
    super.key,
    required this.level,
    required this.color,
    required this.idle,
  });
  final int level;
  final Color color, idle;
  @override
  Widget build(BuildContext context) => CustomPaint(
    size: const Size(13, 13),
    painter: _SignalPainter(level, color, idle),
  );
}

class _SignalPainter extends CustomPainter {
  const _SignalPainter(this.level, this.color, this.idle);
  final int level;
  final Color color, idle;
  @override
  void paint(Canvas canvas, Size size) {
    const width = 3.0, gap = 2.0;
    for (var i = 0; i < 3; i++) {
      final height = size.height * (i + 2) / 4;
      canvas.drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromLTWH(i * (width + gap), size.height - height, width, height),
          const Radius.circular(1),
        ),
        Paint()..color = i < level ? color : idle.withValues(alpha: .45),
      );
    }
  }

  @override
  bool shouldRepaint(_SignalPainter old) =>
      old.level != level || old.color != color || old.idle != idle;
}

/// The session's amber: the colour the floating controls wear on any remote
/// picture.
const _amber = Color(0xFFF5A623);
const _amberInk = Color(0xFF241600);

/// A one-tap button of the floating cluster ([AndroidSessionBar.quick]):
/// an icon, a tiny [label] under it if any, lit while [active].
@immutable
class AndroidQuickButton {
  const AndroidQuickButton({
    required this.key,
    required this.icon,
    required this.tooltip,
    required this.onPressed,
    this.label,
    this.active = false,
    this.accented = false,
    this.onLongPress,
    this.focusNode,
  });
  final Key key;
  final IconData icon;
  final String tooltip;
  final String? label;
  final bool active;

  /// A small brand cue independent of the tool's current toggle state.
  final bool accented;
  final VoidCallback onPressed;

  /// A long press (입력 방식: its panel with 포인터 속도); the tooltip then
  /// stays with screen readers instead of taking the long press.
  final VoidCallback? onLongPress;
  final FocusNode? focusNode;
}

/// The folded tools: one amber rail (solid amber, not a charcoal rail with
/// amber icons) holding its 48 dp [buttons] in a column ([upright]) or a
/// row, with hairlines between them; the latency [dot] sits at the first
/// one's shoulder. A little larger while [dragging].
class AndroidBubbleCluster extends StatelessWidget {
  const AndroidBubbleCluster({
    super.key,
    required this.buttons,
    this.upright = true,
    this.dot,
    this.dragging = false,
  });
  final List<AndroidQuickButton> buttons;
  final bool upright, dragging;

  /// The measured latency's colour; null before a measurement.
  final Color? dot;

  @override
  Widget build(BuildContext context) {
    const size = androidBubbleSize, inset = 3.0;
    final direction = upright ? Axis.vertical : Axis.horizontal;
    // Hairlines between the buttons, drawn on the capsule; the buttons take
    // their whole 48 dp squares on top of it.
    final lines = <Widget>[
      for (var index = 0; index < buttons.length; index++) ...[
        if (index > 0)
          Container(
            width: upright ? size - 22 : 1,
            height: upright ? 1 : size - 22,
            color: _amberInk.withValues(alpha: .22),
          ),
        const Expanded(child: SizedBox.shrink()),
      ],
    ];
    return AnimatedScale(
      scale: dragging ? 1.06 : 1,
      duration: const Duration(milliseconds: 140),
      child: Stack(
        clipBehavior: Clip.none,
        children: [
          Positioned.fill(
            child: Padding(
              padding: const EdgeInsets.all(inset),
              child: DecoratedBox(
                decoration: BoxDecoration(
                  color: _amber,
                  borderRadius: BorderRadius.circular(14),
                  border: Border.all(color: _amberInk.withValues(alpha: .2)),
                  boxShadow: [
                    BoxShadow(
                      color: Colors.black.withValues(
                        alpha: dragging ? .4 : .26,
                      ),
                      blurRadius: dragging ? 16 : 10,
                      offset: const Offset(0, 3),
                    ),
                  ],
                ),
                child: Flex(
                  direction: direction,
                  crossAxisAlignment: CrossAxisAlignment.center,
                  children: lines,
                ),
              ),
            ),
          ),
          Positioned.fill(
            child: Flex(
              direction: direction,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                for (final button in buttons)
                  Expanded(child: _Cell(button: button)),
              ],
            ),
          ),
          // At the shoulder of the first button, ⋯.
          if (dot != null)
            Positioned(
              left: size - 18,
              top: 13,
              child: IgnorePointer(
                child: Container(
                  key: const Key('session-bubble-latency'),
                  width: 11,
                  height: 11,
                  decoration: BoxDecoration(
                    color: dot,
                    shape: BoxShape.circle,
                    border: Border.all(color: _amber, width: 2),
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }
}

class _Cell extends StatelessWidget {
  const _Cell({required this.button});
  final AndroidQuickButton button;

  @override
  Widget build(BuildContext context) {
    final label =
        button.label ??
        (button.icon == Icons.keyboard_rounded ||
                button.icon == Icons.keyboard_hide_rounded
            ? l10n.keyboard
            : null);
    final active = button.active;
    // Dark ink on the amber rail, whatever the tool's state.
    const ink = _amberInk;
    final labelStyle = TextStyle(
      fontFamily: 'Pretendard',
      // 9 dp inside the 48 dp cluster cells; 12 dp would drop the label at
      // large text.
      fontSize: 9,
      height: 1.1,
      fontWeight: FontWeight.w600,
      color: ink,
    );
    // The cell stays 48 dp, so its label follows the device's text size
    // only up to androidButtonTextScaleCap.
    final textScaler = androidButtonTextScaler(context);
    // One word (메뉴, 포인터, Keyboard) is one line in any script.
    final oneWord = label != null && !label.contains(' ');
    final content = LayoutBuilder(
      builder: (context, bounds) {
        var showLabel = false;
        var labelHeight = 0.0;
        if (label != null && bounds.maxWidth > 0) {
          final painter = TextPainter(
            text: TextSpan(
              text: label,
              style: DefaultTextStyle.of(context).style.merge(labelStyle),
            ),
            textDirection: Directionality.of(context),
            textScaler: textScaler,
            maxLines: oneWord ? 1 : null,
          )..layout(maxWidth: oneWord ? double.infinity : bounds.maxWidth);
          labelHeight = painter.height;
          showLabel =
              (oneWord ? painter.width : painter.minIntrinsicWidth) <=
                  bounds.maxWidth &&
              labelHeight <= bounds.maxHeight;
          painter.dispose();
        }
        final iconSize = label == null ? 21.0 : 18.0;
        final showIcon =
            !showLabel ||
            (bounds.maxWidth >= iconSize &&
                labelHeight + iconSize <= bounds.maxHeight);
        // The rail's orientation animation can briefly give a cell less
        // room. Its icon keeps the same action and accessible label; the
        // label returns as soon as the real cell has room for it.
        return Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            if (showIcon) Icon(button.icon, size: iconSize, color: ink),
            if (showLabel)
              // A word is never broken inside it ("Keyboar / d" in English,
              // "포인 / 터" at 200 % text): it stays on one line and, when the
              // cell is a hair narrower than the word, shrinks to fit.
              !oneWord
                  ? KeepAllText(
                      label!,
                      textAlign: TextAlign.center,
                      softWrap: true,
                      textScaler: textScaler,
                      style: labelStyle,
                    )
                  : Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 3),
                      child: FittedBox(
                        fit: BoxFit.scaleDown,
                        child: Text(
                          label,
                          key: const Key('session-bubble-label-one-word'),
                          textAlign: TextAlign.center,
                          maxLines: 1,
                          softWrap: false,
                          textScaler: textScaler,
                          style: labelStyle,
                        ),
                      ),
                    ),
          ],
        );
      },
    );
    final longPress = button.onLongPress;
    return Semantics(
      button: true,
      toggled: button.active,
      label: button.tooltip,
      excludeSemantics: true,
      onTap: button.onPressed,
      onLongPress: longPress,
      child: Tooltip(
        message: button.tooltip,
        excludeFromSemantics: true,
        triggerMode: longPress == null ? null : TooltipTriggerMode.manual,
        child: Material(
          type: MaterialType.transparency,
          child: InkWell(
            key: button.key,
            focusNode: button.focusNode,
            borderRadius: BorderRadius.circular(12),
            highlightColor: _amberInk.withValues(alpha: .12),
            splashFactory: NoSplash.splashFactory,
            onTap: () {
              unawaited(HapticFeedback.selectionClick());
              button.onPressed();
            },
            onLongPress: longPress == null
                ? null
                : () {
                    unawaited(HapticFeedback.mediumImpact());
                    longPress();
                  },
            // Only the tool that is on gets a darker tonal mark.
            child: Center(
              child: AnimatedContainer(
                duration: const Duration(milliseconds: 150),
                width: 40,
                height: 40,
                decoration: BoxDecoration(
                  borderRadius: BorderRadius.circular(10),
                  color: active
                      ? _amberInk.withValues(alpha: .16)
                      : Colors.transparent,
                ),
                child: content,
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// A round handle that moves the visible part of a zoomed picture: dragging
/// it pans, nothing is sent. TalkBack gets four "보기" actions.
class AndroidMoveHandle extends StatelessWidget {
  const AndroidMoveHandle({super.key, required this.onPan, required this.size});
  final ValueChanged<Offset> onPan;
  final Size size;

  static const diameter = 48.0;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    return Semantics(
      label: l10n.screenPanHandleDragMove,
      customSemanticsActions: {
        for (final (label, delta) in [
          (l10n.lookLeft, Offset(size.width * .2, 0)),
          (l10n.lookUp, Offset(0, size.height * .2)),
          (l10n.lookDown, Offset(0, -size.height * .2)),
          (l10n.lookRight, Offset(-size.width * .2, 0)),
        ])
          CustomSemanticsAction(label: label): () => onPan(delta),
      },
      child: GestureDetector(
        key: const Key('android-session-move-handle'),
        behavior: HitTestBehavior.opaque,
        onPanUpdate: (details) => onPan(details.delta),
        child: Container(
          width: diameter,
          height: diameter,
          decoration: BoxDecoration(
            color: _amber.withValues(alpha: .92),
            shape: BoxShape.circle,
            boxShadow: [BoxShadow(color: chrome.shadow, blurRadius: 12)],
          ),
          child: const Icon(
            Icons.open_with_rounded,
            size: 22,
            color: _amberInk,
          ),
        ),
      ),
    );
  }
}

/// The card a toolbar command or menu opens (화면, 입력 방식, 키 조합, ...):
/// a title, a hairline, then rows, captions and choices ([AndroidMenuStyle]).
class AndroidToolbarPanel extends StatelessWidget {
  const AndroidToolbarPanel({
    super.key,
    required this.title,
    required this.children,
    this.trailing,
    this.showTitle = true,
    this.besideSplit,
    this.footer,
  });
  final String title;
  final Widget? trailing;
  final List<Widget> children;

  /// Actions remain visible while the choices scroll within a bounded popup.
  /// An unconstrained parent keeps natural sizing and owns the outer scrolling.
  final Widget? footer;

  /// False where the row is better spent on choices (a phone on its side):
  /// the title stays for screen readers; Back and ⋯ still lead back.
  final bool showTitle;

  /// On a phone on its side ([androidPanelBeside]): the first this many
  /// [children] in a left column, the rest beside them, without the title
  /// row, so the card fits the height instead of scrolling (설정's 재연결
  /// was out of sight).
  final int? besideSplit;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    final split = besideSplit;
    if (split != null && androidPanelBeside(MediaQuery.sizeOf(context))) {
      Widget column(List<Widget> part) => Expanded(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: part,
        ),
      );
      return AndroidToolbarPanel(
        title: title,
        showTitle: false,
        footer: footer,
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              column(children.sublist(0, split)),
              const SizedBox(width: 8),
              column(children.sublist(split)),
            ],
          ),
        ],
      );
    }
    // Opened from the session menu: a back button shows the menu again.
    final back = _AndroidPanelBack.of(context);
    Widget withFooter(Widget content) {
      final actions = footer;
      if (actions == null) return content;
      final bottom = Padding(
        padding: const EdgeInsets.fromLTRB(6, 0, 6, 8),
        child: actions,
      );
      return LayoutBuilder(
        builder: (context, bounds) => Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            if (bounds.hasBoundedHeight) Flexible(child: content) else content,
            bottom,
          ],
        ),
      );
    }

    return Material(
      color: chrome.surface,
      elevation: 0,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: chrome.popupEdge),
      ),
      shadowColor: chrome.shadow,
      clipBehavior: Clip.antiAlias,
      child: withFooter(
        SingleChildScrollView(
          padding: EdgeInsets.fromLTRB(6, 4, 6, footer == null ? 8 : 0),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              if (!showTitle)
                Semantics(header: true, label: title, child: const SizedBox()),
              if (showTitle) ...[
                ConstrainedBox(
                  constraints: const BoxConstraints(minHeight: 40),
                  child: Padding(
                    padding: EdgeInsets.only(
                      left: back == null ? AndroidMenuStyle.inset : 0,
                      right: AndroidMenuStyle.inset,
                    ),
                    child: Row(
                      children: [
                        if (back != null)
                          IconButton(
                            key: const Key('session-menu-back'),
                            tooltip: l10n.back,
                            onPressed: back,
                            style: IconButton.styleFrom(
                              fixedSize: const Size(48, 48),
                              minimumSize: const Size(48, 48),
                              foregroundColor: chrome.icon,
                              splashFactory: NoSplash.splashFactory,
                              highlightColor: chrome.pressed,
                              shape: RoundedRectangleBorder(
                                borderRadius: BorderRadius.circular(12),
                              ),
                            ),
                            icon: const Icon(
                              Icons.chevron_left_rounded,
                              size: 22,
                            ),
                          ),
                        Expanded(
                          child: Text(
                            title,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: AndroidMenuStyle.text(
                              16,
                              chrome.ink,
                              weight: FontWeight.w600,
                            ),
                          ),
                        ),
                        ?trailing,
                      ],
                    ),
                  ),
                ),
                AndroidMenuStyle.divider(chrome),
              ],
              ...children,
            ],
          ),
        ),
      ),
    );
  }
}

/// A labelled row of choices or actions inside [AndroidToolbarPanel]; every
/// item is at least 48 dp tall with a pressed state.
class AndroidPanelRow extends StatelessWidget {
  const AndroidPanelRow({super.key, this.label, required this.items});
  final String? label;
  final List<AndroidPanelItem> items;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 6),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (label != null) AndroidMenuCaption(label!),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: Row(
              children: [
                for (final (index, item) in items.indexed) ...[
                  if (index > 0) const SizedBox(width: 6),
                  Expanded(
                    flex: item.flex,
                    child: _PanelButton(item: item, tonal: true),
                  ),
                ],
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// A labelled set of choices inside [AndroidToolbarPanel] that wraps onto
/// more lines (화면 배율 has nine): as many [minWidth] items per line as fit,
/// sharing the line's width. [note] (e.g. what the Host applied) sits after
/// the label.
class AndroidPanelChoices extends StatelessWidget {
  const AndroidPanelChoices({
    super.key,
    required this.label,
    required this.items,
    this.note,
    this.minWidth = 52,
  });
  final String label;
  final String? note;
  final List<AndroidPanelItem> items;
  final double minWidth;

  @override
  Widget build(BuildContext context) {
    const spacing = 6.0;
    // As the buttons draw their text (androidButtonTextScaleCap).
    final textScaler = androidButtonTextScaler(context);
    double measure(String? text, double size, FontWeight weight) {
      if (text == null) return 0;
      final painter = TextPainter(
        text: TextSpan(
          text: text,
          style: TextStyle(
            fontFamily: 'Pretendard',
            fontSize: size,
            fontWeight: weight,
            fontFeatures: const [FontFeature.tabularFigures()],
          ),
        ),
        textScaler: textScaler,
        textDirection: Directionality.of(context),
        maxLines: 1,
      )..layout();
      final width = painter.width;
      painter.dispose();
      return width;
    }

    final readableWidth = items.fold<double>(
      minWidth,
      (width, item) => math.max(
        width,
        math.max(
              measure(item.label, 14, FontWeight.w600),
              measure(item.detail, 11, FontWeight.w500),
            ) +
            (item.icon == null ? 0 : 25) +
            16,
      ),
    );
    return Padding(
      padding: const EdgeInsets.only(top: 6),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          AndroidMenuCaption(label, note: note),
          LayoutBuilder(
            builder: (context, constraints) {
              final width = constraints.maxWidth - 8;
              final perLine = width.isFinite
                  ? ((width + spacing) / (readableWidth + spacing))
                        .floor()
                        .clamp(1, items.length)
                  : items.length;
              final itemWidth = width.isFinite
                  ? (width - spacing * (perLine - 1)) / perLine
                  : readableWidth;
              return Container(
                margin: const EdgeInsets.symmetric(horizontal: 4),
                decoration: BoxDecoration(
                  color: SessionChrome.of(context).popupHighlight,
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Wrap(
                  spacing: spacing,
                  runSpacing: spacing,
                  children: [
                    for (final item in items)
                      SizedBox(
                        width: itemWidth,
                        child: _PanelButton(item: item),
                      ),
                  ],
                ),
              );
            },
          ),
        ],
      ),
    );
  }
}

@immutable
class AndroidPanelItem {
  const AndroidPanelItem({
    required this.key,
    this.label,
    this.icon,
    this.onPressed,
    this.selected = false,
    this.flex = 1,
    this.tooltip,
    this.detail,
    this.primary = false,
  });
  final Key key;
  final String? label, tooltip;

  /// A second, smaller line under [label] (e.g. a size).
  final String? detail;
  final IconData? icon;
  final VoidCallback? onPressed;
  final bool selected;

  /// A presentation-only primary action, used for applying settings.
  final bool primary;
  final int flex;
}

class _PanelButton extends StatelessWidget {
  const _PanelButton({required this.item, this.tonal = false});
  final AndroidPanelItem item;
  final bool tonal;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    final enabled = item.onPressed != null;
    final primary = item.primary && enabled;
    final color = !enabled
        ? chrome.faint
        : primary
        ? const Color(0xFF241600)
        : item.selected
        ? chrome.accent
        : chrome.ink;
    final textScaler = androidButtonTextScaler(context);
    final content = Row(
      mainAxisSize: MainAxisSize.min,
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        if (item.icon != null) Icon(item.icon, size: 19, color: color),
        if (item.icon != null && item.label != null) const SizedBox(width: 6),
        if (item.label != null)
          Flexible(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                // Korean wraps between words, never inside one.
                KeepAllText(
                  item.label!,
                  textAlign: TextAlign.center,
                  textScaler: textScaler,
                  style: AndroidMenuStyle.text(
                    14,
                    color,
                    weight: item.selected ? FontWeight.w600 : FontWeight.w600,
                  ),
                ),
                if (item.detail != null)
                  Text(
                    item.detail!,
                    textAlign: TextAlign.center,
                    textScaler: textScaler,
                    style: TextStyle(
                      fontFamily: 'Pretendard',
                      fontSize: 12,
                      fontWeight: FontWeight.w500,
                      color: enabled ? chrome.muted : chrome.faint,
                      fontFeatures: const [FontFeature.tabularFigures()],
                    ),
                  ),
              ],
            ),
          ),
      ],
    );
    // Choices share one neutral group surface. Only the current value has
    // an amber tint; a primary action receives the solid accent.
    final button = Material(
      color: primary
          ? chrome.accent
          : item.selected
          ? chrome.accent.withValues(alpha: .14)
          : tonal
          ? chrome.popupHighlight
          : Colors.transparent,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(10),
        side: BorderSide(
          color: item.selected && !primary
              ? chrome.accent.withValues(alpha: .5)
              : Colors.transparent,
        ),
      ),
      child: InkWell(
        key: item.key,
        borderRadius: BorderRadius.circular(10),
        highlightColor: chrome.pressed,
        onTap: enabled
            ? () {
                unawaited(HapticFeedback.selectionClick());
                item.onPressed!();
              }
            : null,
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: 48),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 6),
            child: Center(child: content),
          ),
        ),
      ),
    );
    final tip = item.tooltip;
    // A greyed item says why on a tap (no hover by finger).
    return tip == null
        ? button
        : Tooltip(
            message: tip,
            triggerMode: enabled ? null : TooltipTriggerMode.tap,
            child: button,
          );
  }
}
