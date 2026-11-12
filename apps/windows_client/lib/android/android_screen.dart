import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/foundation.dart' show immutable;
import 'package:flutter/services.dart';

import '../data/display_scale.dart' show nearestDisplayScale;
import '../l10n/l10n.dart';

/// The kinds of screen whose 화면 choices this device remembers apart: a
/// Fold user's cover screen (a phone) and inner screen want different
/// sizes, and so does a tablet.
///
/// Bucketed by the window's smallest width, as Android sizes layouts:
/// below 600 dp a phone; from 600 dp a foldable's inner screen when the
/// panel measures less than 9 inches across (it is held like a book),
/// otherwise a tablet. Without a physical size the 600–719 dp bucket stands
/// for the inner screens (Fold 5/6/7/8 ≈ 690–760 dp, tablets ≥ 800 dp).
enum AndroidDeviceClass {
  phone,
  fold,
  tablet,
  television;

  const AndroidDeviceClass();
  String get label => switch (this) {
    AndroidDeviceClass.phone => l10n.phone,
    AndroidDeviceClass.fold => l10n.foldableInnerScreen,
    AndroidDeviceClass.tablet => l10n.tablet,
    AndroidDeviceClass.television => 'TV',
  };

  static AndroidDeviceClass of(Size window, {double? diagonalInches}) {
    final shortest = window.shortestSide;
    if (!shortest.isFinite || shortest < 600) return phone;
    if (diagonalInches != null &&
        diagonalInches.isFinite &&
        diagonalInches > 0) {
      return diagonalInches < 9 ? fold : tablet;
    }
    return shortest < 720 ? fold : tablet;
  }

  static AndroidDeviceClass parse(String? value) =>
      values.firstWhere((c) => c.name == value, orElse: () => phone);
}

/// This display's physical pixel density (Android's DisplayMetrics xdpi and
/// ydpi) and its full size in pixels, read through `multitap/android/display`
/// (AndroidDisplay.kt). Android's device pixel ratio is a density bucket
/// (2, 2.625, 3 …), not a measurement: a 2560×1600 11-inch tablet and a
/// 10-inch 1920×1200 one can both report 2, although one shows 274 and the
/// other 224 pixels per inch.
@immutable
class AndroidScreenDensity {
  const AndroidScreenDensity({
    required this.xdpi,
    required this.ydpi,
    required this.width,
    required this.height,
  });
  final double xdpi, ydpi;

  /// The whole display in pixels (not just this window).
  final double width, height;

  static const channel = MethodChannel('multitap/android/display');

  /// The current display's metrics, or null where the engine cannot say
  /// (tests, an older APK): the density bucket stands in.
  static Future<AndroidScreenDensity?> read() async {
    try {
      final value = await channel.invokeMapMethod<String, Object?>('metrics');
      return value == null ? null : parse(value);
    } catch (_) {
      return null;
    }
  }

  static AndroidScreenDensity? parse(Map<String, Object?> value) {
    double? number(String key) {
      final v = value[key];
      return v is num && v.isFinite && v > 0 ? v.toDouble() : null;
    }

    final xdpi = number('xdpi'), ydpi = number('ydpi');
    final width = number('width'), height = number('height');
    if (xdpi == null || ydpi == null || width == null || height == null) {
      return null;
    }
    return AndroidScreenDensity(
      xdpi: xdpi,
      ydpi: ydpi,
      width: width,
      height: height,
    );
  }

  /// Pixels per inch. Some panels report the density bucket, 0 or another
  /// screen's value; anything further than ×0.6–×1.6 from the bucket
  /// ([devicePixelRatio] × 160) is not believed and the bucket is used.
  double ppi(double devicePixelRatio) => androidPpi(this, devicePixelRatio);

  double diagonalInches(double devicePixelRatio) =>
      math.sqrt(width * width + height * height) / ppi(devicePixelRatio);

  @override
  bool operator ==(Object other) =>
      other is AndroidScreenDensity &&
      other.xdpi == xdpi &&
      other.ydpi == ydpi &&
      other.width == width &&
      other.height == height;

