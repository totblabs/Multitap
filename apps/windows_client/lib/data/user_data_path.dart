import 'dart:io';

/// This client runs unelevated (since 2026-10-27; an administrator may still
/// start it elevated), and its per-user files live in %LOCALAPPDATA%, where any
/// program of the same user can put a junction or symbolic link. Before this
/// app writes or deletes there, every entry from %LOCALAPPDATA% down to [path]
/// must be missing or a plain folder/file, never a link that would redirect the
/// write somewhere else (more so when started elevated). A path outside %LOCALAPPDATA% (tests) is checked
/// from its own folder down. Throws [FileSystemException] naming the link.
///
/// The check runs right before each write; the native writers (thumbnails,
/// installer) additionally hold every folder open while they write.
void requirePlainUserDataPath(String path, {String? localAppData}) {
  final separator = Platform.isWindows ? r'\' : '/';
  String normal(String value) {
    var text = Platform.isWindows ? value.replaceAll('/', r'\') : value;
    while (text.length > 1 && text.endsWith(separator)) {
      text = text.substring(0, text.length - 1);
    }
    return text;
  }

  final full = normal(File(path).absolute.path);
  final local = normal(
    localAppData ?? Platform.environment['LOCALAPPDATA'] ?? '',
  );
  final inside =
      local.isNotEmpty &&
      full.toLowerCase().startsWith('${local.toLowerCase()}$separator');
  final anchor = inside
      ? full.substring(0, local.length)
      : normal(File(full).parent.path);
  if (!full.startsWith(anchor) || full.length <= anchor.length) return;
  var current = anchor;
  for (final part in full.substring(anchor.length).split(separator)) {
    if (part.isEmpty) continue;
    current = '$current$separator$part';
    if (FileSystemEntity.typeSync(current, followLinks: false) ==
        FileSystemEntityType.link) {
      throw FileSystemException(
        'A link in per-user data was refused.',
        current,
      );
    }
  }
}

/// Writes [contents] to a per-user data [file] (creating its folder) only
/// through plain folders, see [requirePlainUserDataPath].
Future<void> writeUserDataFile(File file, String contents) async {
  requirePlainUserDataPath(file.path);
  await file.parent.create(recursive: true);
  requirePlainUserDataPath(file.path);
  await file.writeAsString(contents, flush: true);
}
