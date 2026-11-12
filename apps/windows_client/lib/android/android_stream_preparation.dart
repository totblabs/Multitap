import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart' show immutable;
import 'package:flutter/widgets.dart' show Size;

import '../data/broker_api.dart';
import '../data/initial_viewport.dart';
import '../data/stream_size.dart';
import '../data/track_stream.dart';
import '../models/session_cleanup.dart';
import '../models/stream_options.dart';
import '../models/stream_preparation.dart';
import 'android_screen.dart'
    show
        AndroidScreenPolicy,
        AndroidResolutionKind,
        AndroidDeviceClass,
        AndroidResolution;

/// Android presents one remote output. An external desktop's existing two
/// outputs must not silently become a wide composite on the phone/tablet.
/// [portrait] (stream-size-v1): the output may be up to 3840 rows tall (the
/// longer side at most 3840, one 4K frame's pixels), otherwise 3840x2160.
StreamOptions androidViewportOptions(
  Size physical,
  StreamOptions quality, {
  bool portrait = false,
  bool fullWorkspace = false,
}) {
  if (!physical.width.isFinite ||
      !physical.height.isFinite ||
      physical.width <= 0 ||
      physical.height <= 0) {
    throw const BrokerException('invalid_response');
  }
  final scale = fullWorkspace
      ? 1.0
      : math.min(
          1.0,
          portrait
              ? math.min(
                  math.min(3840 / physical.width, 3840 / physical.height),
                  math.sqrt(8294400 / (physical.width * physical.height)),
                )
              : math.min(3840 / physical.width, 2160 / physical.height),
        );
  final width = (physical.width * scale).floor() ~/ 2 * 2;
  final height = (physical.height * scale).floor() ~/ 2 * 2;
  if (width < 320 ||
      height < 180 ||
      width > 0x7fffffff ||
      height > 0x7fffffff ||
      width * height > 0x1fffffffffffff) {
    throw const BrokerException('invalid_response');
  }
  return StreamOptions(
    maxWidth: width,
    maxHeight: height,
    fps: quality.fps,
    bitrate: quality.bitrate,
    audio: quality.audio,
    monitor: '0',
    // 자동 (the default) must reach the broker request, where it is
    // negotiated (bitrate-adaptive-v1, else the released fixed 20 Mbps).
    bitrateMode: quality.bitrateMode,
  );
}

/// The remote output this screen asks for. [pixelScale] device pixels show
/// one remote pixel (1 = the device's own pixels,0 =fractional fit);
/// [displayScale] is the
/// Windows display scale requested with display-scale-v1 (null for Hosts
/// without it, which get an exact integer fraction of the pixels instead).
@immutable
class AndroidViewportPlan {
  const AndroidViewportPlan({
    required this.width,
    required this.height,
    required this.pixelScale,
    this.displayScale,
  });