  @override
  int get hashCode => Object.hash(xdpi, ydpi, width, height);
}

/// See [AndroidScreenDensity.ppi]; [density] null: the bucket.
double androidPpi(AndroidScreenDensity? density, double devicePixelRatio) {
  final nominal =
      (devicePixelRatio.isFinite && devicePixelRatio > 0
          ? devicePixelRatio
          : 1.0) *
      160;
  if (density == null) return nominal;
  final measured = (density.xdpi + density.ydpi) / 2;
  return measured >= nominal * .6 && measured <= nominal * 1.6
      ? measured
      : nominal;
}

/// The fixed 화면 배율 choices (Windows display scale, percent); 0 is 자동.
const androidScreenScales = [100, 125, 150, 175, 200];
const androidTvScreenScales = [100, 125, 150, 175, 200, 225, 250, 300];

/// 해상도: which remote output this screen asks for.
enum AndroidResolutionKind {
  /// A density-adaptive handheld source with a2560 long-edge safety bound.
  /// TV keeps native pixels; an older Host retains its integer fallback.
  auto,

  /// Every device pixel (sharpest).
  device,

  /// Half the pixels each way, drawn 2:1 without smoothing: a quarter of
  /// the work for the Host and the decoder.
  half,

  /// A fixed size of this screen's shape (its short side), drawn scaled.
  preset,

  /// An aspect-preserving handheld source bounded by its longer edge.
  longEdge,

  /// Legacy 16:9 workspace API. Saved desktop choices migrate to workspace.
  desktop,

  /// Device-shaped workspace, enlarged relative to this app's viewport.
  workspace,

  /// 선명하게: (up to) the panel's own pixels with Windows' scale raised by
  /// the same factor, so things keep their size (android_sharp_picture.dart).
  sharp,
}

@immutable
class AndroidResolution {
  const AndroidResolution._(this.kind, [this.shortSide = 0]);
  const AndroidResolution.preset(int shortSide)
    : this._(AndroidResolutionKind.preset, shortSide);
  const AndroidResolution.longEdge(int pixels)
    : this._(AndroidResolutionKind.longEdge, pixels);
  const AndroidResolution.desktop(int shortSide)
    : this._(AndroidResolutionKind.desktop, shortSide);
  const AndroidResolution.workspace(int percent)
    : this._(AndroidResolutionKind.workspace, percent);
  static const auto = AndroidResolution._(AndroidResolutionKind.auto);
  static const device = AndroidResolution._(AndroidResolutionKind.device);
  static const half = AndroidResolution._(AndroidResolutionKind.half);
  static const sharp = AndroidResolution._(AndroidResolutionKind.sharp);

  final AndroidResolutionKind kind;

  /// Preset/desktop: short side in pixels. Workspace: relative percent.
  final int shortSide;

  /// The preset short sides offered between 절반 and 기기 화소.
  static const presets = [720, 900, 1080, 1200, 1440];
  static const longEdges = [1280, 1600, 1920, 2560];
  static const recommendedLongEdge = 2560;
  static const desktops = [1080, 1440, 2160];
  static const workspaces = [100, 125, 150, 200];

  int get workspacePercent => shortSide;
  String get workspaceLabel => switch (shortSide) {
    100 => l10n.fitDevice,
    125 => l10n.n125,
    150 => l10n.n15,
    200 => l10n.n2,
    _ => '',
  };

  Size workspaceSize(Size physical) {
    final factor = workspacePercent / 100;
    double even(double side) => (side * factor).floor() ~/ 2 * 2.0;
    return Size(even(physical.width), even(physical.height));
  }

  String get desktopLabel => switch (shortSide) {
    1080 => 'FHD',
    1440 => 'QHD',
    2160 => '4K',
    _ => '',
  };

  Size desktopSize(Size physical) {
    final long = shortSide * 16 / 9;
    return physical.width >= physical.height
        ? Size(long, shortSide.toDouble())
        : Size(shortSide.toDouble(), long);
  }

