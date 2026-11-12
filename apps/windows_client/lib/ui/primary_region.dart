import 'dart:ui' show DisplayFeature, DisplayFeatureState, DisplayFeatureType;

import 'package:flutter/material.dart';

/// Keeps a window's dialogs, menus and panels on one part of it.
///
/// In 모든 모니터 사용 one remote window spans two local monitors, and every
/// centred popup (a dialog, the disconnect question, a menu) landed on the
/// seam between them (2026-10-26). The remote view publishes the primary
/// monitor's part of the window in [region]; every route of this app then
/// sees the rest of the window as display features, the way a foldable's
/// hinge is described, so Flutter's dialogs and popup menus (which lay out
/// in the display-feature sub-screen) and [AnchoredPanelRoute] stay inside
/// that part. Null is the whole window: one monitor or an unknown layout.
class PrimaryRegionScope extends StatefulWidget {
  const PrimaryRegionScope({super.key, required this.child});
  final Widget child;

  /// [MaterialApp.builder] form: above the navigator, so routes see it.
  static Widget builder(BuildContext context, Widget? child) =>
      PrimaryRegionScope(child: child ?? const SizedBox.shrink());

  /// Where a remote view publishes its primary-monitor part (window logical
  /// coordinates); null outside a scope.
  static ValueNotifier<Rect?>? maybeOf(BuildContext context) =>
      context.getInheritedWidgetOfExactType<_PrimaryRegion>()?.region;

  /// The display features that leave exactly [region] of a [size] window:
  /// full-height bands beside it, then full-width bands above and below it
  /// (two monitors of different sizes leave such bands).
  static List<DisplayFeature> featuresFor(Rect region, Size size) {
    DisplayFeature band(Rect bounds) => DisplayFeature(
      bounds: bounds,
      type: DisplayFeatureType.unknown,
      state: DisplayFeatureState.unknown,
    );
    return [
      if (region.left > 0) band(Rect.fromLTRB(0, 0, region.left, size.height)),
      if (region.right < size.width)
        band(Rect.fromLTRB(region.right, 0, size.width, size.height)),
      if (region.top > 0) band(Rect.fromLTRB(0, 0, size.width, region.top)),
      if (region.bottom < size.height)
        band(Rect.fromLTRB(0, region.bottom, size.width, size.height)),
    ];
  }

  @override
  State<PrimaryRegionScope> createState() => _PrimaryRegionScopeState();
}

class _PrimaryRegionScopeState extends State<PrimaryRegionScope> {
  final _region = ValueNotifier<Rect?>(null);

  @override
  void dispose() {
    _region.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => _PrimaryRegion(
    region: _region,
    child: ValueListenableBuilder<Rect?>(
      valueListenable: _region,
      child: widget.child,
      builder: (context, region, child) {
        final media = MediaQuery.of(context);
        final window = Offset.zero & media.size;
        final inside = region?.intersect(window);
        // Always the same MediaQuery node, so the navigator keeps its state.
        return MediaQuery(
          data: inside == null || inside.isEmpty || inside == window
              ? media
              : media.copyWith(
                  displayFeatures: [
                    ...media.displayFeatures,
                    ...PrimaryRegionScope.featuresFor(inside, media.size),
                  ],
                ),
          child: child!,
        );
      },
    ),
  );
}

class _PrimaryRegion extends InheritedWidget {
  const _PrimaryRegion({required this.region, required super.child});
  final ValueNotifier<Rect?> region;
  @override
  bool updateShouldNotify(_PrimaryRegion old) => region != old.region;
}
