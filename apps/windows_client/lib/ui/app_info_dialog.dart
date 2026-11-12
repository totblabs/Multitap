import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/app_licenses.dart';
import '../data/broker_api.dart' show brokerServerVersion;
import '../data/client_build_version.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'components.dart' show ProductMark;
import 'korean_text.dart';
import '../l10n/l10n.dart';

enum AppInfoTab { information, server, licenses }

Future<void> showAppInfoDialog(
  BuildContext context, {
  String? title,
  String productName = 'Multitap',
  String? platform,
  String? versionLabel,
  String? serverOrigin,
  String? account,
  bool isDemo = false,
  String manifestAsset = nativeLicenseManifestAsset,
  Future<ClientBuildVersion> Function()? loadVersion,
  VoidCallback? onReleaseHistory,
  VoidCallback? onCheckUpdates,
  String? serverVersion,
}) => showDialog<void>(
  context: context,
  builder: (_) => AppInfoDialog(
    title: title,
    productName: productName,
    platform: platform,
    versionLabel: versionLabel,
    serverOrigin: serverOrigin,
    account: account,
    isDemo: isDemo,
    manifestAsset: manifestAsset,
    loadVersion: loadVersion,
    onReleaseHistory: onReleaseHistory,
    onCheckUpdates: onCheckUpdates,
    serverVersion: serverVersion,
  ),
);

class AppInfoDialog extends StatefulWidget {
  const AppInfoDialog({
    super.key,
    String? title,
    this.productName = 'Multitap',
    String? platform,
    String? versionLabel,
    this.serverOrigin,
    this.account,
    this.isDemo = false,
    this.manifestAsset = nativeLicenseManifestAsset,
    this.loadVersion,
    this.loadLicenses,
    this.onReleaseHistory,
    this.onCheckUpdates,
    this.serverVersion,
    this.initialTab = AppInfoTab.information,
  }) : _title = title, // ignore: prefer_initializing_formals
       _platform = platform, // ignore: prefer_initializing_formals
       _versionLabel = versionLabel; // ignore: prefer_initializing_formals

  final String productName, manifestAsset;
  final String? _title, _platform, _versionLabel;
  String get title => _title ?? l10n.clientInfo;
  String get platform => _platform ?? l10n.windowsClient;
  String get versionLabel => _versionLabel ?? l10n.version;
  final String? serverOrigin, account;

  /// The server's version when the caller knows it; else the last desktop
  /// list's ([brokerServerVersion]).
  final String? serverVersion;
  final bool isDemo;
  final Future<ClientBuildVersion> Function()? loadVersion;
  final Future<AppLicenseCatalog> Function()? loadLicenses;
  final VoidCallback? onReleaseHistory;

  /// Opens the app's update panel (Android: the update dialog).
  final VoidCallback? onCheckUpdates;
  final AppInfoTab initialTab;

  @override
  State<AppInfoDialog> createState() => _AppInfoDialogState();
}

class _AppInfoDialogState extends State<AppInfoDialog> {
  late AppInfoTab _tab = widget.initialTab;
  ClientBuildVersion _version = const ClientBuildVersion.unknown();
  final _search = TextEditingController();
  final _searchFocus = FocusNode();
  final _textScroll = ScrollController();
  AppLicenseCatalog? _catalog;
  String? _selected;
  bool _loading = false;
  bool _initialComponentChosen = false;
  int _loadGeneration = 0;
  Timer? _versionDeadline;
  Completer<ClientBuildVersion>? _versionExpiry;

  @override
  void initState() {
    super.initState();
    _search.addListener(_searchChanged);
    unawaited(_readVersion());
    if (_tab == AppInfoTab.licenses) unawaited(_readLicenses());
  }

  Future<void> _readVersion() async {
    final expiry = Completer<ClientBuildVersion>();
    _versionExpiry = expiry;
    _versionDeadline = Timer(const Duration(seconds: 3), () {
      expiry.complete(const ClientBuildVersion.unknown());
    });
    ClientBuildVersion version;
    try {
      // The native read cannot be cancelled. Race it with an owned observation
      // deadline so closing or expiry also releases this State's async wait.
      version = await Future.any([
        widget.loadVersion?.call() ?? ClientBuildVersion.load(),
        expiry.future,
      ]);
    } catch (_) {
      version = const ClientBuildVersion.unknown();
    }
    if (!mounted || _versionExpiry != expiry) return;
    _versionExpiry = null;
    _versionDeadline?.cancel();
    _versionDeadline = null;
    if (!expiry.isCompleted) {
      expiry.complete(const ClientBuildVersion.unknown());
    }
    setState(() => _version = version);
  }

