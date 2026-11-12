import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/product_updates.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'release_notes.dart';
import '../l10n/l10n.dart';

/// Opens the release history ("변경 내역") of [product] from the configured
/// server: every version it ever published, newest first, loaded page by
/// page. [api] reuses an open update check's connection. Only notes are
/// read; nothing is downloaded or installed here.
Future<void> showReleaseHistory(
  BuildContext context, {
  required String server,
  required String product,
  required String currentVersion,
  ProductUpdates? api,
}) => showDialog<void>(
  context: context,
  builder: (_) => ReleaseHistoryPanel(
    server: server,
    product: product,
    currentVersion: currentVersion,
    api: api,
  ),
);

/// 버전별: a version sidebar beside the selected version's notes.
/// 전체 보기: every note in one long scroll under sticky version headers.
enum ReleaseHistoryView { versions, all }

/// The history panel: wide windows get [ReleaseHistoryView.versions] (or
/// 전체 보기 on request); narrow ones a single scrolling timeline.
class ReleaseHistoryPanel extends StatefulWidget {
  const ReleaseHistoryPanel({
    super.key,
    required this.server,
    required this.product,
    required this.currentVersion,
    this.api,
    this.pageSize = 40,
    this.initialView = ReleaseHistoryView.versions,
    this.loadPage,
    this.compare,
    this.productLabel,
  });
  final String server, product, currentVersion;
  final ProductUpdates? api;

  /// Another product's feed (the Android app's): pages by [before], newest
  /// first, replacing [api]; [compare] orders its versions and
  /// [productLabel] names it.
  final Future<ReleaseHistoryPage> Function({String? before, int limit})?
  loadPage;
  final int Function(String a, String b)? compare;
  final String? productLabel;
  final int pageSize;
  final ReleaseHistoryView initialView;

  /// Below this panel width the sidebar gives way to the timeline.
  static const narrowWidth = 720.0;

  @override
  State<ReleaseHistoryPanel> createState() => _ReleaseHistoryPanelState();
}

class _ReleaseHistoryPanelState extends State<ReleaseHistoryPanel> {
  ProductUpdates? _api;
  final _notes = <ReleaseNote>[];
  final _versions = <String>{};
  String? _next;
  int? _total;
  bool _started = false, _loading = false, _done = false, _moreFailed = false;
  String? _failure;
  int _selected = 0;
  late ReleaseHistoryView _view = widget.initialView;
  bool _narrow = false;
  final _sidebar = ScrollController();
  final _notesScroll = ScrollController();
  final _all = ScrollController();
  final _focus = FocusNode(debugLabel: 'release-history');

  bool get _timeline => _narrow || _view == ReleaseHistoryView.all;
  ScrollController get _active => _timeline ? _all : _sidebar;

  @override
  void initState() {
    super.initState();
    try {
      _api = widget.loadPage != null
          ? null
          : widget.api ?? ProductUpdates(widget.server);
    } on FormatException {
      _api = null;
    }
    _sidebar.addListener(_nearEnd);
    _all.addListener(_nearEnd);
    unawaited(_load());
  }

  @override
  void dispose() {
    // A connection lent by the update dialog stays that dialog's to close.
    if (widget.api == null) _api?.close();
    _sidebar.dispose();
    _notesScroll.dispose();
    _all.dispose();
    _focus.dispose();
    super.dispose();
  }

  // --- loading ----------------------------------------------------------------

