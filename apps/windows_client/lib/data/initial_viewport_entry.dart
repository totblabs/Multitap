/// First source intent, independent of legitimate encoder downscaling.
class InitialViewportEntry {
  InitialViewportEntry(this.requested) : latest = requested;
  final (int, int) requested;
  (int, int) latest;
  void observe((int, int) source) => latest = source;
  bool get changed => latest != requested;
  bool satisfied({
    required int? sourceWidth,
    required int? sourceHeight,
    required bool decoded,
    required bool changing,
  }) => decoded && !changing && (sourceWidth, sourceHeight) == latest;
}