  Future<void> _readLicenses() async {
    if (_loading) return;
    final generation = ++_loadGeneration;
    setState(() => _loading = true);
    AppLicenseCatalog catalog;
    try {
      catalog =
          await (widget.loadLicenses?.call() ??
              AppLicenseCatalog.load(manifestAsset: widget.manifestAsset));
    } catch (_) {
      catalog = AppLicenseCatalog(
        components: const [],
        issues: const [AppLicenseIssue.registeredUnavailable],
      );
    }
    if (!mounted || generation != _loadGeneration) return;
    setState(() {
      _catalog = catalog;
      _loading = false;
      _selectAvailable();
    });
  }

  List<AppLicenseComponent> get _filtered {
    final query = _search.text.trim().toLowerCase();
    return _catalog?.components
            .where((entry) => entry.matches(query))
            .toList() ??
        const [];
  }

  void _selectAvailable() {
    final filtered = _filtered;
    if (!filtered.any((entry) => entry.name == _selected)) {
      final project = filtered.where(
        (entry) => entry.name.toLowerCase() == widget.productName.toLowerCase(),
      );
      _selected = filtered.isEmpty
          ? null
          : (!_initialComponentChosen && project.isNotEmpty
                ? project.first.name
                : filtered.first.name);
    }
    if (filtered.isNotEmpty) _initialComponentChosen = true;
  }

  void _searchChanged() {
    setState(_selectAvailable);
    if (_textScroll.hasClients) _textScroll.jumpTo(0);
  }

  void _select(String? name) {
    setState(() => _selected = name);
    if (_textScroll.hasClients) _textScroll.jumpTo(0);
  }

  void _changeTab(AppInfoTab tab) {
    setState(() => _tab = tab);
    if (tab == AppInfoTab.licenses && _catalog == null) {
      unawaited(_readLicenses());
    }
  }

