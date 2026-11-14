/// The last path segment of a wire name ("a/b\c.txt" is "c.txt"); null for an empty one.
String? clipboardLeaf(String? name) {
  if (name == null || name.isEmpty) return null;
  final cut = name.lastIndexOf(RegExp(r'[\\/]'));
  final leaf = cut < 0 ? name : name.substring(cut + 1);
  return leaf.isEmpty ? null : leaf;
}

/// A snapshot of the running clipboard transfer. [sending] is this PC → VM.
/// [format] is `text`, `richtext`, `image`, `files` or `tree`; for a folder tree
/// [fileCount] counts its files and [folderCount] its folders.
class ClipboardProgress {
  const ClipboardProgress({
    required this.sending,
    required this.format,
    required this.fileCount,
    required this.fileIndex,
    required this.transferred,
    required this.total,
    this.folderCount = 0,
    this.fileName,
    this.asyncConsumer = false,
    this.topLevel = const [],
    this.listed,
  });

  /// A copy whose list is walked when it is pasted (deferred manifest): the number of items walked so far while that runs; null
  /// otherwise.
  final int? listed;
  final bool sending;
  final String format;
  final int fileCount, fileIndex, transferred, total, folderCount;

  /// The file in progress (the last path segment); null while unknown or for text and images.
  final String? fileName;

  /// A paste on this PC: the names of the pasted items themselves (the first
  /// path segment of each file, a few at most). Only to recognise them where
  /// Explorer creates them; empty otherwise.
  final List<String> topLevel;

  /// A paste of virtual files that Windows itself reports progress for (Explorer started an
  /// asynchronous operation and shows its own copy dialog): the app shows no card of its own.
  final bool asyncConsumer;
}