  Future<void> _load() async {
    final api = _api;
    final custom = widget.loadPage;
    if (api == null && custom == null) {
      setState(() => _failure = l10n.noServerAddressSetSo);
      return;
    }
    if (_loading || _done) return;
    setState(() {
      _loading = true;
      _moreFailed = false;
      if (!_started) _failure = null;
    });
    try {
      final before = _started ? _next : null;
      final page = custom != null
          ? await custom(before: before, limit: widget.pageSize)
          : await api!.historyPage(
              widget.product,
              before: before,
              limit: widget.pageSize,
            );
      if (!mounted) return;
      setState(() {
        for (final note in page.releases) {
          if (_versions.add(note.version)) _notes.add(note);
        }
        _next = page.next;
        _done = page.next == null;
        _total = page.total ?? _total;
        _started = true;
      });
      _fillViewport();
    } catch (_) {
      if (!mounted) return;
      setState(() {
        if (_started) {
          _moreFailed = true;
        } else {
          _failure = l10n.couldNotLoadServersChange;
        }
      });
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  /// Infinite scroll: the next page once the end comes near.
  void _nearEnd() {
    final controller = _active;
    if (!controller.hasClients || _done || _loading || _moreFailed) return;
    if (controller.position.extentAfter < 640) unawaited(_load());
  }

  /// A first page shorter than the window cannot be scrolled to its end.
  void _fillViewport() => WidgetsBinding.instance.addPostFrameCallback((_) {
    if (!mounted || _done) return;
    final controller = _active;
    if (controller.hasClients && controller.position.extentAfter < 640) {
      unawaited(_load());
    }
  });

  // --- selection and keys -----------------------------------------------------

  // Grows a little faster than the text (2026-11-03: two lines at 200 %
  // overflowed a 86 px row by 2 px).
  double _rowExtent(BuildContext context) {
    final scale = MediaQuery.textScalerOf(context).scale(1);
    return (30 + 28 * scale + 4 * (scale - 1)).clamp(58.0, 124.0);
  }

  void _select(int index) {
    if (_notes.isEmpty) return;
    final target = index.clamp(0, _notes.length - 1);
    if (target != _selected) {
      setState(() => _selected = target);
      if (_notesScroll.hasClients) _notesScroll.jumpTo(0);
    }
    // Keep the selected row in view.
    if (_sidebar.hasClients) {
      final extent = _rowExtent(context), position = _sidebar.position;
      final top = 10 + target * extent, bottom = top + extent;
      if (top < position.pixels) {
        _sidebar.jumpTo(math.max(0, top - 10));
      } else if (bottom > position.pixels + position.viewportDimension) {
        _sidebar.jumpTo(
          math.min(
            position.maxScrollExtent,
            bottom + 10 - position.viewportDimension,
          ),
        );
      }
    }
    if (target >= _notes.length - 3) unawaited(_load());
  }

  void _scrollAll(double delta) {
    if (!_all.hasClients) return;
    final position = _all.position;
    _all.jumpTo((position.pixels + delta).clamp(0.0, position.maxScrollExtent));
  }

  KeyEventResult _onKey(FocusNode _, KeyEvent event) {
    if (event is! KeyDownEvent && event is! KeyRepeatEvent) {
      return KeyEventResult.ignored;
    }
    final key = event.logicalKey;
    if (_notes.isEmpty) return KeyEventResult.ignored;
    if (_timeline) {
      // Enter and Space stay with a focused button (the view switch, close).
      final page = _all.hasClients
          ? _all.position.viewportDimension * .85
          : 0.0;
      final double? delta = switch (key) {
        LogicalKeyboardKey.arrowDown => 72.0,
        LogicalKeyboardKey.arrowUp => -72.0,
        LogicalKeyboardKey.pageDown => page,
        LogicalKeyboardKey.pageUp => -page,
        LogicalKeyboardKey.home => double.negativeInfinity,
        LogicalKeyboardKey.end => double.infinity,
        _ => null,
      };
      if (delta == null) return KeyEventResult.ignored;
      if (delta.isInfinite && _all.hasClients) {
        _all.jumpTo(delta < 0 ? 0 : _all.position.maxScrollExtent);
      } else {
        _scrollAll(delta);
      }
      return KeyEventResult.handled;
    }
    final target = switch (key) {
      LogicalKeyboardKey.arrowDown || LogicalKeyboardKey.keyJ => _selected + 1,
      LogicalKeyboardKey.arrowUp || LogicalKeyboardKey.keyK => _selected - 1,
      LogicalKeyboardKey.pageDown => _selected + 10,
      LogicalKeyboardKey.pageUp => _selected - 10,
      LogicalKeyboardKey.home => 0,
      LogicalKeyboardKey.end => _notes.length - 1,
      _ => null,
    };
    if (target == null) return KeyEventResult.ignored;
    _select(target);
    return KeyEventResult.handled;
  }

  // --- pieces -----------------------------------------------------------------

  int? _relation(String version) {
    try {
      return (widget.compare ?? compareProductVersions)(
        version,
        widget.currentVersion,
      );
    } on UpdateFailure {
      return null;
    }
  }

  Widget? _badge(String version) {
    final relation = _relation(version);
    if (relation == 0) {
      return VersionBadge(
        l10n.current3,
        key: Key('release-current'),
        tone: AppTone.success,
      );
    }
    if (relation != null && relation > 0) {
      return VersionBadge(
        l10n.newVersion4,
        key: Key('release-new-$version'),
        tone: AppTone.accent,
      );
    }
    return null;
  }

  TextStyle _digits(double size, Color color, {FontWeight? weight}) =>
      uiText(
        size: size,
        weight: weight ?? FontWeight.w600,
        color: color,
      ).copyWith(
        fontFeatures: const [FontFeature.tabularFigures()],
        // Small text is never tightened.
        letterSpacing: size >= 16 ? -.2 : 0,
      );

  String _date(ReleaseNote note) =>
      note.releasedAt == null ? l10n.noReleaseDate : releaseDate(note.releasedAt!);

  Widget _notesBody(AppPalette p, ReleaseNote note, {Key? key}) =>
      note.changelog.trim().isEmpty
      ? Row(
          key: key,
          children: [
            Icon(Icons.notes_rounded, size: 18, color: p.faint),
            const SizedBox(width: 8),
            Expanded(
              child: KeepAllText(
                l10n.noChangesListedVersion,
                style: uiText(size: 13, color: p.muted),
              ),
            ),
          ],
        )
      : SelectionArea(
          child: ChangelogText(note.changelog, key: key, keepWords: true),
        );

  Widget _header(AppPalette p) {
    final current = widget.currentVersion.isEmpty
        ? l10n.currentVersionUnknown
        : l10n.current5(widget.currentVersion);
    final total = _total ?? (_done ? _notes.length : null);
    final title = Row(
      children: [
        const AppToneIcon(icon: Icons.history_rounded, size: 40),
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              KeepAllText(
                l10n.changeLog,
                style: uiText(
                  size: 17,
                  weight: FontWeight.w600,
                  color: p.ink,
                  height: 1.3,
                ).copyWith(letterSpacing: -.2),
              ),
              const SizedBox(height: 2),
              KeepAllText(
                [
                  widget.productLabel ?? productName(widget.product),
                  current,
                  if (total != null) l10n.text28(total),
                ].join(' · '),
                key: const Key('release-history-subtitle'),
                style: uiText(size: 13, color: p.muted),
              ),
            ],
          ),
        ),
      ],
    );
    final close = IconButton(
      key: const Key('close-release-history'),
      tooltip: l10n.close,
      onPressed: () => Navigator.pop(context),
      style: IconButton.styleFrom(
        foregroundColor: p.muted,
        minimumSize: const Size.square(34),
        maximumSize: const Size.square(34),
        padding: EdgeInsets.zero,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
      ),
      icon: const Icon(Icons.close_rounded, size: 20),
    );
    final toggle = SegmentedButton<ReleaseHistoryView>(
      key: const Key('release-history-view'),
      showSelectedIcon: false,
      segments: [
        ButtonSegment(
          value: ReleaseHistoryView.versions,
          icon: Icon(Icons.view_sidebar_outlined, size: 17),
          label: KeepAllText(l10n.byVersion, key: Key('release-history-view-versions')),
          tooltip: l10n.seeVersionListEachVersions,
        ),
        ButtonSegment(
          value: ReleaseHistoryView.all,
          icon: Icon(Icons.view_agenda_outlined, size: 17),
          label: KeepAllText(l10n.releaseHistoryShowAll, key: Key('release-history-view-all')),
          tooltip: l10n.seeChangesAllVersionsOne,
        ),
      ],
      selected: {_view},
      onSelectionChanged: (selection) {
        setState(() => _view = selection.first);
        _focus.requestFocus();
        _fillViewport();
      },
    );
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 16, 14, 14),
      child: Row(
        children: [
          Expanded(child: title),
          if (!_narrow && _notes.isNotEmpty) ...[
            const SizedBox(width: 12),
            toggle,
          ],
          const SizedBox(width: 8),
          close,
        ],
      ),
    );
  }

  Widget _state(AppPalette p) {
    if (_failure != null) {
      return Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 420),
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.cloud_off_rounded, size: 34, color: p.red),
                const SizedBox(height: 12),
                KeepAllText(
                  _failure!,
                  key: const Key('release-history-error'),
                  textAlign: TextAlign.center,
                  style: uiText(size: 13.5, height: 1.6, color: p.ink),
                ),
                if (_api != null || widget.loadPage != null) ...[
                  const SizedBox(height: 16),
                  OutlinedButton.icon(
                    key: const Key('release-history-retry'),
                    onPressed: _loading ? null : () => unawaited(_load()),
                    style: AppButtons.outlined(p),
                    icon: const Icon(Icons.refresh_rounded, size: 18),
                    label: KeepAllText(l10n.tryAgain),
                  ),
                ],
              ],
            ),
          ),
        ),
      );
    }
    if (!_started) {
      return Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 360),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              const LinearProgressIndicator(),
              const SizedBox(height: 12),
              KeepAllText(
                l10n.loadingChangeList,
                textAlign: TextAlign.center,
                style: uiText(size: 13.5, color: p.muted),
              ),
            ],
          ),
        ),
      );
    }
    return Center(
      child: Column(
        key: const Key('release-history-empty'),
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.inbox_outlined, size: 34, color: p.faint),
          const SizedBox(height: 10),
          KeepAllText(
            l10n.serverHasNoChangeList,
            style: uiText(size: 13.5, color: p.muted),
          ),
        ],
      ),
    );
  }

  /// The end of a list: loading more, a failed page with its retry, or the
  /// oldest version reached.
  Widget _more(AppPalette p, {required bool compact}) {
    if (_moreFailed) {
      return Center(
        child: TextButton.icon(
          key: const Key('release-history-more-retry'),
          onPressed: () => unawaited(_load()),
          icon: const Icon(Icons.refresh_rounded, size: 16),
          label: KeepAllText(compact ? l10n.reload : l10n.couldNotLoadEarlierVersions),
        ),
      );
    }
    if (!_done) {
      // The spinner turns only while a page is on its way.
      return Center(
        child: SizedBox.square(
          key: const Key('release-history-loading-more'),
          dimension: 18,
          child: _loading
              ? const CircularProgressIndicator(strokeWidth: 2)
              : null,
        ),
      );
    }
    if (compact) return const SizedBox.shrink();
    return Center(
      child: Row(
        key: const Key('release-history-end'),
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.flag_outlined, size: 16, color: p.faint),
          const SizedBox(width: 6),
          KeepAllText(
            l10n.youveReachedFirstVersion,
            style: uiText(size: 12.5, color: p.muted),
          ),
        ],
      ),
    );
  }

  // --- 버전별 -------------------------------------------------------------------

  Widget _row(AppPalette p, int index) {
    final note = _notes[index], selected = index == _selected;
    final badge = _badge(note.version);
    final missing = note.changelog.trim().isEmpty;
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: Semantics(
        selected: selected,
        child: Material(
          color: Colors.transparent,
          child: InkWell(
            key: Key('release-version-${note.version}'),
            onTap: () {
              _select(index);
              _focus.requestFocus();
            },
            borderRadius: BorderRadius.circular(10),
            hoverColor: p.hover,
            // The chosen version: its fill and the colour of its number.
            // (It also had a bar and a frame: five signals for one choice.)
            child: Ink(
              decoration: BoxDecoration(
                color: selected ? p.amberSoft : Colors.transparent,
                borderRadius: BorderRadius.circular(10),
              ),
              child: Row(
                children: [
                  const SizedBox(width: 12, height: 26),
                  Expanded(
                    child: Column(
                      mainAxisAlignment: MainAxisAlignment.center,
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          'v${note.version}',
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: _digits(14, selected ? p.amberInk : p.ink),
                        ),
                        const SizedBox(height: 2),
                        KeepAllText(
                          missing ? l10n.noChangesListed(_date(note)) : _date(note),
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: uiText(size: 12, color: p.muted),
                        ),
                      ],
                    ),
                  ),
                  if (badge != null) ...[badge, const SizedBox(width: 8)],
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _versionsView(AppPalette p) {
    final extent = _rowExtent(context);
    final note = _notes[_selected.clamp(0, _notes.length - 1)];
    final badge = _badge(note.version);
    final footer = !_done || _moreFailed;
    // Wider with larger text so "v0.4.174" and its badge stay readable, but
    // never more than ~40 % of the panel.
    final scale = MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.5);
    final sidebar = Container(
      width: math.min(252 * scale, MediaQuery.sizeOf(context).width * .4),
      color: p.tileWell,
      child: Scrollbar(
        controller: _sidebar,
        child: ListView.builder(
          key: const Key('release-history-sidebar'),
          controller: _sidebar,
          padding: const EdgeInsets.fromLTRB(10, 10, 10, 10),
          itemExtent: extent,
          itemCount: _notes.length + (footer ? 1 : 0),
          itemBuilder: (context, index) =>
              index < _notes.length ? _row(p, index) : _more(p, compact: true),
        ),
      ),
    );
    Widget step(IconData icon, String tooltip, Key key, int? target) =>
        IconButton(
          key: key,
          tooltip: tooltip,
          onPressed: target == null
              ? null
              : () {
                  _select(target);
                  _focus.requestFocus();
                },
          style: IconButton.styleFrom(
            foregroundColor: p.ink,
            disabledForegroundColor: p.faint.withValues(alpha: .5),
            backgroundColor: p.quiet,
            minimumSize: const Size.square(34),
            maximumSize: const Size.square(34),
            padding: EdgeInsets.zero,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(10),
            ),
          ),
          icon: Icon(icon, size: 20),
        );
    final last = _selected >= _notes.length - 1;
    final detail = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(26, 18, 18, 14),
          child: Row(
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Wrap(
                      spacing: 10,
                      runSpacing: 6,
                      crossAxisAlignment: WrapCrossAlignment.center,
                      children: [
                        Text(
                          'v${note.version}',
                          key: const Key('release-detail-version'),
                          style: _digits(22, p.ink),
                        ),
                        ?badge,
                      ],
                    ),
                    const SizedBox(height: 4),
                    KeepAllText(
                      note.releasedAt == null
                          ? l10n.noReleaseDate
                          : l10n.released(releaseDate(note.releasedAt!)),
                      style: uiText(size: 12.5, color: p.muted),
                    ),
                  ],
                ),
              ),
              Text(
                '${_selected + 1} / ${_total ?? _notes.length}',
                key: const Key('release-detail-position'),
                style: _digits(12.5, p.muted, weight: FontWeight.w600),
              ),
              const SizedBox(width: 12),
              step(
                Icons.keyboard_arrow_up_rounded,
                l10n.newerVersion,
                const Key('release-newer'),
                _selected > 0 ? _selected - 1 : null,
              ),
              const SizedBox(width: 6),
              step(
                Icons.keyboard_arrow_down_rounded,
                l10n.olderVersion,
                const Key('release-older'),
                last ? null : _selected + 1,
              ),
            ],
          ),
        ),
        Divider(height: 1, color: p.tileLine),
        Expanded(
          child: Scrollbar(
            controller: _notesScroll,
            thumbVisibility: true,
            child: SingleChildScrollView(
              key: const Key('release-notes-scroll'),
              controller: _notesScroll,
              padding: const EdgeInsets.fromLTRB(26, 18, 26, 26),
              child: ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 680),
                child: _notesBody(
                  p,
                  note,
                  key: Key('release-notes-${note.version}'),
                ),
              ),
            ),
          ),
        ),
      ],
    );
    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        sidebar,
        VerticalDivider(width: 1, thickness: 1, color: p.tileLine),
        Expanded(child: detail),
      ],
    );
  }

  // --- 전체 보기 ------------------------------------------------------------------

  Widget _stickyHeader(AppPalette p, ReleaseNote note, double side) {
    final badge = _badge(note.version);
    return Container(
      key: Key('release-all-header-${note.version}'),
      padding: EdgeInsets.fromLTRB(side, 12, side, 10),
      decoration: BoxDecoration(
        color: p.tile,
        border: Border(bottom: BorderSide(color: p.tileLine)),
      ),
      child: Row(
        children: [
          Container(
            width: 11,
            height: 11,
            decoration: BoxDecoration(
              color: badge == null ? p.tile : p.amber,
              shape: BoxShape.circle,
              border: Border.all(color: p.amber, width: 2),
            ),
          ),
          const SizedBox(width: 12),
          // Version and badge on the left, the date on the right; a narrow
          // window or large text moves the date under them instead of
          // cutting either.
          Expanded(
            child: Wrap(
              alignment: WrapAlignment.spaceBetween,
              crossAxisAlignment: WrapCrossAlignment.center,
              spacing: 8,
              runSpacing: 2,
              children: [
                Wrap(
                  crossAxisAlignment: WrapCrossAlignment.center,
                  spacing: 8,
                  runSpacing: 4,
                  children: [
                    Text('v${note.version}', style: _digits(15.5, p.ink)),
                    ?badge,
                  ],
                ),
                KeepAllText(
                  _date(note),
                  style: uiText(size: 12.5, color: p.muted),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _allView(AppPalette p) {
    final side = _narrow ? 18.0 : 26.0;
    return Scrollbar(
      controller: _all,
      thumbVisibility: true,
      child: CustomScrollView(
        key: const Key('release-history-all'),
        controller: _all,
        slivers: [
          for (final note in _notes)
            SliverMainAxisGroup(
              slivers: [
                PinnedHeaderSliver(child: _stickyHeader(p, note, side)),
                SliverToBoxAdapter(
                  child: Padding(
                    padding: EdgeInsets.fromLTRB(side + 4, 0, side, 0),
                    child: Container(
                      padding: const EdgeInsets.fromLTRB(22, 12, 0, 26),
                      decoration: BoxDecoration(
                        border: Border(
                          left: BorderSide(color: p.tileLine, width: 2),
                        ),
                      ),
                      child: Align(
                        alignment: Alignment.topLeft,
                        child: ConstrainedBox(
                          constraints: const BoxConstraints(maxWidth: 720),
                          child: _notesBody(
                            p,
                            note,
                            key: Key('release-all-notes-${note.version}'),
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          SliverToBoxAdapter(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 16, 16, 28),
              child: _more(p, compact: false),
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final screen = MediaQuery.sizeOf(context);
    final inset = screen.width < 520 ? 12.0 : 24.0;
    final width = math.min(1080.0, screen.width - 2 * inset);
    final height = math.min(780.0, screen.height - 48);
    _narrow = width < ReleaseHistoryPanel.narrowWidth;
    final Widget body;
    if (_failure != null || !_started || _notes.isEmpty) {
      body = _state(p);
    } else if (_timeline) {
      body = _allView(p);
    } else {
      body = _versionsView(p);
    }
    return Dialog(
      key: const Key('release-history-dialog'),
      backgroundColor: p.tile,
      surfaceTintColor: Colors.transparent,
      insetPadding: EdgeInsets.symmetric(horizontal: inset, vertical: 24),
      clipBehavior: Clip.antiAlias,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(18),
        side: BorderSide(color: p.tileLine),
      ),
      child: SizedBox(
        width: width,
        height: math.max(240.0, height),
        child: Focus(
          focusNode: _focus,
          autofocus: true,
          onKeyEvent: _onKey,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              _header(p),
              Divider(height: 1, color: p.tileLine),
              Expanded(child: body),
            ],
          ),
        ),
      ),
    );
  }
}