  /// [physical]: the picture area in device pixels; [mode]: this screen's
  /// 해상도 and 화면 배율 (android_screen.dart). [portraitLimits]: the decoder
  /// limits of a stream-size-v1 connection (declared or about to be): the
  /// output may then be portrait up to 2160x3840 within them at [fps].
  factory AndroidViewportPlan.of(
    Size physical,
    double devicePixelRatio,
    AndroidScreenPolicy mode, {
    required bool hostScale,
    DecoderLimits? portraitLimits,
    int fps = 60,
    bool fullWorkspace = false,
  }) {
    final dpr = devicePixelRatio.isFinite && devicePixelRatio > 0
        ? devicePixelRatio
        : 1.0;
    if (!physical.width.isFinite ||
        !physical.height.isFinite ||
        physical.width <= 0 ||
        physical.height <= 0) {
      throw const BrokerException('invalid_response');
    }
    // 해상도: the screen's pixels, or a preset of its shape.
    final screen = physical;
    final legacyAutomatic =
        !hostScale &&
        mode.deviceClass != AndroidDeviceClass.television &&
        mode.choice.resolution.kind == AndroidResolutionKind.auto;
    // Without a DPI grant keep the existing sharp/readable integer fraction
    // of the actual screen, while still bounding the new recommended source.
    final recommendation = legacyAutomatic
        ? mode.recommendedSize(screen, dpr)
        : null;
    physical = legacyAutomatic ? screen : mode.base(screen, dpr);
    var k = mode.pixelScale(dpr, physical, hostScale: hostScale);
    int even(double value, int by) => (value / by).floor() ~/ 2 * 2;
    // A released stream is at most 3840×2160 (8.3 MP): a screen beyond that
    // (a portrait phone taller than 2160 pixels) takes the next whole
    // fraction, still drawn sharp, rather than a fractional fit. With
    // stream-size-v1 a portrait screen keeps its own pixels while the decoder
    // takes them (1080x2400 is 1080x2400 at 1:1).
    // v2 requests the real workspace independently of encoded decoder limits.
    // Host display validation remains authoritative; native exact validation
    // applies to the eventual encoded picture, not these source coordinates.
    bool withinLimits(int w, int h) =>
        (!legacyAutomatic ||
            math.max(w, h) <= AndroidResolution.recommendedLongEdge) &&
        (fullWorkspace
            ? w >= 320 &&
                  h >= 180 &&
                  w <= 0x7fffffff &&
                  h <= 0x7fffffff &&
                  w * h <= 0x1fffffffffffff
            : portraitLimits == null
            ? w <= 3840 && h <= 2160 && w * h <= 8294400
            : w <= 3840 &&
                  h <= 3840 &&
                  w * h <= 8294400 &&
                  portraitLimits.fits(w, h, fps));
    bool fits(int w, int h) =>
        withinLimits(w, h) &&
        (!legacyAutomatic || math.max(w, h) <= recommendation!.longestSide);
    final boundedHandheld =
        mode.deviceClass != AndroidDeviceClass.television &&
        (mode.choice.resolution.kind == AndroidResolutionKind.longEdge ||
            mode.choice.resolution.kind == AndroidResolutionKind.sharp ||
            mode.choice.resolution.kind == AndroidResolutionKind.auto &&
                hostScale);
    if (mode.choice.resolution.kind == AndroidResolutionKind.workspace ||
        boundedHandheld) {
      // Relative workspaces must keep this viewport's shape. At a decoder or
      // protocol limit, fit proportionally rather than halving all the way
      // back to the device's original workspace.
      var factor = 1.0;
      if (!fits(even(physical.width, 1), even(physical.height, 1))) {
        var low = 0.0, high = 1.0;
        for (var i = 0; i < 40; i++) {
          final middle = (low + high) / 2;
          if (fits(
            even(physical.width * middle, 1),
            even(physical.height * middle, 1),
          )) {
            low = middle;
          } else {
            high = middle;
          }
        }
        factor = low;
      }
      final w = even(physical.width * factor, 1);
      final h = even(physical.height * factor, 1);
      if (w < 320 || h < 180) throw const BrokerException('invalid_response');
      return AndroidViewportPlan(
        width: w,
        height: h,
        pixelScale: w == screen.width && h == screen.height ? 1 : 0,
        displayScale: hostScale
            ? mode.windowsScale(dpr, boundedHandheld ? screen.width / w : 1)
            : null,
      );
    }
    while (k < 8 && !fits(even(physical.width, k), even(physical.height, k))) {
      if (legacyAutomatic &&
          (even(physical.width, k + 1) < 320 ||
              even(physical.height, k + 1) < 180)) {
        // The nearest valid whole fraction may exceed the soft recommendation.
        // Keep it sharp without violating protocol minima or hard limits.
        break;
      }
      k++;
    }
    if (legacyAutomatic &&
        (even(physical.width, k) < 320 ||
            even(physical.height, k) < 180 ||
            !withinLimits(even(physical.width, k), even(physical.height, k)))) {
      throw const BrokerException('invalid_response');
    }
    final exact = physical == screen;
    return AndroidViewportPlan(
      width: even(physical.width, k),
      height: even(physical.height, k),
      // A preset is drawn scaled (0: no whole pixel ratio).
      pixelScale: exact ? k : 0,
      displayScale: hostScale
          ? mode.windowsScale(
              dpr,
              exact ? k.toDouble() : screen.width / even(physical.width, k),
            )
          : null,
    );
  }

