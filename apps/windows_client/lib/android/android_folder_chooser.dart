import 'dart:async';

import 'package:flutter/material.dart';

import '../data/drive_files.dart';
import '../ui/app_dialog.dart';
import '../ui/app_progress.dart';
import '../ui/app_theme.dart';
import '../ui/korean_text.dart';
import 'android_drive.dart';
import '../l10n/l10n.dart';

/// Shared folders on Android: this app's own folder list, with all-files
/// access. It is the only way on every device
/// that has that access (Android 11+): Android's own picker is an app that
/// differs from phone to phone, can be turned off and is missing on a TV.
///
/// Where the access is not allowed yet, the person is told what it means and
/// taken to Android's page for it; back in the app with it allowed, the list
/// opens. The chosen folder's path, or null when the person leaves.
Future<String?> showAndroidFolderChooser(
  BuildContext context,
  AndroidDriveFileSystem files, {
  Duration settingsWait = const Duration(minutes: 5),
}) async {
  var access = await files.storageAccess();
  if (!context.mounted) return null;
  if (!access.granted) {
    final agreed = await showAppConfirm(
      context,
      icon: Icons.folder_shared_rounded,
      tone: AppTone.warn,
      title: l10n.allowAccessAllFiles,
      message:
          l10n.chooseFolderMultitapNeedsSee,
      confirmLabel: l10n.openSettings,
      dialogKey: const Key('android-storage-access'),
      confirmKey: const Key('android-storage-access-yes'),
      cancelKey: const Key('android-storage-access-no'),
    );
    if (!agreed) return null;
    // Android's page, then back here: the list opens by itself once the
    // person is in the app again with the access allowed.
    final back = Completer<void>();
    final watch = AppLifecycleListener(
      onResume: () {
        if (!back.isCompleted) back.complete();
      },
    );
    try {
      if (!await files.openStorageSettings()) return null;
      await back.future.timeout(settingsWait, onTimeout: () {});
    } finally {
      watch.dispose();
    }
    access = await files.storageAccess();
    if (!access.granted || !context.mounted) return null;
  }
  return showDialog<String>(
    context: context,
    builder: (_) => AndroidFolderChooser(files: files),
  );
}
/// The folder list itself: the storage volumes first, then one folder at a
/// time. Only folders are shown; the folder being looked at is the one that
/// is shared.
class AndroidFolderChooser extends StatefulWidget {
  const AndroidFolderChooser({super.key, required this.files});
  final AndroidDriveFileSystem files;

  @override
  State<AndroidFolderChooser> createState() => _AndroidFolderChooserState();
}

class _AndroidFolderChooserState extends State<AndroidFolderChooser> {
  AndroidFolderList? _list;
  String? _error;
  bool _loading = true, _single = false;

  /// The read has run long enough to be said in the list's place. A folder
  /// that opens at once keeps the folders on screen: no flash of a line.
  bool _slow = false;
  Timer? _slowTimer;
  int _asked = 0;

  @override
  void initState() {
    super.initState();
    unawaited(_go(null));
  }

  @override
  void dispose() {
    _slowTimer?.cancel();
    super.dispose();
  }

  Future<void> _go(String? path) async {
    final asked = ++_asked;
    _slowTimer?.cancel();
    _slowTimer = Timer(const Duration(milliseconds: 300), () {
      if (mounted && _loading && asked == _asked) setState(() => _slow = true);
    });
    setState(() {
      _loading = true;
      _slow = false;
      _error = null;
    });
    try {
      var list = await widget.files.storageFolders(path);
      // One volume only (the usual phone): straight into it, and nothing
      // above its top.
      if (path == null) {
        _single = list.folders.length == 1;
        if (_single) {
          list = await widget.files.storageFolders(list.folders.single.path);
        }
      }
      if (!mounted || asked != _asked) return;
      _slowTimer?.cancel();
      setState(() {
        _list = list;
        _loading = false;
      });
    } on DriveFileException catch (failure) {
      if (!mounted || asked != _asked) return;
      _slowTimer?.cancel();
      setState(() {
        _loading = false;
        _error = failure.code == 'drive_denied'
            ? l10n.cantOpenFolderChooseAnother
            : androidDriveMessage(failure.code);
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final list = _list;
    final path = list?.path;
    Widget row(IconData icon, String name, VoidCallback onPressed, Key key) =>
        TextButton(
          key: key,
          onPressed: _loading ? null : onPressed,
          style: AppButtons.quiet(p).copyWith(
            alignment: AlignmentDirectional.centerStart,
            minimumSize: const WidgetStatePropertyAll(Size.fromHeight(48)),
            padding: const WidgetStatePropertyAll(
              EdgeInsets.symmetric(horizontal: 10),
            ),
          ),
          child: Row(
            children: [
              Icon(icon, size: 20, color: p.muted),
              const SizedBox(width: 12),
              Expanded(
                child: Text(
                  name,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: uiText(size: 14, color: p.ink),
                ),
              ),
            ],
          ),
        );
    final rows = <Widget>[
      if (list != null && path != null && (list.parent != null || !_single))
        row(
          Icons.arrow_upward_rounded,
          l10n.upOneFolder,
          // At a volume's top the list of volumes is above it.
          () => unawaited(_go(list.parent)),
          const Key('android-folder-up'),
        ),
      if (list != null)
        for (final folder in list.folders)
          row(
            path == null ? Icons.sd_storage_outlined : Icons.folder_outlined,
            folder.name,
            () => unawaited(_go(folder.path)),
            Key('android-folder-${folder.path}'),
          ),
    ];
    return AppDialog(
      key: const Key('android-folder-chooser'),
      icon: Icons.folder_shared_rounded,
      title: l10n.chooseFolderShare,
      subtitle: list?.where ?? l10n.storage,
      subtitleKey: const Key('android-folder-where'),
      busy: _loading,
      body: [
        if (_error != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: KeepAllText(
              _error!,
              key: const Key('android-folder-error'),
              style: uiText(size: 12.5, color: p.red),
            ),
          ),
        SizedBox(
          height: 260,
          // Reading (the first list, or a folder that takes a while: a big
          // one, a slow card): said where the folders will be. Its result
          // or the reason above then takes its place.
          child: _loading && (list == null || _slow)
              ? Align(
                  alignment: Alignment.topLeft,
                  child: Padding(
                    padding: const EdgeInsets.all(12),
                    child: AppBusyLine(
                      l10n.androidReadingFolder,
                      textKey: const Key('android-folder-reading'),
                      trailing: const AppElapsedText(
                        after: Duration(seconds: 3),
                      ),
                    ),
                  ),
                )
              : list == null
              ? const SizedBox.shrink()
              : rows.isEmpty || (path != null && list.folders.isEmpty)
              ? Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    ...rows,
                    Padding(
                      padding: const EdgeInsets.all(12),
                      child: KeepAllText(
                        path == null ? l10n.couldntFindStorage : l10n.noFoldersInside,
                        key: const Key('android-folder-empty'),
                        style: uiText(size: 13, color: p.muted),
                      ),
                    ),
                  ],
                )
              : ListView.builder(
                  key: const Key('android-folder-list'),
                  itemCount: rows.length,
                  itemBuilder: (_, index) => rows[index],
                ),
        ),
      ],
      actions: [
        TextButton(
          key: const Key('android-folder-cancel'),
          onPressed: () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.cancel),
        ),
        FilledButton(
          key: const Key('android-folder-share'),
          onPressed: path == null || _loading
              ? null
              : () => Navigator.pop(context, path),
          style: AppButtons.filled(p),
          child: KeepAllText(l10n.shareFolder),
        ),
      ],
    );
  }
}
