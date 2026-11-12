import 'dart:async';

import 'package:flutter/material.dart';

import '../data/notices.dart';
import 'app_dialog.dart' show AppButtons, AppTone, showAppConfirm;
import 'app_menu.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'primary_region.dart';
import 'workspace_header.dart' show HeaderControl;
import '../l10n/l10n.dart';

/// 공지 on screen (notice-v1, lib/data/notices.dart): the banner that rises
/// when a notice arrives, the bell with its unread count, the list behind the
/// bell, and the same banner over a stream.
///
/// A notice is shown as plain text. Nothing in it is linked or opened.
/// The banner never asks for keyboard focus: it sits under [ExcludeFocus] and
/// is closed with a click or by itself after a while.

/// The colours of a notice card: the app's surface in the launcher, a fixed
/// dark one over a stream (readable on any picture, in either appearance).
class _NoticeTone {
  const _NoticeTone({
    required this.surface,
    required this.edge,
    required this.ink,
    required this.muted,
    required this.accent,
    required this.accentInk,
    required this.chip,
    required this.shadow,
  });
  final Color surface, edge, ink, muted, accent, accentInk, chip, shadow;

  factory _NoticeTone.of(BuildContext context, {required bool onStream}) {
    final p = AppPalette.of(context);
    if (onStream) {
      return const _NoticeTone(
        surface: Color(0xF5141C21),
        edge: Color(0x38FFFFFF),
        ink: Color(0xFFE9F1F3),
        muted: Color(0xFFA9BAC2),
        accent: Color(0xFFF5A623),
        accentInk: Color(0xFFF5A623),
        chip: Color(0x29F5A623),
        shadow: Color(0xB3000000),
      );
    }
    return _NoticeTone(
      surface: p.surface,
      edge: p.line,
      ink: p.ink,
      muted: p.muted,
      accent: p.amber,
      accentInk: p.amberInk,
      chip: p.amberSoft,
      shadow: p.shadow,
    );
  }
}

/// One notice as a card: what kind and when, its title, its lines.
class NoticeCard extends StatelessWidget {
  const NoticeCard({
    super.key,
    required this.notice,
    required this.now,
    this.onStream = false,
    this.bodyLines,
    this.waiting = 0,
    this.closable = false,
    this.unread = false,
    this.flat = false,
  });
  final BrokerNotice notice;
  final DateTime now;

  /// Over a stream: the fixed dark surface.
  final bool onStream;

  /// Cuts a long body in the banner; null shows all of it (the list).
  final int? bodyLines;

  /// Banners that wait behind this one.
  final int waiting;

  /// Shows the small close mark of a banner.
  final bool closable;

  /// The amber dot of a notice not yet seen (the list).
  final bool unread;

  /// A row of the list: no surface, edge or shadow of its own.
  final bool flat;