  String encode() => switch (kind) {
    AndroidResolutionKind.preset => 'p$shortSide',
    AndroidResolutionKind.longEdge => 'l$shortSide',
    AndroidResolutionKind.desktop => 'd$shortSide',
    AndroidResolutionKind.workspace => 'w$shortSide',
    _ => kind.name,
  };

  static AndroidResolution parse(Object? value) {
    if (value is! String) return auto;
    if (value.startsWith('l')) {
      final edge = int.tryParse(value.substring(1));
      return edge != null && longEdges.contains(edge)
          ? AndroidResolution.longEdge(edge)
          : auto;
    }
    if (value == 'device') return const AndroidResolution.workspace(100);
    if (value.startsWith('w')) {
      final percent = int.tryParse(value.substring(1));
      return percent != null && workspaces.contains(percent)
          ? AndroidResolution.workspace(percent)
          : auto;
    }
    if (value.startsWith('p')) {
      final side = int.tryParse(value.substring(1));
      return side != null && presets.contains(side)
          ? AndroidResolution.preset(side)
          : auto;
    }
    if (value.startsWith('d')) {
      final side = int.tryParse(value.substring(1));
      if (side != null) {
        return switch (side) {
          1080 => const AndroidResolution.workspace(100),
          1440 => const AndroidResolution.workspace(150),
          2160 => const AndroidResolution.workspace(200),
          _ => auto,
        };
      }
    }
    return switch (value) {
      'half' => half,
      'sharp' => sharp,
      _ => auto,
    };
  }

  @override
  bool operator ==(Object other) =>
      other is AndroidResolution &&
      other.kind == kind &&
      other.shortSide == shortSide;

  @override
  int get hashCode => Object.hash(kind, shortSide);
}

/// 화면 방향 during a session (ToTBDesk USABILITY-2.3.6): the device's own
/// rotation, or held in landscape or portrait.
enum AndroidOrientation {
  auto,
  landscape,
  portrait;

  const AndroidOrientation();
  String get label => switch (this) {
    AndroidOrientation.auto => l10n.autoRotate,
    AndroidOrientation.landscape => l10n.landscape,
    AndroidOrientation.portrait => l10n.portrait,
  };

  static AndroidOrientation parse(Object? value) =>
      values.firstWhere((o) => o.name == value, orElse: () => auto);

  List<DeviceOrientation> get preferred => switch (this) {
    auto => const [],
    landscape => const [
      DeviceOrientation.landscapeLeft,
      DeviceOrientation.landscapeRight,
    ],
    portrait => const [DeviceOrientation.portraitUp],
  };
}

/// What one kind of screen ([AndroidDeviceClass]) remembers: 화면 배율
/// ([scale], percent, 0 = 자동), 해상도 and 화면 방향.
@immutable
class AndroidScreenChoice {
  const AndroidScreenChoice({
    this.scale = 0,
    this.resolution = AndroidResolution.auto,
    this.orientation = AndroidOrientation.auto,
  });
  final int scale;
  final AndroidResolution resolution;
  final AndroidOrientation orientation;

  AndroidScreenChoice copyWith({
    int? scale,
    AndroidResolution? resolution,
    AndroidOrientation? orientation,
  }) => AndroidScreenChoice(
    scale: scale ?? this.scale,
    resolution: resolution ?? this.resolution,
    orientation: orientation ?? this.orientation,
  );

  String encode() =>
      jsonEncode({'s': scale, 'r': resolution.encode(), 'o': orientation.name});

  static AndroidScreenChoice parse(String? value) {
    try {
      final data = jsonDecode(value ?? '');
      if (data is Map) {
        final scale = data['s'];
        return AndroidScreenChoice(
          scale: scale is int && androidTvScreenScales.contains(scale)
              ? scale
              : 0,
          resolution: AndroidResolution.parse(data['r']),
          orientation: AndroidOrientation.parse(data['o']),
        );
      }
    } on FormatException {
      // The defaults below.
    }
    return const AndroidScreenChoice();
  }