  final int width, height, pixelScale;
  final int? displayScale;
  Size get size => Size(width.toDouble(), height.toDouble());

  @override
  bool operator ==(Object other) =>
      other is AndroidViewportPlan &&
      other.width == width &&
      other.height == height &&
      other.pixelScale == pixelScale &&
      other.displayScale == displayScale;

  @override
  int get hashCode => Object.hash(width, height, pixelScale, displayScale);
}

class AndroidStreamPreparation {
  AndroidStreamPreparation({
    required this.api,
    required this.authorized,
    Future<void> Function(Duration)? wait,
    DateTime Function()? now,
    this.timeout = const Duration(seconds: 30),
    this.cleanupTimeout = SessionCleanupState.settleWindow,
    this.settleTimeout = const Duration(seconds: 15),
  }) : wait = wait ?? Future<void>.delayed,
       now = now ?? DateTime.now;

  final BrokerApi api;
  final bool Function() authorized;
  final Future<void> Function(Duration) wait;
  final DateTime Function() now;
  final Duration timeout;

  /// An earlier stream's broker cleanup barrier (this device, another window
  /// or another PC): observed read-only, as the Windows client does.
  final Duration cleanupTimeout;

  /// A count-capable host still reporting its fresh display state.
  final Duration settleTimeout;

  void _check() {
    if (!authorized()) {
      throw const BrokerException('session_expired', 401);
    }
  }

  Duration _remaining(
    DateTime deadline, [
    String code = 'display_operation_timeout',
  ]) {
    _check();
    final remaining = deadline.difference(now());
    if (remaining <= Duration.zero) {
      throw BrokerException(
        code,
        code == 'display_operation_timeout' ? 0 : 409,
      );
    }
    return remaining;
  }

  Future<T> _read<T>(
    Future<T> Function() request,
    DateTime deadline, [
    String code = 'display_operation_timeout',
  ]) async {
    final remaining = _remaining(deadline, code);
    final result = await request().timeout(
      remaining,
      onTimeout: () => throw BrokerException(
        code,
        code == 'display_operation_timeout' ? 0 : 409,
      ),
    );
    _check();
    return result;
  }

  // A reservation is a mutation: never replay it, and release its exact grant
  // if it completes after this owner or bounded wait was retired.
  Future<BrokerConnection> _reserve(
    Future<BrokerConnection> Function() request,
    DateTime deadline,
  ) async {
    final remaining = _remaining(deadline);
    var abandoned = false, handedOff = false, released = false;
    void release(BrokerConnection connection) {
      if (released) return;
      released = true;
      unawaited(connection.release().catchError((Object _) {}));
    }

    final pending = request();
    unawaited(
      pending.then<void>((connection) {
        if (abandoned) release(connection);
      }, onError: (Object _) {}),
    );
    try {
      final connection = await pending.timeout(
        remaining,
        onTimeout: () =>
            throw const BrokerException('display_operation_timeout'),
      );
      try {
        _check();
      } catch (_) {
        release(connection);
        rethrow;
      }
      handedOff = true;
      return connection;
    } finally {
      abandoned = !handedOff;
    }
  }

  Future<BrokerDesktop> _desktop(
    String token,
    String id,
    DateTime deadline, [
    String code = 'display_operation_timeout',
  ]) async {
    final inventory = await _read(() => api.desktops(token), deadline, code);
    _remaining(deadline, code);
    final matches = inventory.where((desktop) => desktop.id == id).toList();
    if (matches.length != 1) {
      throw const BrokerException('desktop_not_found', 404);
    }
    return matches.single;
  }