  @override
  Widget build(BuildContext context) {
    final tone = _NoticeTone.of(context, onStream: onStream);
    final maintenance = notice.kind == NoticeKind.maintenance;
    final when = maintenance ? noticeWhen(notice, now) : null;
    final content = Padding(
      padding: flat
          ? const EdgeInsets.symmetric(horizontal: 14, vertical: 12)
          : const EdgeInsets.fromLTRB(14, 12, 12, 13),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                maintenance
                    ? Icons.schedule_rounded
                    : Icons.campaign_outlined,
                size: 17,
                color: tone.accentInk,
              ),
              const SizedBox(width: 6),
              Expanded(
                child: Row(
                  children: [
              Flexible(
                child: Container(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 7,
                    vertical: 2,
                  ),
                  decoration: BoxDecoration(
                    color: maintenance ? tone.chip : Colors.transparent,
                    borderRadius: BorderRadius.circular(999),
                  ),
                  child: KeepAllText(
                    maintenance
                        ? (when == null ? l10n.plannedMaintenance : l10n.plannedMaintenance2(when))
                        : l10n.notice2,
                    key: const Key('notice-kind'),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: uiText(
                      size: 12,
                      weight: FontWeight.w600,
                      color: maintenance ? tone.accentInk : tone.muted,
                    ),
                  ),
                ),
              ),
              const SizedBox(width: 8),
              if (!maintenance || flat)
                Text(
                  noticeAge(notice.sentAt, now),
                  key: const Key('notice-age'),
                  maxLines: 1,
                  style: uiText(size: 12, color: tone.muted),
                ),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              if (unread)
                Container(
                  key: const Key('notice-unread-dot'),
                  width: 8,
                  height: 8,
                  decoration: BoxDecoration(
                    color: tone.accent,
                    shape: BoxShape.circle,
                  ),
                ),
              if (closable)
                Icon(
                  Icons.close_rounded,
                  key: const Key('notice-close'),
                  size: 17,
                  color: tone.muted,
                ),
            ],
          ),
          const SizedBox(height: 7),
          KeepAllText(
            notice.title,
            key: const Key('notice-title'),
            style: uiText(
              size: 14,
              weight: FontWeight.w600,
              color: tone.ink,
              height: 1.35,
            ),
          ),
          if (notice.body.isNotEmpty) ...[
            const SizedBox(height: 4),
            KeepAllText(
              notice.body,
              key: const Key('notice-body'),
              maxLines: bodyLines,
              overflow: bodyLines == null ? null : TextOverflow.ellipsis,
              style: uiText(size: 13, color: tone.muted, height: 1.5),
            ),
          ],
          if (waiting > 0) ...[
            const SizedBox(height: 8),
            KeepAllText(
              l10n.text24(waiting),
              key: const Key('notice-waiting'),
              style: uiText(size: 12, color: tone.muted),
            ),
          ],
        ],
      ),
    );
    if (flat) return content;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: tone.surface,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: tone.edge),
        boxShadow: [
          BoxShadow(
            color: tone.shadow,
            blurRadius: 18,
            offset: const Offset(0, 6),
          ),
        ],
      ),
      child: ClipRRect(
        borderRadius: BorderRadius.circular(12),
        child: IntrinsicHeight(
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              // The amber edge that marks a notice among the app's own cards.
              ColoredBox(color: tone.accent, child: const SizedBox(width: 3)),
              Expanded(child: content),
            ],
          ),
        ),
      ),
    );
  }
}

/// The banner of the notice that arrived. It rises by itself, goes down by
/// itself after [noticeBannerDuration] (not while the pointer rests on it, and
/// not while the window cannot be seen: [active] false, or the app hidden or
/// paused, when no frame would tell it), and a click closes it as seen. It takes no
/// keyboard focus and nothing outside its own card takes its clicks.
class NoticeBanner extends StatefulWidget {
  const NoticeBanner({
    super.key,
    required this.center,
    this.onStream = false,
    this.active = true,
    this.maxWidth = 380,
    this.clock = DateTime.now,
    this.duration = noticeBannerDuration,
    this.fromBelow = false,
  });
  final NoticeCenter center;
  final bool onStream;

  /// It rises from below (a bottom corner) instead of dropping from above.
  final bool fromBelow;

  /// False while the window cannot be seen: the banner waits instead of
  /// spending its time unseen.
  final bool active;
  final double maxWidth;
  final DateTime Function() clock;
  final Duration Function(BrokerNotice) duration;

  @override
  State<NoticeBanner> createState() => _NoticeBannerState();
}