  void _find() {
    _changeTab(AppInfoTab.licenses);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _searchFocus.requestFocus();
    });
  }

  @override
  void dispose() {
    _loadGeneration++;
    _versionDeadline?.cancel();
    final expiry = _versionExpiry;
    _versionExpiry = null;
    if (expiry != null && !expiry.isCompleted) {
      expiry.complete(const ClientBuildVersion.unknown());
    }
    _search.dispose();
    _searchFocus.dispose();
    _textScroll.dispose();
    super.dispose();
  }

  /// The sections, in order. 서버 only when the app is signed in to one.
  List<(AppInfoTab, IconData, String)> get _sections => [
    (AppInfoTab.information, Icons.info_outline_rounded, widget.productName),
    if (widget.serverOrigin != null || widget.isDemo)
      (AppInfoTab.server, Icons.dns_outlined, l10n.server),
    (AppInfoTab.licenses, Icons.description_outlined, l10n.openSourceLicenses),
  ];

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    void close() => Navigator.of(context).pop();
    final scale = MediaQuery.textScalerOf(context).scale(1);
    // The information and the server are as large as what they say (two or
    // three rows in a 640 px dialog). Only the license browser, a list beside
    // a long text, takes the wide dialog and the window's height (a
    // 960 × 686 dialog would hold two rows).
    final browser = _tab == AppInfoTab.licenses;
    final browserHeight = (MediaQuery.sizeOf(context).height - 94 - 112 * scale)
        .clamp(160.0, 560.0)
        .toDouble();
    return CallbackShortcuts(
      bindings: {
        const SingleActivator(LogicalKeyboardKey.escape): close,
        const SingleActivator(LogicalKeyboardKey.keyF, control: true): _find,
      },
      // Esc and Ctrl+F need focus inside the dialog; it rests here, on no
      // control, until Tab moves it (the header X is the one way to close).
      child: Focus(
        autofocus: true,
        child: AppDialog(
          key: const Key('client-info-dialog'),
          title: widget.title,
          icon: Icons.info_outline_rounded,
          onClose: close,
          closeKey: const Key('close-settings'),
          maxWidth: browser ? 960 : 640,
          // The outer body may scroll in very short windows; the original text
          // has its own viewport while the header stays reachable.
          scrollBody: true,
          body: [
            LayoutBuilder(
              builder: (context, constraints) {
                // The side list from the 640 px dialog's own width (596 px
                // of content) up; narrower, the sections are tabs on top.
                final rail = constraints.maxWidth / scale >= 590;
                final pane = switch (_tab) {
                  AppInfoTab.information => _about(context),
                  AppInfoTab.server => _server(context),
                  AppInfoTab.licenses => SizedBox(
                    height: browserHeight,
                    child: _licenses(context),
                  ),
                };
                if (!rail) {
                  return Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      SegmentedButton<AppInfoTab>(
                        key: const Key('app-info-tabs'),
                        showSelectedIcon: false,
                        style: const ButtonStyle(
                          padding: WidgetStatePropertyAll(
                            EdgeInsets.symmetric(horizontal: 2),
                          ),
                        ),
                        segments: [
                          // Words only: a narrow window has no room for an
                          // icon and the whole label in each third.
                          for (final (tab, _, label) in _sections)
                            ButtonSegment(
                              value: tab,
                              label: Text(
                                tab == AppInfoTab.licenses
                                    ? l10n.licenses
                                    : tab == AppInfoTab.information
                                    ? l10n.about
                                    : label,
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                              ),
                            ),
                        ],
                        selected: {_tab},
                        onSelectionChanged: (values) =>
                            _changeTab(values.single),
                      ),
                      const SizedBox(height: 16),
                      pane,
                    ],
                  );
                }
                return Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    SizedBox(
                      key: const Key('app-info-tabs'),
                      width: 188,
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          for (final (tab, icon, label) in _sections)
                            _SectionButton(
                              key: Key('app-info-section-${tab.name}'),
                              icon: icon,
                              label: label,
                              selected: tab == _tab,
                              onTap: () => _changeTab(tab),
                            ),
                        ],
                      ),
                    ),
                    const SizedBox(width: 16),
                    // The line beside the list is the pane's own left edge, so
                    // it is as tall as the pane, and never shorter than the
                    // list.
                    Expanded(
                      child: Container(
                        constraints: BoxConstraints(
                          minHeight:
                              _sections.length *
                              (2 + (16 + 18 * scale).clamp(40.0, 96.0)),
                        ),
                        padding: const EdgeInsets.only(left: 20),
                        decoration: BoxDecoration(
                          border: Border(left: BorderSide(color: p.tileLine)),
                        ),
                        child: pane,
                      ),
                    ),
                  ],
                );
              },
            ),
          ],
        ),
      ),
    );
  }

  /// The product: its mark and name, then what this build is.
  Widget _about(BuildContext context) {
    final p = AppPalette.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            const ProductMark(size: 56, tile: true),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  KeepAllText(
                    widget.productName,
                    style: uiText(
                      size: 22,
                      weight: FontWeight.w700,
                      color: p.ink,
                    ),
                  ),
                  const SizedBox(height: 2),
                  KeepAllText(
                    'for Proxmox VE · ${widget.platform}',
                    style: uiText(size: 13, color: p.muted),
                  ),
                ],
              ),
            ),
          ],
        ),
        const SizedBox(height: 24),
        _InfoFacts(
          key: const Key('app-info-facts'),
          rows: [
            (widget.versionLabel, _version.label),
            (l10n.build, _version.build ?? l10n.needsCheck),
          ],
        ),
        if (widget.onCheckUpdates != null ||
            widget.onReleaseHistory != null) ...[
          const SizedBox(height: 16),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              // Two actions of one rank: one look (outlined).
              if (widget.onCheckUpdates case final check?)
                OutlinedButton.icon(
                  key: const Key('app-info-check-updates'),
                  onPressed: check,
                  style: AppButtons.outlined(p),
                  icon: const Icon(Icons.arrow_circle_up_outlined, size: 18),
                  label: KeepAllText(l10n.checkUpdates),
                ),
              if (widget.onReleaseHistory case final history?)
                OutlinedButton.icon(
                  key: const Key('client-release-history'),
                  onPressed: history,
                  style: AppButtons.outlined(p),
                  icon: const Icon(Icons.history_rounded, size: 18),
                  label: KeepAllText(l10n.changeLog),
                ),
            ],
          ),
        ],
      ],
    );
  }

  /// The server this app is signed in to.
  Widget _server(BuildContext context) {
    final p = AppPalette.of(context);
    final version = widget.serverVersion ?? brokerServerVersion.value;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        KeepAllText(
          widget.isDemo ? l10n.demoMode : l10n.multitapServer,
          style: uiText(size: 18, weight: FontWeight.w700, color: p.ink),
        ),
        const SizedBox(height: 4),
        KeepAllText(
          widget.isDemo
              ? l10n.screenIsntConnectedRealServer
              : l10n.serverManagesDesktopListConnections,
          style: uiText(size: 13, color: p.muted),
        ),
        const SizedBox(height: 20),
        _InfoFacts(
          key: const Key('app-info-server-facts'),
          rows: [
            if (widget.serverOrigin case final origin?) (l10n.address, origin),
            (l10n.serverVersion, version ?? l10n.unavailable),
            if (widget.account case final account?) (l10n.account2, account),
          ],
        ),
      ],
    );
  }

  Widget _licenses(BuildContext context) {
    final p = AppPalette.of(context);
    if (_loading && _catalog == null) {
      return const Center(child: CircularProgressIndicator());
    }
    final filtered = _filtered;
    final current = filtered.isEmpty
        ? null
        : filtered.firstWhere(
            (entry) => entry.name == _selected,
            orElse: () => filtered.first,
          );
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        TextField(
          key: const Key('license-search'),
          controller: _search,
          focusNode: _searchFocus,
          decoration: InputDecoration(
            hintText: _catalog == null || _catalog!.components.isEmpty
                ? l10n.searchComponentsLicenseText
                : l10n.componentsSearchNameLicenseText(_catalog!.components.length),
            prefixIcon: const Icon(Icons.search_rounded),
            suffixIcon: _search.text.isEmpty
                ? null
                : IconButton(
                    tooltip: l10n.clearSearch,
                    onPressed: _search.clear,
                    icon: const Icon(Icons.close_rounded),
                  ),
          ),
        ),
        if (_catalog?.issues.isNotEmpty == true) ...[
          const SizedBox(height: 8),
          Row(
            children: [
              Icon(Icons.warning_amber_rounded, color: p.warn, size: 18),
              const SizedBox(width: 8),
              Expanded(
                child: KeepAllText(
                  l10n.someLicensesCouldntLoaded,
                  key: const Key('license-load-warning'),
                  style: uiText(size: 12, color: p.muted),
                ),
              ),
              TextButton(
                onPressed: _loading ? null : _readLicenses,
                child: KeepAllText(_loading ? l10n.loading : l10n.reload),
              ),
            ],
          ),
        ],
        const SizedBox(height: 10),
        Expanded(
          child: filtered.isEmpty
              ? Center(
                  child: KeepAllText(
                    _search.text.isEmpty ? l10n.noLicensesShow : l10n.noMatchingLicenses,
                    key: const Key('license-empty'),
                    style: uiText(color: p.muted),
                  ),
                )
              : LayoutBuilder(
                  builder: (context, constraints) {
                    final wide =
                        constraints.maxWidth /
                            MediaQuery.textScalerOf(context).scale(1) >=
                        640;
                    final details = _noticeBody(
                      context,
                      current!,
                      compact: !wide,
                    );
                    if (!wide) {
                      return Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          Material(
                            color: p.tileWell,
                            borderRadius: BorderRadius.circular(12),
                            child: Padding(
                              padding: const EdgeInsets.symmetric(
                                horizontal: 12,
                              ),
                              child: DropdownButtonHideUnderline(
                                child: DropdownButton<String>(
                                  key: const Key('license-component-select'),
                                  value: _selected,
                                  isExpanded: true,
                                  items: [
                                    for (final entry in filtered)
                                      DropdownMenuItem(
                                        value: entry.name,
                                        child: Text(
                                          entry.name,
                                          maxLines: 1,
                                          overflow: TextOverflow.ellipsis,
                                        ),
                                      ),
                                  ],
                                  onChanged: _select,
                                ),
                              ),
                            ),
                          ),
                          const SizedBox(height: 10),
                          Expanded(child: details),
                        ],
                      );
                    }
                    return Row(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        SizedBox(
                          width: 256,
                          child: ListView.builder(
                            key: const Key('license-component-list'),
                            itemCount: filtered.length,
                            itemBuilder: (context, index) {
                              final entry = filtered[index];
                              final selected = entry.name == _selected;
                              return Padding(
                                padding: const EdgeInsets.only(bottom: 2),
                                child: Material(
                                  color: selected
                                      ? p.amberSoft
                                      : Colors.transparent,
                                  borderRadius: BorderRadius.circular(8),
                                  clipBehavior: Clip.antiAlias,
                                  child: ListTile(
                                    dense: true,
                                    visualDensity: VisualDensity.compact,
                                    contentPadding: const EdgeInsets.symmetric(
                                      horizontal: 12,
                                    ),
                                    selected: selected,
                                    selectedColor: p.amberInk,
                                    title: Text(
                                      entry.name,
                                      maxLines: 2,
                                      overflow: TextOverflow.ellipsis,
                                      style: uiText(
                                        size: 13,
                                        weight: FontWeight.w600,
                                      ),
                                    ),
                                    trailing: entry.notices.length > 1
                                        ? Text(
                                            l10n.text15(entry.notices.length),
                                            style: uiText(
                                              size: 12,
                                              color: p.muted,
                                            ),
                                          )
                                        : null,
                                    onTap: () => _select(entry.name),
                                  ),
                                ),
                              );
                            },
                          ),
                        ),
                        const SizedBox(width: 16),
                        Expanded(child: details),
                      ],
                    );
                  },
                ),
        ),
      ],
    );
  }

  Widget _noticeBody(
    BuildContext context,
    AppLicenseComponent component, {
    bool compact = false,
  }) {
    final p = AppPalette.of(context);
    return Material(
      key: const Key('license-document'),
      color: p.tileWell,
      borderRadius: BorderRadius.circular(12),
      clipBehavior: Clip.antiAlias,
      child: Scrollbar(
        controller: _textScroll,
        child: SingleChildScrollView(
          controller: _textScroll,
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              if (!compact)
                SelectableText(
                  component.name,
                  style: uiText(
                    size: 16,
                    weight: FontWeight.w600,
                    color: p.ink,
                  ),
                ),
              for (final (index, notice) in component.notices.indexed) ...[
                if (index > 0)
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 16),
                    child: Divider(color: p.tileLine),
                  ),
                const SizedBox(height: 8),
                SelectableText(
                  [
                    notice.license,
                    if (notice.version?.isNotEmpty == true)
                      l10n.version2('${notice.version}'),
                    if (notice.source?.isNotEmpty == true) notice.source!,
                  ].join('\n'),
                  style: uiText(size: 12, color: p.muted, height: 1.5),
                ),
                const SizedBox(height: 16),
                SelectableText(
                  notice.text,
                  key: ValueKey('license-text-$index'),
                  style: uiText(size: 13, color: p.ink, height: 1.55),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

/// One section in the information window's side list (40 px, the amber
/// selection of the launcher's menus).
class _SectionButton extends StatelessWidget {
  const _SectionButton({
    super.key,
    required this.icon,
    required this.label,
    required this.selected,
    required this.onTap,
  });
  final IconData icon;
  final String label;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Padding(
      padding: const EdgeInsets.only(bottom: 2),
      child: Material(
        color: selected ? p.amberSoft : Colors.transparent,
        borderRadius: BorderRadius.circular(8),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: onTap,
          child: Semantics(
            selected: selected,
            button: true,
            child: ConstrainedBox(
              constraints: const BoxConstraints(minHeight: 40),
              child: Padding(
                padding: const EdgeInsets.symmetric(
                  horizontal: 10,
                  vertical: 8,
                ),
                child: Row(
                  children: [
                    Icon(
                      icon,
                      size: 18,
                      color: selected ? p.amberInk : p.muted,
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      child: KeepAllText(
                        label,
                        style: uiText(
                          size: 13,
                          weight: selected ? FontWeight.w600 : FontWeight.w500,
                          color: selected ? p.amberInk : p.ink,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// Label / value rows on one surface, separated by hairlines. Values are
/// selectable so a version or address can be copied.
class _InfoFacts extends StatelessWidget {
  const _InfoFacts({super.key, required this.rows});
  final List<(String, String)> rows;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return DecoratedBox(
      decoration: BoxDecoration(
        color: p.tileWell,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: p.tileLine),
      ),
      child: Column(
        children: [
          for (final (index, row) in rows.indexed)
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
              decoration: BoxDecoration(
                border: index == 0
                    ? null
                    : Border(top: BorderSide(color: p.tileLine)),
              ),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(
                    width: 112,
                    child: KeepAllText(
                      row.$1,
                      style: uiText(size: 13, color: p.muted),
                    ),
                  ),
                  Expanded(
                    child: SelectableText(
                      row.$2,
                      style: uiText(
                        size: 13,
                        weight: FontWeight.w600,
                        color: p.ink,
                      ),
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }
}