  /// Observe an existing controller before any output-count or console change.
  /// This never releases it. A negotiated busy response keeps its offer for
  /// the remote view's card, whose button is the one explicit confirmation.
  Future<void> checkTakeover(String token, String desktopId) async {
    _check();
    final broker = api;
    if (broker is! StreamTakeoverPreflightBrokerApi) return;
    await _read(
      () => (broker as StreamTakeoverPreflightBrokerApi).checkStreamTakeover(
        token,
        desktopId,
      ),
      now().add(timeout),
    );
  }

  bool _oneReady(BrokerDesktop desktop) =>
      !desktop.sessionCleanup.pending &&
      desktop.hostConnected == true &&
      desktop.canStream &&
      !desktop.display.busy &&
      !desktop.display.stale &&
      desktop.display.checkedAt != null &&
      desktop.display.activeCount == 1 &&
      desktop.monitors.length == 1 &&
      desktop.monitors.single.id == '0';

  /// The host is up and count-capable but its display report is not fresh yet
  /// (just after a previous session or a Host restart).
  bool _settling(BrokerDesktop desktop) =>
      desktop.hostConnected == true &&
      desktop.virtualMonitorControl &&
      desktop.display.supported &&
      !desktop.powerBusy &&
      !_oneReady(desktop) &&
      (desktop.display.busy ||
          desktop.display.stale ||
          desktop.display.checkedAt == null ||
          desktop.streamPreparation == StreamPreparation.maintenance ||
          desktop.streamPreparation == StreamPreparation.checking);

  Future<BrokerDesktop> _observe(
    String token,
    String id,
    BrokerDesktop desktop,
    bool Function(BrokerDesktop) waiting,
    Duration limit,
    String code,
  ) async {
    final until = now().add(limit);
    while (waiting(desktop)) {
      final remaining = _remaining(until, code);
      await wait(
        remaining < const Duration(milliseconds: 500)
            ? remaining
            : const Duration(milliseconds: 500),
      );
      desktop = await _desktop(token, id, until, code);
    }
    return desktop;
  }