class _NoticeBannerState extends State<NoticeBanner>
    with WidgetsBindingObserver {
  String? _shown;
  Timer? _lower, _minute;
  bool _hovered = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    widget.center.addListener(_changed);
    // "5분 뒤" follows the clock while a banner is up.
    _minute = Timer.periodic(const Duration(seconds: 20), (_) {
      if (mounted && widget.center.banner != null) setState(() {});
    });
    _arm();
  }

  @override
  void didUpdateWidget(NoticeBanner old) {
    super.didUpdateWidget(old);
    if (old.center != widget.center) {
      old.center.removeListener(_changed);
      widget.center.addListener(_changed);
      _shown = null;
    }
    _arm();
  }

  /// Whether the banner can be seen now. Read from the binding, not from a
  /// rebuild: a hidden or paused app draws no frame that could say so.
  bool get _visible {
    final state = WidgetsBinding.instance.lifecycleState;
    return widget.active &&
        (state == null ||
            state == AppLifecycleState.resumed ||
            state == AppLifecycleState.inactive);
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (mounted) _arm();
  }

  void _changed() {
    if (!mounted) return;
    _arm();
    setState(() {});
  }

  void _arm() {
    final banner = widget.center.banner, visible = _visible;
    if (banner?.id == _shown && _lower != null && visible) return;
    _lower?.cancel();
    _lower = null;
    _shown = banner?.id;
    // Unseen, it keeps its whole time for when the window is back.
    if (banner == null || !visible) {
      _shown = null;
      return;
    }
    _lower = Timer(widget.duration(banner), () => _timeUp(banner.id));
  }

  void _timeUp(String id) {
    _lower = null;
    if (!mounted) return;
    if (!_visible) {
      _shown = null;
      return;
    }
    // Being read: look again shortly.
    if (_hovered) {
      _lower = Timer(const Duration(seconds: 2), () => _timeUp(id));
      return;
    }
    widget.center.lower(id);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    widget.center.removeListener(_changed);
    _lower?.cancel();
    _minute?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final banner = widget.center.banner;
    return ExcludeFocus(
      child: AnimatedSwitcher(
        duration: MediaQuery.disableAnimationsOf(context)
            ? Duration.zero
            : const Duration(milliseconds: 180),
        switchInCurve: Curves.easeOutCubic,
        switchOutCurve: Curves.easeInCubic,
        transitionBuilder: (child, animation) => FadeTransition(
          opacity: animation,
          child: SlideTransition(
            position: Tween(
              begin: Offset(0, widget.fromBelow ? .08 : -.08),
              end: Offset.zero,
            ).animate(animation),
            child: child,
          ),
        ),
        child: banner == null
            ? const SizedBox.shrink(key: ValueKey('notice-banner-none'))
            : ConstrainedBox(
                key: ValueKey('notice-banner-${banner.id}'),
                constraints: BoxConstraints(maxWidth: widget.maxWidth),
                child: MouseRegion(
                  cursor: SystemMouseCursors.click,
                  onEnter: (_) => _hovered = true,
                  onExit: (_) => _hovered = false,
                  child: Semantics(
                    container: true,
                    liveRegion: true,
                    button: true,
                    label: l10n.closeNotice,
                    child: GestureDetector(
                      key: const Key('notice-banner'),
                      behavior: HitTestBehavior.opaque,
                      onTap: () => widget.center.dismiss(banner.id),
                      // Its own text defaults: the banner may sit above
                      // every page, outside any Material.
                      child: Material(
                        type: MaterialType.transparency,
                        child: NoticeCard(
                          notice: banner,
                          now: widget.clock(),
                          onStream: widget.onStream,
                          bodyLines: 6,
                          waiting: widget.center.waiting,
                          closable: true,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
      ),
    );
  }
}

/// The header's bell: a quiet button, with the number of unread notices on
/// its corner. It opens the list under itself and that marks them seen.
class NoticeBell extends StatelessWidget {
  const NoticeBell({
    super.key,
    required this.center,
    this.style,
    this.clock = DateTime.now,
    this.iconSize = HeaderControl.icon,
    this.touch = false,
  });
  final NoticeCenter center;

  /// A touch screen: the list's 지우기 controls are full touch targets.
  final bool touch;

  /// The surrounding header's button style (Android brings its own).
  final ButtonStyle? style;
  final DateTime Function() clock;
  final double iconSize;

  Future<void> _open(BuildContext context) async {
    final box = context.findRenderObject() as RenderBox?;
    if (box == null || !box.hasSize) return;
    final anchor = box.localToGlobal(Offset.zero) & box.size;
    // What was unread when the list opened keeps its dot while it is open.
    final fresh = {
      for (final notice in center.notices)
        if (!notice.seen) notice.id,
    };
    center.markAllSeen();
    await Navigator.of(context).push(
      AnchoredPanelRoute<void>(
        anchor: anchor,
        width: 380,
        label: l10n.notice2,
        // 지우기 and 모두 지우기 ask first; the list stays open behind it.
        keepOpenForDialog: true,
        builder: (_) => NoticeList(
          center: center,
          fresh: fresh,
          clock: clock,
          touch: touch,
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: center,
    builder: (context, _) {
      final p = AppPalette.of(context);
      final unread = center.unread;
      final button = IconButton(
        key: const Key('notice-bell'),
        tooltip: unread > 0 ? l10n.text25(unread) : l10n.notice2,
        onPressed: () => unawaited(_open(context)),
        style: style ?? HeaderControl.quiet(p),
        icon: Icon(
          unread > 0
              ? Icons.notifications_rounded
              : Icons.notifications_none_rounded,
          size: iconSize,
        ),
      );
      if (unread == 0) return button;
      return Stack(
        clipBehavior: Clip.none,
        children: [
          button,
          Positioned(
            top: -4,
            right: -4,
            child: IgnorePointer(
              child: Container(
                key: const Key('notice-bell-count'),
                constraints: const BoxConstraints(minWidth: 18, minHeight: 18),
                padding: const EdgeInsets.symmetric(horizontal: 5),
                alignment: Alignment.center,
                decoration: BoxDecoration(
                  color: p.amber,
                  borderRadius: BorderRadius.circular(999),
                  border: Border.all(
                    color: p.headerGlass.withValues(alpha: 1),
                    width: 2,
                  ),
                ),
                child: Text(
                  unread > 9 ? '9+' : '$unread',
                  textScaler: TextScaler.noScaling,
                  style: uiText(
                    size: 11,
                    weight: FontWeight.w700,
                    color: p.onAmber,
                    height: 1,
                  ),
                ),
              ),
            ),
          ),
        ],
      );
    },
  );
}

/// The list behind the bell: every notice that still shows, newest first.
/// Where the broker can do it ([NoticeCenter.canRemove]) each row has a small
/// 지우기 and the header 모두 지우기: after one question, the
/// notice leaves this account's list on every device. With an older broker
/// neither control is there.
class NoticeList extends StatelessWidget {
  const NoticeList({
    super.key,
    required this.center,
    this.fresh = const {},
    this.clock = DateTime.now,
    this.touch = false,
  });
  final NoticeCenter center;

  /// A touch screen: 48 dp targets instead of the pointer's compact ones.
  final bool touch;

  /// The notices that were unread when the list opened.
  final Set<String> fresh;
  final DateTime Function() clock;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: center,
    builder: (context, _) {
      final p = AppPalette.of(context);
      final notices = center.notices, now = clock();
      final removable = center.canRemove && notices.isNotEmpty;
      // Asked once: the notice is gone from every device of this account.
      Future<void> removeOne(BrokerNotice notice) async {
        final title = notice.title.trim();
        final agreed = await showAppConfirm(
          context,
          dialogKey: const Key('notice-remove-confirm'),
          confirmKey: const Key('notice-remove-accept'),
          cancelKey: const Key('notice-remove-cancel'),
          icon: Icons.delete_outline_rounded,
          tone: AppTone.danger,
          routeSettings: AnchoredPanelRoute.ownedDialogSettings(context),
          title: l10n.deleteNotice,
          subtitle: title.isEmpty ? null : title,
          message: l10n.deletedEveryDeviceSignedAccount,
          confirmLabel: l10n.clear,
        );
        if (agreed && context.mounted) center.remove([notice.id]);
      }

      Future<void> removeAll() async {
        final agreed = await showAppConfirm(
          context,
          dialogKey: const Key('notice-remove-confirm'),
          confirmKey: const Key('notice-remove-accept'),
          cancelKey: const Key('notice-remove-cancel'),
          icon: Icons.delete_sweep_outlined,
          tone: AppTone.danger,
          routeSettings: AnchoredPanelRoute.ownedDialogSettings(context),
          title: l10n.text26(notices.length),
          message: l10n.deletedEveryDeviceSignedAccount,
          confirmLabel: l10n.deleteAll,
        );
        if (agreed && context.mounted) center.removeAll();
      }

      Widget remove(BrokerNotice notice) => IconButton(
        key: ValueKey('notice-remove-${notice.id}'),
        tooltip: l10n.clear,
        onPressed: () => unawaited(removeOne(notice)),
        style: IconButton.styleFrom(
          foregroundColor: p.muted,
          minimumSize: Size.square(touch ? 48 : 28),
          maximumSize: Size.square(touch ? 48 : 28),
          padding: EdgeInsets.zero,
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          visualDensity: VisualDensity.standard,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(8),
          ),
        ).copyWith(side: AppButtons.focusRing(p.focus)),
        icon: Icon(Icons.close_rounded, size: touch ? 20 : 16),
      );
      return ConstrainedBox(
        constraints: const BoxConstraints(maxHeight: 460),
        child: Column(
          key: const Key('notice-list'),
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Padding(
              // 모두 지우기 fills the header's height; without it the title
              // keeps its own padding.
              padding: removable
                  ? EdgeInsets.fromLTRB(14, touch ? 0 : 5, touch ? 4 : 8, touch ? 0 : 3)
                  : const EdgeInsets.fromLTRB(14, 12, 14, 10),
              child: Row(
                children: [
                  Text(
                    l10n.notice2,
                    style: uiText(
                      size: 14,
                      weight: FontWeight.w600,
                      color: p.ink,
                    ),
                  ),
                  const SizedBox(width: 8),
                  if (notices.isNotEmpty)
                    Text(
                      '${notices.length}',
                      key: const Key('notice-list-count'),
                      style: uiText(
                        size: 12,
                        weight: FontWeight.w600,
                        color: p.muted,
                      ),
                    ),
                  const Spacer(),
                  if (removable)
                    TextButton(
                      key: const Key('notice-remove-all'),
                      onPressed: () => unawaited(removeAll()),
                      style: AppButtons.link(p).copyWith(
                        minimumSize: WidgetStatePropertyAll(
                          Size(0, touch ? 48 : 30),
                        ),
                        padding: const WidgetStatePropertyAll(
                          EdgeInsets.symmetric(horizontal: 10),
                        ),
                        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                        visualDensity: VisualDensity.standard,
                        textStyle: WidgetStatePropertyAll(
                          uiText(size: 13, weight: FontWeight.w600),
                        ),
                      ),
                      child: Text(l10n.deleteAll),
                    ),
                ],
              ),
            ),
            Divider(height: 1, color: p.line),
            if (notices.isEmpty)
              Padding(
                padding: const EdgeInsets.fromLTRB(14, 22, 14, 24),
                child: KeepAllText(
                  l10n.noNotices,
                  key: const Key('notice-list-empty'),
                  textAlign: TextAlign.center,
                  style: uiText(size: 13, color: p.muted),
                ),
              )
            else
              Flexible(
                child: ListView.separated(
                  shrinkWrap: true,
                  padding: EdgeInsets.zero,
                  itemCount: notices.length,
                  separatorBuilder: (_, _) =>
                      Divider(height: 1, color: p.line.withValues(alpha: .6)),
                  itemBuilder: (context, index) {
                    final notice = notices[index];
                    final card = NoticeCard(
                      key: ValueKey('notice-row-${notice.id}'),
                      notice: notice,
                      now: now,
                      flat: true,
                      unread: fresh.contains(notice.id),
                    );
                    if (!removable) return card;
                    // The 지우기 mark in its own narrow column at the row's
                    // top-right: the text keeps its lines.
                    return Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Expanded(child: card),
                        Padding(
                          padding: EdgeInsets.only(
                            top: touch ? 0 : 6,
                            right: touch ? 0 : 8,
                          ),
                          child: remove(notice),
                        ),
                      ],
                    );
                  },
                ),
              ),
          ],
        ),
      );
    },
  );
}

/// Where a banner sits in a window that may span two monitors: the top-right
/// corner of the primary monitor's part ([PrimaryRegionScope]), which is the
/// whole window with one monitor and in full screen. The rest of the window
/// passes every pointer through.
class NoticeCorner extends StatelessWidget {
  const NoticeCorner({
    super.key,
    required this.child,
    this.top = 16,
    this.side = 16,
  });
  final Widget child;
  final double top, side;

  @override
  Widget build(BuildContext context) {
    final region = PrimaryRegionScope.maybeOf(context);
    Widget place(Rect? area) {
      final size = MediaQuery.sizeOf(context);
      final box = (area ?? Offset.zero & size).intersect(Offset.zero & size);
      final within = box.isEmpty ? Offset.zero & size : box;
      return Stack(
        children: [
          Positioned(
            top: within.top + top,
            right: size.width - within.right + side,
            // Never wider than the part it sits in.
            width: (within.width - 2 * side).clamp(0.0, 380.0).toDouble(),
            child: Align(alignment: Alignment.topRight, child: child),
          ),
        ],
      );
    }

    if (region == null) return place(null);
    return ValueListenableBuilder<Rect?>(
      valueListenable: region,
      builder: (context, area, _) => place(area),
    );
  }
}
