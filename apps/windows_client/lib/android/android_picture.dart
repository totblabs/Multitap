import 'package:flutter/widgets.dart';

/// One texture and the remote picture size it was created for.
@immutable
class AndroidShownPicture {
  const AndroidShownPicture(this.texture, this.size);
  final int texture;
  final Size size;

  @override
  bool operator ==(Object other) =>
      other is AndroidShownPicture &&
      other.texture == texture &&
      other.size == size;

  @override
  int get hashCode => Object.hash(texture, size);
}

/// Which picture to draw while the remote resolution changes. A texture is
/// always drawn at the size it was created for: the session announces the
/// new size before its new texture exists, and a new texture has no frame
/// until the resize finishes. Until then the previous picture stays on
/// screen at its own shape (letterboxed), never stretched to the new one and
/// never blank. The Android engine keeps a retired texture's last frame
/// until the next one has a frame.
class AndroidPictureContinuity {
  final _sizes = <int, Size>{};
  AndroidShownPicture? _shown;

  AndroidShownPicture? get shown => _shown;

  /// [texture], [size]: the session's current texture and picture size;
  /// [settled]: the session is not changing its resolution (a texture it
  /// created has decoded a frame).
  AndroidShownPicture? update({
    required int? texture,
    required Size size,
    required bool settled,
  }) {
    if (texture == null) return _shown;
    // The first sight of a texture comes after the session set its size.
    final own = AndroidShownPicture(
      texture,
      _sizes.putIfAbsent(texture, () => size),
    );
    if (_shown == null || _shown!.texture == texture || settled) {
      _sizes.removeWhere((id, _) => id != texture);
      return _shown = own;
    }
    return _shown;
  }

  /// A new connection starts without a previous picture.
  void reset() {
    _sizes.clear();
    _shown = null;
  }
}