  /// Admit saved handheld choices in memory; reading never rewrites storage
  /// or changes an active Windows output. TV retains its existing choices.
  AndroidScreenChoice forDevice(AndroidDeviceClass kind) {
    if (kind == AndroidDeviceClass.television) return this;
    return AndroidScreenChoice(
      scale: scale == 0 ? 0 : scale.clamp(100, 200),
      resolution:
          resolution.kind == AndroidResolutionKind.sharp ||
              resolution.kind == AndroidResolutionKind.longEdge &&
                  AndroidResolution.longEdges.contains(resolution.shortSide)
          ? resolution
          : AndroidResolution.auto,
    );
  }

  @override
  bool operator ==(Object other) =>
      other is AndroidScreenChoice &&
      other.scale == scale &&
      other.resolution == resolution &&
      other.orientation == orientation;

  @override
  int get hashCode => Object.hash(scale, resolution, orientation);
}

/// How this screen asks for its remote picture: the size (해상도) and the
/// Windows display scale (화면 배율) for [choice] on a screen of
/// [deviceClass] with [density].
///
/// 자동 배율:
/// * Phones (smallest width < 600 dp) keep the tuned rule: Windows at 100 %
///   gets 1.6 remote pixels per dp, so the scale is device pixel ratio / 1.6
///   per device pixel of a remote pixel (200 % on a 3× phone at its own
///   pixels).
/// * From 600 dp (foldable inner screens, tablets) the scale comes from the
///   panel's measured pixels per inch: Windows' 100 % is made for a 96-dpi
///   monitor seen from about 65 cm, so a Windows pixel should cover the same
///   angle at the distance this screen is held, about 12 cm × √(diagonal in
///   inches) — 34 cm for an 8-inch inner screen, 40 cm for an 11-inch and
///   46 cm for a 14.6-inch tablet:
///
///       scale = remote ppi × distance (cm) / (96 × 65)
///
///   where remote ppi = panel ppi / device pixels per remote pixel. An
///   At native size, an11-inch2560×1600 tablet (274ppi) has a175% baseline.
///   An explicit1920×1200 source reduces its recommendation to125%; the
///   automatic source uses that readability ratio at the initial100% scale.
///   Explicit resolutions retain their own requested size. The
///   chosen source's pixel ratio is included before rounding, and handheld
///   recommendations/manual choices stay within100..200%. Windows may
///   allow less; only the Host's reported applied value proves the result.
/// * TV uses the remote viewport's pixel size, independent of panel PPI or
///   a handheld viewing-distance estimate: 1080p starts at 150 %, 4K at
///   225 %. These conservative defaults can still be changed in 화면.
@immutable
class AndroidScreenPolicy {
  const AndroidScreenPolicy({
    this.deviceClass = AndroidDeviceClass.phone,
    this.choice = const AndroidScreenChoice(),
    this.density,
    this.window,
    this.sharp,
  });

  /// [window] in dp, [density] as measured (null: the density bucket).
  factory AndroidScreenPolicy.of(
    Size window,
    double devicePixelRatio,
    AndroidScreenDensity? density,
    AndroidScreenChoice Function(AndroidDeviceClass kind) choice, {
    bool television = false,
    Size? sharp,
  }) {
    final kind = television
        ? AndroidDeviceClass.television
        : AndroidDeviceClass.of(
            window,
            diagonalInches: density?.diagonalInches(devicePixelRatio),
          );
    return AndroidScreenPolicy(
      deviceClass: kind,
      choice: choice(kind),
      density: density,
      window: window,
      sharp: sharp,
    );
  }

  /// 선명하게: the size the engine planned for this picture area (null: not
  /// planned, which is the automatic size).
  final Size? sharp;

  final AndroidDeviceClass deviceClass;
  final AndroidScreenChoice choice;
  final AndroidScreenDensity? density;

  /// This window in dp (a diagonal estimate without measured metrics).
  final Size? window;

  /// Phones: remote Windows pixels at 100 % per dp.
  static const phonePixelsPerDp = 1.6;

