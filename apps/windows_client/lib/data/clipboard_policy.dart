/// The broker owns the permission decision. Missing policy always denies sharing.
class ClipboardPolicy {
  const ClipboardPolicy({
    this.textToRemote = false,
    this.textToLocal = false,
    this.filesToRemote = false,
    this.filesToLocal = false,
  });
  const ClipboardPolicy.all()
    : textToRemote = true,
      textToLocal = true,
      filesToRemote = true,
      filesToLocal = true;
  final bool textToRemote, textToLocal, filesToRemote, filesToLocal;
  bool get toRemote => textToRemote || filesToRemote;
  bool get toLocal => textToLocal || filesToLocal;
  bool get any => toRemote || toLocal;
  /// Plain and rich text need the text permission; files, folder trees and
  /// images (binary content) need the files permission.
  bool allows(String format, {required bool toRemote}) => switch (format) {
    'text' || 'richtext' => toRemote ? textToRemote : textToLocal,
    'files' || 'image' || 'tree' => toRemote ? filesToRemote : filesToLocal,
    _ => false,
  };
  static const _keys = [
    'textToRemote',
    'textToLocal',
    'filesToRemote',
    'filesToLocal',
  ];
  factory ClipboardPolicy.fromJson(Object? value) {
    if (value == null) return const ClipboardPolicy();
    if (value is! Map ||
        value.length != 4 ||
        !_keys.every((key) => value[key] is bool)) {
      throw const FormatException('Invalid clipboard policy.');
    }
    return ClipboardPolicy(
      textToRemote: value['textToRemote'] as bool,
      textToLocal: value['textToLocal'] as bool,
      filesToRemote: value['filesToRemote'] as bool,
      filesToLocal: value['filesToLocal'] as bool,
    );
  }
  ClipboardPolicy intersect(ClipboardPolicy other) => ClipboardPolicy(
    textToRemote: textToRemote && other.textToRemote,
    textToLocal: textToLocal && other.textToLocal,
    filesToRemote: filesToRemote && other.filesToRemote,
    filesToLocal: filesToLocal && other.filesToLocal,
  );
}