  /// [displayScale]: the Windows display scale to ask for where this
  /// desktop's Host and broker support display-scale-v1.
  Future<BrokerConnection> connect(
    String token,
    String desktopId,
    StreamOptions options, {
    int? displayScale,
  }) async {
    if (!['0', 'all'].contains(options.monitor)) {
      throw const BrokerException('invalid_response');
    }
    options.toJson();
    var desktop = await _desktop(token, desktopId, now().add(timeout));
    if (desktop.sessionCleanup.pending) {
      desktop = await _observe(
        token,
        desktopId,
        desktop,
        (value) => value.sessionCleanup.pending,
        cleanupTimeout,
        'session_cleanup_pending',
      );
    }
    await checkTakeover(token, desktopId);
    if (options.monitor == 'all') {
      final broker = api;
      if (!desktop.profileTracks || broker is! ProfileTrackBrokerApi) {
        throw const BrokerException('track_streaming_unavailable');
      }
      final plan = TrackPlan([
        TrackViewport('0', options.maxWidth, options.maxHeight, 0, 0),
        TrackViewport(
          '1',
          options.maxWidth,
          options.maxHeight,
          options.maxWidth,
          0,
        ),
      ]);
      return _reserve(
        () => (broker as ProfileTrackBrokerApi).connectProfileTracks(
          token,
          desktopId,
          plan,
          options,
          displayScale: desktop.displayScaleOutputs ? displayScale : null,
        ),
        now().add(timeout),
      );
    }
    if (_settling(desktop)) {
      desktop = await _observe(
        token,
        desktopId,
        desktop,
        _settling,
        settleTimeout,
        'host_maintenance',
      );
    }
    final deadline = now().add(timeout);
    if (!_oneReady(desktop)) {
      if (desktop.powerBusy) {
        throw const BrokerException('power_in_progress');
      }
      if (desktop.display.busy) {
        throw const BrokerException('display_busy');
      }
      if (desktop.hostConnected != true ||
          !desktop.virtualMonitorControl ||
          !desktop.display.supported ||
          desktop.display.stale) {
        throw const BrokerException('display_not_supported');
      }
      var state = await _read(
        () => api.configureDisplays(token, desktopId, 1),
        deadline,
      );
      final operationId = state.operation?.id;
      if (operationId == null) {
        throw const BrokerException('invalid_response');
      }
      while (true) {
        if (state.operation?.id != operationId ||
            state.operation?.requestedCount != 1) {
          throw const BrokerException('invalid_response');
        }
        if (state.operation?.status == 'failed') {
          throw const BrokerException('display_operation_failed');
        }
        if (state.operation?.status == 'unknown') {
          throw const BrokerException('display_operation_unknown');
        }
        if (state.applied(1, operationId)) {
          desktop = await _desktop(token, desktopId, deadline);
          if (_oneReady(desktop) && desktop.display.applied(1, operationId)) {
            break;
          }
        }
        final remaining = _remaining(deadline);
        await wait(
          remaining < const Duration(milliseconds: 500)
              ? remaining
              : const Duration(milliseconds: 500),
        );
        state = await _read(
          () => api.display(token, desktopId, operationId: operationId),
          deadline,
        );
      }
    }
    // No capture/relay ticket is requested until the fresh owned-output result
    // is ready. Revocation or closing this route also prevents this call.
    final broker = api;
    final viewport =
        desktop.initialViewport && broker is InitialViewportBrokerApi
        ? InitialViewport(
            width: options.maxWidth,
            height: options.maxHeight,
            fps: options.fps,
            preserveEncoderLimits: desktop.viewportEncoderLimits,
          )
        : null;
    // The encoder box stays this session's ceiling for every later resize
    // (stream.resize carries only the size). A Fold connected folded must
    // still get its whole unfolded picture (otherwise, unfolded, it receives
    // 1248×942 of a 2448×1848 output, drawn 1:1 in a quarter of the screen).
    // Where the Host keeps the box apart from the first viewport, ask for the
    // codec ceiling, as the Windows client does.
    if (viewport != null && viewport.preserveEncoderLimits) {
      options = StreamOptions(
        fps: options.fps,
        bitrate: options.bitrate,
        audio: options.audio,
        monitor: options.monitor,
        bitrateMode: options.bitrateMode,
      );
    }
    if (displayScale != null &&
        desktop.displayScale &&
        broker is DisplayScaleBrokerApi) {
      return _reserve(
        () => (broker as DisplayScaleBrokerApi).connectScaled(
          token,
          desktopId,
          options: options,
          initialViewport: viewport,
          displayScale: displayScale,
        ),
        deadline,
      );
    }
    if (viewport != null) {
      // The host prepares this device's actual pixel size before its first
      // frame instead of streaming the old size and resizing right after.
      return _reserve(
        () => (broker as InitialViewportBrokerApi).connectViewport(
          token,
          desktopId,
          viewport,
          options,
        ),
        deadline,
      );
    }
    return _reserve(
      () => api.connect(token, desktopId, options: options),
      deadline,
    );
  }

  /// desktop_busy with the broker's takeover permission: ends the other
  /// connection only; the caller then connects through [connect], which
  /// waits for that session's cleanup.
  Future<void> takeover(
    String token,
    String desktopId, {
    StreamTakeoverOffer? offer,
  }) async {
    _check();
    if (offer == null || offer.desktopId != desktopId) {
      throw const BrokerException('invalid_takeover_context');
    }
    if (!offer.currentAt(now())) {
      throw const BrokerException('takeover_context_expired', 410);
    }
    final broker = api;
    if (broker is! ConfirmedTakeoverBrokerApi) {
      throw const BrokerException('desktop_busy');
    }
    final released = await (broker as ConfirmedTakeoverBrokerApi)
        .confirmTakeover(token, desktopId, offer)
        .timeout(
          const Duration(seconds: 15),
          onTimeout: () => throw const BrokerException('network_error'),
        );
    _check();
    if (!released) {
      throw const BrokerException('desktop_busy', 409, null, false);
    }
  }
}