  /// Windows' 100 %: 96 pixels per inch seen from 65 cm.
  static const referenceDpiCm = 96 * 65;

  /// The panel's diagonal (inches), or without measured metrics the window
  /// in density-bucket inches (160 dp each).
  double _diagonal(double dpr) {
    final measured = density;
    if (measured != null) return measured.diagonalInches(dpr);
    final size = window;
    if (size == null || size.isEmpty) return 11;
    return math.sqrt(size.width * size.width + size.height * size.height) / 160;
  }

  /// How far away this screen is held (cm): 12 cm × √(diagonal inches).
  static double viewingDistanceCm(double diagonalInches) =>
      12 * math.sqrt(diagonalInches.clamp(4.0, 32.0));

  /// The Windows scale (1.0 = 100 %) 자동 picks when one remote pixel spans
  /// [devicePixels] device pixels.
  double autoRatio(double devicePixelRatio, double devicePixels) {
    final dpr = devicePixelRatio.isFinite && devicePixelRatio > 0
        ? devicePixelRatio
        : 1.0;
    final p = devicePixels.isFinite && devicePixels > 0 ? devicePixels : 1.0;
    if (deviceClass == AndroidDeviceClass.television) {
      // Use this app window, not the whole HDMI panel, so half-resolution
      // and smaller remote viewports get an appropriate Windows scale too.
      final size = window;
      final short = size != null && !size.isEmpty
          ? size.shortestSide * dpr / p
          : density != null
          ? math.min(density!.width, density!.height) / p
          : 1080 / p;
      // Midpoint boundaries tolerate system bars and small viewport insets.
      return switch (short) {
        >= 2880 => 3.0,
        >= 1800 => 2.25,
        >= 1260 => 1.75,
        >= 900 => 1.5,
        >= 600 => 1.25,
        _ => 1.0,
      };
    }
    if (deviceClass == AndroidDeviceClass.phone) {
      return dpr / phonePixelsPerDp / p;
    }
    final ppi = androidPpi(density, dpr);
    final distance = viewingDistanceCm(_diagonal(dpr));
    return ppi / p * distance / referenceDpiCm;
  }

  /// A fixed workspace keeps the device's automatic text scale, so choosing
  /// more pixels actually adds workspace instead of raising Windows DPI too.
  /// A manual Windows scale always wins.
  int windowsScale(double devicePixelRatio, double devicePixels) {
    final wanted = choice.scale != 0
        ? choice.scale
        : nearestDisplayScale(
            autoRatio(
              devicePixelRatio,
              (choice.resolution.kind == AndroidResolutionKind.desktop ||
                      choice.resolution.kind == AndroidResolutionKind.workspace)
                  ? 1
                  : devicePixels,
            ),
          );
    return deviceClass == AndroidDeviceClass.television
        ? wanted
        : wanted.clamp(100, 200);
  }

  /// Automatic source for the actual picture area. The existing readability
  /// ratio adapts to DPR, validated panel density and viewing distance; a
  /// manual Windows scale never changes this recommendation.
  Size recommendedSize(Size physical, double devicePixelRatio) {
    if (!physical.width.isFinite ||
        !physical.height.isFinite ||
        physical.isEmpty) {
      return Size.zero;
    }
    if (deviceClass == AndroidDeviceClass.television) {
      return Size(
        physical.width.floor() ~/ 2 * 2.0,
        physical.height.floor() ~/ 2 * 2.0,
      );
    }
    final wanted = math.max(1.0, autoRatio(devicePixelRatio, 1));
    final widthMinimum = physical.width / 320;
    final heightMinimum = physical.height / 180;
    final minimum = math.max(1.0, math.min(widthMinimum, heightMinimum));
    final divisor = math.min(wanted, minimum);
    // Keep the exact coordinate which bound the minimum divisor. Dividing it
    // back can yield179.99999999999997 and even-floor to178 instead of180.
    return _longEdgeSize(
      Size(
        divisor == widthMinimum && physical.width >= 320
            ? 320.0
            : physical.width / divisor,
        divisor == heightMinimum && physical.height >= 180
            ? 180.0
            : physical.height / divisor,
      ),
      AndroidResolution.recommendedLongEdge,
    );
  }

  /// Automatic sizing adapts; explicit long-edge choices keep their original
  /// independent limit. Legacy/TV modes retain their explicit size rules.
  Size base(Size physical, [double devicePixelRatio = 1]) {
    final resolution = choice.resolution;
    if (resolution.kind == AndroidResolutionKind.sharp &&
        deviceClass != AndroidDeviceClass.television) {
      final planned = sharp;
      return planned == null || planned.isEmpty
          ? recommendedSize(physical, devicePixelRatio)
          : planned;
    }
    if (resolution.kind == AndroidResolutionKind.longEdge) {
      return _longEdgeSize(
        physical,
        resolution.shortSide.clamp(1280, AndroidResolution.recommendedLongEdge),
      );
    }
    if (deviceClass != AndroidDeviceClass.television &&
        resolution.kind == AndroidResolutionKind.auto) {
      return recommendedSize(physical, devicePixelRatio);
    }
    if (resolution.kind == AndroidResolutionKind.workspace &&
        AndroidResolution.workspaces.contains(resolution.workspacePercent)) {
      return resolution.workspaceSize(physical);
    }
    if (resolution.kind == AndroidResolutionKind.desktop &&
        AndroidResolution.desktops.contains(resolution.shortSide)) {
      return resolution.desktopSize(physical);
    }
    final short = physical.shortestSide;
    if (resolution.kind != AndroidResolutionKind.preset ||
        !short.isFinite ||
        short <= 0 ||
        resolution.shortSide >= short) {
      return physical;
    }
    return _presetSize(physical, resolution.shortSide);
  }

  /// Integer divisor of [base]:1 for device/presets,2 for half, and for
  /// auto1 where the Host sets the Windows scale ([hostScale]),
  /// otherwise the whole fraction nearest the wanted size (never shorter
  /// than480 rows). A bounded base can already differ from device pixels;
  /// AndroidViewportPlan reports fractional fitting separately as0.
  int pixelScale(
    double devicePixelRatio,
    Size base, {
    required bool hostScale,
  }) {
    final short = base.shortestSide;
    switch (choice.resolution.kind) {
      case AndroidResolutionKind.device ||
          AndroidResolutionKind.sharp ||
          AndroidResolutionKind.longEdge ||
          AndroidResolutionKind.preset ||
          AndroidResolutionKind.desktop ||
          AndroidResolutionKind.workspace:
        return 1;
      case AndroidResolutionKind.half:
        return short / 2 >= 360 ? 2 : 1;
      case AndroidResolutionKind.auto:
        if (hostScale) return 1;
        final wanted = choice.scale != 0
            ? choice.scale / 100
            : autoRatio(devicePixelRatio, 1);
        final limit = math.max(1, (short / 480).floor());
        return wanted.round().clamp(1, limit);
    }
  }
}

Size _longEdgeSize(Size physical, int maximum) {
  if (!physical.width.isFinite ||
      !physical.height.isFinite ||
      physical.isEmpty) {
    return Size.zero;
  }
  final factor = math.min(1.0, maximum / physical.longestSide);
  double even(double side) => (side * factor).floor() ~/ 2 * 2.0;
  return Size(even(physical.width), even(physical.height));
}

/// [physical]'s shape with a short side of [side] pixels.
Size _presetSize(Size physical, int side) {
  final factor = side / physical.shortestSide;
  return Size(
    (physical.width * factor).roundToDouble(),
    (physical.height * factor).roundToDouble(),
  );
}

/// One 해상도 option for this screen as offered in the 화면 panel.
@immutable
class AndroidResolutionOption {
  const AndroidResolutionOption(
    this.resolution,
    this.label,
    this.size, {
    this.deviceSize = false,
  });
  final AndroidResolution resolution;
  final String label;

  /// The remote size it asks for now (null for 자동).
  final Size? size;

  /// Requested source equals this device's even physical size. This is not
  /// evidence of the Host's applied source or an on-screen1to1 presentation.
  final bool deviceSize;
}

/// Resolution choices for [physical]: automatic, device-shaped fit, half,
/// lower presets and larger workspaces of the same viewport's shape.
List<AndroidResolutionOption> androidResolutionOptions(
  Size physical, {
  AndroidDeviceClass deviceClass = AndroidDeviceClass.phone,
  AndroidScreenPolicy? policy,
  double devicePixelRatio = 1,
  bool sharp = false,
}) {
  final mode = policy ?? AndroidScreenPolicy(deviceClass: deviceClass);
  if (mode.deviceClass != AndroidDeviceClass.television) {
    final automatic = mode.recommendedSize(physical, devicePixelRatio);
    final options = <AndroidResolutionOption>[
      AndroidResolutionOption(AndroidResolution.auto, l10n.autoRecommended, automatic),
      // 선명하게, where Windows' scale can follow and the panel has more
      // pixels than the automatic size (the planned size once known).
      if (sharp &&
          !automatic.isEmpty &&
          physical.width >= automatic.width + 2 &&
          physical.height >= automatic.height + 2)
        AndroidResolutionOption(
          AndroidResolution.sharp,
          l10n.sharp,
          mode.sharp ??
              Size(
                physical.width.floor() ~/ 2 * 2.0,
                physical.height.floor() ~/ 2 * 2.0,
              ),
        ),
    ];
    // Automatic remains a distinct preference; duplicate explicit sizes on a
    // smaller screen are offered once, without upsampling it.
    final sizes = <Size>{};
    for (final edge in AndroidResolution.longEdges) {
      final size = _longEdgeSize(physical, edge);
      if (size.isEmpty || !sizes.add(size)) continue;
      final deviceSize =
          physical.longestSide <= AndroidResolution.recommendedLongEdge &&
          size ==
              _longEdgeSize(physical, AndroidResolution.recommendedLongEdge);
      options.add(
        AndroidResolutionOption(
          AndroidResolution.longEdge(edge),
          deviceSize
              ? l10n.fitDevice
              : '${size.width.toInt()}×${size.height.toInt()}',
          size,
          deviceSize: deviceSize,
        ),
      );
    }
    return options;
  }
  // The same even sizes AndroidViewportPlan asks for.
  double even(double v, [int by = 1]) => ((v / by).floor() ~/ 2 * 2).toDouble();
  final short = physical.shortestSide;
  if (!short.isFinite || short <= 0) {
    return [AndroidResolutionOption(AndroidResolution.auto, l10n.auto, null)];
  }
  final options = <AndroidResolutionOption>[
    AndroidResolutionOption(AndroidResolution.auto, l10n.auto, null),
    AndroidResolutionOption(
      const AndroidResolution.workspace(100),
      l10n.fitDevice,
      Size(even(physical.width), even(physical.height)),
    ),
    if (short / 2 >= 360)
      AndroidResolutionOption(
        AndroidResolution.half,
        l10n.half,
        Size(even(physical.width, 2), even(physical.height, 2)),
      ),
  ];
  final between = [
    for (final side in AndroidResolution.presets)
      if (side > short / 2 + 1 && side < short - 1) side,
  ];
  final picked = between.length <= 3
      ? between
      : [between.first, between[between.length ~/ 2], between.last];
  for (final side in picked) {
    final preset = _presetSize(physical, side);
    final size = Size(even(preset.width), even(preset.height));
    options.add(
      AndroidResolutionOption(
        AndroidResolution.preset(side),
        '${size.width.toInt()}×${size.height.toInt()}',
        size,
      ),
    );
  }
  for (final percent in AndroidResolution.workspaces.where((p) => p > 100)) {
    final resolution = AndroidResolution.workspace(percent);
    options.add(
      AndroidResolutionOption(
        resolution,
        resolution.workspaceLabel,
        resolution.workspaceSize(physical),
      ),
    );
  }
  return options;
}
