import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'broker_api.dart';
import 'session_activity.dart';
import 'android_watchdog_observation.dart';
import 'audio_codec.dart';
import 'client_diagnostic_codes.dart';
import 'connect_timing.dart';
import 'input_latency.dart';
import 'remote_cursor.dart';
import 'relay_inbox.dart';
import 'notices.dart' show noticeLiveFeature;
import 'relay_socket.dart';
import 'channel_redial.dart';
import 'clipboard_session.dart';
import 'decode_settings.dart';
import 'display_scale.dart';
import 'print_session.dart';
import 'drive_session.dart';
import 'remote_input_gate.dart';
import 'session_failure.dart';
import 'session_input_observation.dart';
import 'session_resume.dart';
import 'product_log.dart';
import 'stream_size.dart';
import 'stream_capacity.dart';
import 'stream_stall.dart';
import 'stream_stats_report.dart';
import 'audio_stats.dart';
import 'stream_observability.dart';
import 'client_build_version.dart';
import 'stream_telemetry.dart';
import 'touch_input.dart';
import 'text_input.dart';
import 'caret_follow.dart';
import 'track_stream.dart';
import 'udp_probe.dart';
import 'usb_redirection.dart';
import '../l10n/l10n.dart';

abstract interface class RemoteMedia {
  Future<int> start(int width, int height);
  Future<void> push(Uint8List packet);
  Future<Map<String, dynamic>> stats();
  Future<bool> capture(bool enabled);
  Future<void> stop();
  Stream<Map<String, dynamic>> get input;
}

abstract interface class RemoteTrackMedia implements RemoteMedia {
  Future<Map<String, int>> startTracks(Map<String, dynamic> configuration);
}

/// Optional local thumbnail revocation. It grants no capture/desktop capability.
abstract interface class RemoteThumbnailPrivacy {
  Future<void> clearThumbnail();
}

/// ToTBSeamless lossless tiles (tiles-v1): a media engine that composites the
/// host's lossless tiles over decoded video. Its session opts in by prefixing
/// RTT ping nonces, the only client field relayed unchanged to the host on
/// both legacy and track channels; hosts without tiles simply echo the pong.
abstract interface class LosslessTileMedia {
  bool get losslessTiles;
}

const losslessTilesPingPrefix = 'lt1-';

/// The same session asks for no tiles now (게임 마우스 holds the mouse).
/// Hosts without it read a plain ping and keep their tiles.
const losslessTilesPausePrefix = 'lt0-';

/// input-latency-v1: a media engine that reports (in its stats `inputFrames`,
/// as [pts, queued -> handed-to-texture microseconds]) when the first decoded
/// frame at or after a watched video packet was handed to its texture.
abstract interface class InputFrameWatchMedia {
  Future<void> pushWatched(Uint8List packet);
}

/// stream-size-v1: a media engine that restarts its decoder at a new picture
/// size for the same session without releasing input capture (the Host's
/// picture change of the same source).
abstract interface class PictureChangeMedia {
  Future<int> restart(int width, int height);
}

/// A media engine that keeps the previous picture on screen across a
/// viewport resize (tuning backlog #1: the window went black for 0.5-2 s):
/// [startHolding] starts the decoder for the new size while the previous
/// texture keeps its last frame until [releaseHeld].
abstract interface class HeldPictureMedia {
  bool get holdsPicture;
  Future<int> startHolding(int width, int height);
  Future<void> releaseHeld(int texture);
}

/// viewportResult: the Host's stages from a resize's stream.info
/// (resize_stages.hpp), under their diagnostic names.
const _hostResizeStages = {
  'suspendMs': 'hostSuspendMs',
  'displayMs': 'hostDisplayMs',
  'sdkMs': 'hostSdkMs',
  'settleMs': 'hostSettleMs',
  'confirmMs': 'hostConfirmMs',
  'startMs': 'hostRestartMs',
  'captureMs': 'hostCaptureStartMs',
  'encoderMs': 'hostEncoderStartMs',
  'audioMs': 'hostAudioStartMs',
};
Map<String, int> hostResizeStages(Object? raw) => raw is! Map
    ? const {}
    : {
        for (final stage in _hostResizeStages.entries)
          if (raw[stage.key] case final int value
              when value >= 0 && value <= 600000)
            stage.value: value,
      };

/// decoder-prestart-v1: a media engine that starts its decoder(s) while the
/// session still waits for its stream info. The Windows CPU decoder's start
/// took 0.1-1.8 s on a loaded VM, at any size, after the stream info. A hint
/// only: the native side reads the actual size from the stream.
abstract interface class DecoderPrestartMedia {
  Future<void> prepareDecoder(int width, int height, int fps, int outputs);
}

/// udp-media-v1: a media engine whose WebSocket packets follow the path rule
/// once the keys are done (windows/runner/udp_path_gate.h).
abstract interface class UdpPathMedia {
  set udpPathRule(bool value);
}

/// cursor-v1: a media engine that shows the host's separated cursor as the
/// real local cursor over the picture. `record` is the payload of one binary
/// media kind-3 message of media `epoch`; `cursorView` reports where the
/// picture is (logical pixels in the view) and the remote source size.
abstract interface class RemoteCursorMedia {
  Future<void> cursor(int epoch, Uint8List record);
  Future<void> cursorView(CursorView view);
}

class NativeRemoteMedia
    implements
        RemoteTrackMedia,
        LosslessTileMedia,
        InputFrameWatchMedia,
        RemoteCursorMedia,
        RemoteThumbnailPrivacy,
        PictureChangeMedia,
        HeldPictureMedia,
        DecoderPrestartMedia,
        UdpPathMedia,
        AudioFormatMedia {
  NativeRemoteMedia({
    this.audioEnabled = true,
    this.losslessTiles = false,
    this.hardwareDecode = true,
    this.thumbnailAllowed = true,
    this.textInputAllowed = false,
  });
  final bool audioEnabled;

  /// Immutable ordinary-session grant. Missing/false keeps native capture's
  /// released key-only shape, including Android and independent outputs.
  final bool textInputAllowed;

  /// False is an irreversible native-window privacy latch, not a per-decoder
  /// hint. Ordinary media omit the field and retain their existing behavior.
  final bool thumbnailAllowed;

  /// audio-opus-v1: the runner decodes the audio packets as Opus; the released
  /// PCM start arguments otherwise.
  @override
  bool audioOpus = false;
  Map<String, Object> get _audioFormat =>
      audioOpus ? const {'audioFormat': 'opus'} : const {};

  /// The user's "하드웨어 디코딩" choice at the time the stream starts (see
  /// DecodeSettings); the runner falls back to the CPU on its own when this PC
  /// or the stream cannot use it.
  final bool hardwareDecode;
  Map<String, Object>? _videoContext;
  void videoConfiguration(
    int sourceWidth,
    int sourceHeight,
    int fps,
    bool streamSizeV2,
  ) {
    _videoContext = {
      'sourceWidth': sourceWidth,
      'sourceHeight': sourceHeight,
      'fps': fps,
      'streamSizeV2': streamSizeV2,
    };
  }

  @override
  final bool losslessTiles;
  @override
  bool udpPathRule = false;
  int _generation = 0;
  static int _nextLease = 0, _activeLease = 0;
  int _lease = 0;
  static const _channel = MethodChannel('multitap/remote');
  static const _events = EventChannel('multitap/remote/input');
  static final _inputEvents = _events.receiveBroadcastStream().map(
    (value) => Map<String, dynamic>.from(value as Map),
  );
  @override
  Future<int> start(int width, int height) => _begin(
    'start',
    {
      'audioEnabled': audioEnabled,
      'width': width,
      'height': height,
      'hardwareDecode': hardwareDecode,
      ...?_videoContext,
      ..._audioFormat,
    },
    (value) {
      if (value is! int || value < 0) {
        throw PlatformException(code: 'invalid_media_reply');
      }
      return value;
    },
  );

  @override
  Future<int> restart(int width, int height) => _begin(
    'start',
    {
      'audioEnabled': audioEnabled,
      'width': width,
      'height': height,
      'keepCapture': true,
      'hardwareDecode': hardwareDecode,
      ...?_videoContext,
      ..._audioFormat,
    },
    (value) {
      if (value is! int || value < 0) {
        throw PlatformException(code: 'invalid_media_reply');
      }
      return value;
    },
  );

  /// The Windows runner keeps the held texture (RemoteMediaSession::Start
  /// `hold`); other platforms' engines stop the previous one as before.
  @override
  bool get holdsPicture => defaultTargetPlatform == TargetPlatform.windows;

  @override
  Future<int> startHolding(int width, int height) => _begin(
    'start',
    {
      'audioEnabled': audioEnabled,
      'width': width,
      'height': height,
      'holdPrevious': true,
      'hardwareDecode': hardwareDecode,
      ...?_videoContext,
      ..._audioFormat,
    },
    (value) {
      if (value is! int || value < 0) {
        throw PlatformException(code: 'invalid_media_reply');
      }
      return value;
    },
  );

  @override
  Future<void> prepareDecoder(
    int width,
    int height,
    int fps,
    int outputs,
  ) async {
    // Android's own native bridge has no prestart.
    if (defaultTargetPlatform != TargetPlatform.windows) return;
    await _channel.invokeMethod<void>('prepareDecoder', {
      'width': width,
      'height': height,
      'fps': fps,
      'outputs': outputs,
    });
  }

  @override
  Future<void> releaseHeld(int texture) async {
    try {
      await _channel.invokeMethod<bool>('releaseHeld', {
        'lease': _lease < 1 ? 1 : _lease,
        'texture': texture,
      });
    } catch (_) {
      // A later start, stop or window close releases it natively anyway.
    }
  }

  /// Candidate independent output engine. Connection negotiation and separate
  /// local windows must opt in explicitly; this never changes legacy sessions.
  @override
  Future<Map<String, int>> startTracks(Map<String, dynamic> configuration) =>
      _begin(
        'startTracks',
        {
          'configuration': configuration,
          'hardwareDecode': hardwareDecode,
          if (_videoContext != null) 'fps': _videoContext!['fps'],
          if (_videoContext != null)
            'streamSizeV2': _videoContext!['streamSizeV2'],
        },
        (value) {
          if (value is! Map || value.isEmpty || value.length > 2) {
            throw PlatformException(code: 'invalid_media_reply');
          }
          final textures = <String, int>{};
          for (final entry in value.entries) {
            if (!['0', '1'].contains(entry.key) ||
                entry.value is! int ||
                (entry.value as int) < 0) {
              throw PlatformException(code: 'invalid_media_reply');
            }
            textures[entry.key as String] = entry.value as int;
          }
          if (textures.values.toSet().length != textures.length) {
            throw PlatformException(code: 'invalid_media_reply');
          }
          return Map.unmodifiable(textures);
        },
      );

  Future<T> _begin<T>(
    String method,
    Map<String, dynamic> arguments,
    T Function(Object?) decode,
  ) async {
    final generation = ++_generation;
    final previousLease = _lease;
    final lease = _lease = ++_nextLease;
    _activeLease = lease;
    try {
      final response = await _channel.invokeMethod<Object?>(method, {
        'lease': lease,
        if (!thumbnailAllowed) 'thumbnailAllowed': false,
        ...arguments,
      });
      final decoded = decode(response);
      if (_activeLease != lease || generation != _generation) {
        throw PlatformException(code: 'session_replaced');
      }
      if (!thumbnailAllowed) {
        // Older runners can ignore an unknown start argument. A durable revoke
        // plus the new lease-bound latch acknowledgement is mandatory; an
        // echoed argument or a texture ID alone never proves this barrier.
        await _revokeThumbnailLease(lease);
        if (_activeLease != lease || generation != _generation) {
          throw PlatformException(code: 'session_replaced');
        }
        final bool? locked;
        try {
          locked = await _channel
              .invokeMethod<bool>('thumbnailPrivacyLocked', {'lease': lease})
              .timeout(const Duration(seconds: 2));
        } on MissingPluginException {
          throw PlatformException(code: 'thumbnail_privacy_unknown');
        } on TimeoutException {
          throw PlatformException(code: 'thumbnail_privacy_unknown');
        }
        if (_activeLease != lease || generation != _generation) {
          throw PlatformException(code: 'session_replaced');
        }
        if (locked != true) {
          throw PlatformException(code: 'thumbnail_privacy_unknown');
        }
      }
      return decoded;
    } catch (_) {
      if (_activeLease == lease && generation == _generation) _activeLease = 0;
      if (_lease == lease && generation == _generation) _lease = 0;
      // Native cleanup carries the retired owner too. A later connection can
      // start before this reaches Windows without being stopped by it.
      Future<void> retire() async {
        // Admission may fail before native replaced the previous engine. Both
        // exact leases belong to this wrapper; neither can retire a newer L3.
        for (final retired in <int>{
          lease,
          if (previousLease > 0) previousLease,
        }) {
          if (defaultTargetPlatform == TargetPlatform.windows) {
            try {
              await _revokeThumbnailLease(retired);
            } catch (_) {}
          }
          try {
            await _channel
                .invokeMethod<void>('stop', {'lease': retired})
                .timeout(const Duration(seconds: 2));
          } catch (_) {}
        }
      }

      unawaited(retire().catchError((Object _) {}));
      rethrow;
    }
  }

  @override
  Future<void> push(Uint8List packet) => _push(packet, false);

  @override
  Future<void> pushWatched(Uint8List packet) => _push(packet, true);

  Future<void> _push(Uint8List packet, bool watch) async {
    final lease = _lease, generation = _generation;
    if (lease == 0 || _activeLease != lease) {
      throw PlatformException(code: 'session_replaced');
    }
    await sendPacket(lease, packet, watch);
    if (_activeLease != lease || generation != _generation) {
      throw PlatformException(code: 'session_replaced');
    }
  }

  /// Hands one media packet of [lease] to the native engine; the lease checks
  /// around it stay in [NativeRemoteMedia]. Engines with their own packet
  /// channel (Android: AndroidRemoteMedia) override this.
  @protected
  Future<void> sendPacket(
    int lease,
    Uint8List packet,
    bool watch,
  ) => _channel.invokeMethod<void>('push', {
    'lease': lease,
    'bytes': packet,
    // Only a watched packet carries the field; the released call is unchanged.
    if (watch) 'watch': true,
    if (udpPathRule) 'path': true,
  });

  @override
  Future<void> cursor(int epoch, Uint8List record) =>
      _leased('cursor', {'epoch': epoch, 'bytes': record});

  @override
  Future<void> cursorView(CursorView view) =>
      _leased('cursorView', view.toJson());

  Future<void> _leased(String method, Map<String, Object> arguments) async {
    final lease = _lease, generation = _generation;
    if (lease == 0 || _activeLease != lease) {
      throw PlatformException(code: 'session_replaced');
    }
    await _channel.invokeMethod<void>(method, {'lease': lease, ...arguments});
    if (_activeLease != lease || generation != _generation) {
      throw PlatformException(code: 'session_replaced');
    }
  }

  @override
  Future<Map<String, dynamic>> stats() async {
    final generation = _generation, lease = _lease;
    if (lease == 0 || _activeLease != lease) {
      throw PlatformException(code: 'session_replaced');
    }
    final result = await _channel.invokeMapMethod<String, dynamic>('stats', {
      'lease': lease,
    });
    if (_activeLease != lease || generation != _generation) {
      throw PlatformException(code: 'session_replaced');
    }
    return Map<String, dynamic>.from(result!);
  }

  @override
  Future<bool> capture(bool enabled) async {
    if (_lease == 0 || _activeLease != _lease) return false;
    final generation = _generation, lease = _lease;
    try {
      final accepted = await _channel.invokeMethod<bool>('capture', {
        'lease': lease,
        'enabled': enabled,
        if (enabled && textInputAllowed) 'textInput': true,
      });
      return _activeLease == lease &&
          generation == _generation &&
          accepted == true;
    } on PlatformException catch (error) {
      if (_activeLease != lease ||
          generation != _generation ||
          error.code == 'session_replaced') {
        return false;
      }
      rethrow;
    }
  }

  @override
  Future<void> clearThumbnail() async {
    // Android's separately owned native bridge has no matching contract yet.
    if (defaultTargetPlatform != TargetPlatform.windows) return;
    final lease = _lease, generation = _generation;
    if (lease == 0 || _activeLease != lease) return;
    await _revokeThumbnailLease(lease);
    if (_activeLease != lease || generation != _generation) {
      throw PlatformException(code: 'session_replaced');
    }
  }

  Future<void> _revokeThumbnailLease(int lease) async {
    final elapsed = Stopwatch()..start();
    const budget = Duration(seconds: 2);
    Duration remaining() {
      final value = budget - elapsed.elapsed;
      if (value <= Duration.zero) {
        throw PlatformException(code: 'thumbnail_privacy_unknown');
      }
      return value;
    }

    try {
      final generation = await _channel
          .invokeMethod<int>('thumbnailPrivacy', {'lease': lease})
          .timeout(remaining());
      if (generation == 0) return; // This exact native owner has no target.
      if (generation == null || generation < 0) {
        throw PlatformException(code: 'thumbnail_privacy_unknown');
      }
      while (true) {
        final durable = await _channel
            .invokeMethod<bool>('thumbnailPrivacyStatus', {
              'lease': lease,
              'generation': generation,
            })
            .timeout(remaining());
        if (durable == true) return;
        final wait = remaining();
        await Future<void>.delayed(
          wait < const Duration(milliseconds: 20)
              ? wait
              : const Duration(milliseconds: 20),
        );
      }
    } on TimeoutException {
      throw PlatformException(code: 'thumbnail_privacy_unknown');
    } on MissingPluginException {
      throw PlatformException(code: 'thumbnail_privacy_unknown');
    } catch (_) {
      throw PlatformException(code: 'thumbnail_privacy_unknown');
    }
  }

  @override
  Future<void> stop() async {
    // A late close of the old dialog must not stop a newly opened desktop.
    if (_lease == 0 || _activeLease != _lease) return;
    final lease = _lease;
    _generation++;
    _activeLease = 0;
    _lease = 0;
    await _channel.invokeMethod<void>('stop', {'lease': lease});
  }

  @override
  Stream<Map<String, dynamic>> get input => _inputEvents
      .where(
        (event) =>
            _lease > 0 && _activeLease == _lease && event['lease'] == _lease,
      )
      .map((event) => Map<String, dynamic>.from(event)..remove('lease'));
}

typedef SessionDialer = Future<WebSocket> Function(Uri, String);

/// session-resume-v1: dials the session URL again with its resume token.
typedef SessionResumeDialer = Future<WebSocket> Function(
  Uri url,
  String token,
  String resumeToken,
);
typedef ClipboardSessionFactory = ClipboardSession Function(
  BrokerConnection,
  String,
);

class _ViewportChange {
  _ViewportChange(this.requestId, this.width, this.height, this.deadlineUs);
  final String requestId;
  final int width, height;
  final done = Completer<bool>();
  int deadlineUs;
  int? epoch;
  String? resizeId;
  bool decoderStarted = false;
  DecoderLimits? decoderLimits;

  /// The previous picture kept on screen during this change (HeldPictureMedia).
  int? heldTexture;

  // viewportResult stage timing (session clock, microseconds) and the Host's
  // stages from the new size's stream.info.
  int beganUs = 0, dispatchedUs = 0;
  int? ackUs, infoUs, decoderUs, frameUs;
  Map<String, int> hostStages = const {};
  Map<String, int> stageValues() {
    int ms(int from, int to) => ((to - from) / 1000).round().clamp(0, 600000);
    return {
      'probeMs': ms(beganUs, dispatchedUs),
      if (ackUs case final ack?) 'ackMs': ms(dispatchedUs, ack),
      if (infoUs case final info?) 'infoMs': ms(dispatchedUs, info),
      if ((infoUs, decoderUs) case (final info?, final decoder?))
        'decoderMs': ms(info, decoder),
      if ((decoderUs, frameUs) case (final decoder?, final frame?))
        'frameMs': ms(decoder, frame),
      if (frameUs case final frame?) 'elapsedMs': ms(dispatchedUs, frame),
      ...hostStages,
    };
  }
}

// channel-resume-v1: this client dials its side channels again after a resume
// (a released broker ignores the header; channel_redial.dart).
// notice-v1 (notices.dart): this socket also hears the broker's `notices`.
// Headers, so a released broker ignores them.
const _dialHeaders = {
  channelResumeHeader: channelResumeProtocol,
  'x-totb-live': noticeLiveFeature,
};
Future<WebSocket> _dial(Uri uri, String token) =>
    connectRelaySocket(uri, token, headers: _dialHeaders);
Future<WebSocket> _dialResume(Uri uri, String token, String resumeToken) =>
    connectRelaySocket(
      uri,
      token,
      headers: {..._dialHeaders, sessionResumeHeader: resumeToken},
    );

/// Creates the print-v1 session of a connection the broker granted it
/// ([BrokerConnection.printWebSocketUrl]); see print_session.dart.
/// [folderName]: null for the session's first channel, its own folder name for
/// one that replaces an ended channel (channel-resume-v1).
typedef PrintSessionFactory = PrintSession Function(
  BrokerConnection connection,
  String token,
  String? folderName,
);

/// Creates the drive-v1 session of a connection the broker granted it
/// ([BrokerConnection.driveWebSocketUrl]); see drive_session.dart.
typedef DriveSessionFactory = DriveSession Function(
  BrokerConnection connection,
  String token,
);

/// Creates the usb-v1 session of a connection the broker granted it
/// ([BrokerConnection.usbWebSocketUrl]); see usb_redirection.dart.
typedef UsbSessionFactory = UsbSession Function(
  BrokerConnection connection,
  String token,
);

/// Builds the leg of a udp-media-v1 grant (tests); null: the native leg.
typedef UdpLegFactory = UdpLeg Function();

class RemoteSession extends ChangeNotifier {
  /// A display change the relay refused before anything changed: the stream
  /// goes on at its current size.
  static String get keptSizeMessage => l10n.couldntChangeScreenSizeSo;

  /// notice-v1 (notices.dart): the broker said on this session's control
  /// socket that the account has notices to read. Contentless; the window's
  /// NoticeCenter reads them. Nothing else in the session depends on it.
  void Function()? onBrokerNotices;

  RemoteSession({
    required this.connection,
    required this._token,
    RemoteMedia? media,
    SessionDialer? dialer,
    SessionResumeDialer? resumeDialer,
    Stopwatch? clock,
    this.statsSink,
    this.stallSink,
    this.mediaQueueWait = const Duration(seconds: 3),
    this.printDialer,
    this.printFactory,
    this.clipboardFactory,
    this.driveFactory,
    this.usbFactory,
    this.udpLegFactory,
    this.capacity,
    bool? observe,
  }) : _media =
           media ??
           // The Windows runner composites lossless tiles (lossless_tiles.hpp).
           NativeRemoteMedia(
             audioEnabled: connection.options.audio,
             losslessTiles: true,
             hardwareDecode:
                 connection.hardwareDecode ?? DecodeSettings.hardwareDecode,
             thumbnailAllowed: connection.signin == null,
             textInputAllowed:
                 connection.textInput &&
                 connection.trackPlan == null &&
                 connection.signin == null,
           ),
       _dialer = dialer ?? _dial,
       _resumeDialer = resumeDialer ?? _dialResume,
       _clock = clock ?? (Stopwatch()..start()),
       _observabilityEnabled = observe ?? StreamObservability.enabled {
    if (connection.signin != null &&
        (!connection.epochMedia ||
            connection.trackPlan != null ||
            connection.options.monitor != '0')) {
      throw const FormatException('Unsupported sign-in stream output.');
    }
    if (connection.signin != null &&
        _media is NativeRemoteMedia &&
        (_media).thumbnailAllowed) {
      throw const FormatException('Native sign-in privacy latch is required.');
    }
  }
  final BrokerConnection connection;
  late final SigninStreamState? _signin = connection.signin == null
      ? null
      : SigninStreamState(
          sessionId: connection.sessionId,
          initial: connection.signin!,
        );
  bool _signinPrivacyReady = false;
  SigninRedirectOwner? _signinRedirectOwner;
  bool get _userPhase => _signin == null || _signin.redirectsAllowed;
  SigninPhase? get signinPhase => _signin?.binding.phase;
  int? get signinRevision => _signin?.revision;
  bool get signinDesktopReady =>
      _signin != null &&
      _signin.active &&
      _signin.binding.phase == SigninPhase.desktop &&
      frameReady &&
      !_closed;
  final StreamAdmissionQuery? capacity;
  late DecoderLimits? _activeDecoderLimits = connection.streamSize;
  bool _capacityPending = false;
  Future<DecoderLimits> _probeCapacity(
    List<StreamSourceSize> sources,
    int fps,
  ) async =>
      (await (capacity != null
              ? capacity!(sources, fps)
              : nativeStreamAdmission(
                  sources,
                  fps,
                  hardware: _media is NativeRemoteMedia
                      ? (_media).hardwareDecode
                      : null,
                )))
          .limits;

  /// decoder-prestart-v1: the next stream info's decoder(s) start now. A
  /// failure only leaves the decoder start where it was.
  void _prepareDecoder(int width, int height, int fps, int outputs) {
    if (_media case final DecoderPrestartMedia media) {
      unawaited(
        media
            .prepareDecoder(width, height, fps, outputs)
            .catchError((Object _) {}),
      );
    }
  }

  void _videoConfiguration(int sw, int sh, int fps) {
    if (_media is NativeRemoteMedia) {
      (_media).videoConfiguration(sw, sh, fps, connection.streamSizeV2);
    }
  }

  final String _token;

  /// connect-timing-v1: this connection's stages, added to its first
  /// streamReady record (the window and the view fill [ConnectTiming.before]).
  final connectTiming = ConnectTiming();

  /// Periodic statistics summary for automatic diagnostics (optional).
  final StreamStatsSink? statsSink;

  /// stream-stall-v1 events (stream_stall.dart), optional like [statsSink].
  final StreamStatsSink? stallSink;
  final _stallWatch = StallWatch();
  int? hostFpsTarget;
  bool _observabilityEnabled;
  bool get observabilityEnabled => _observabilityEnabled;
  final observationReport = StreamObservationReport();
  RelayInboxProbe? _inboxProbe;
  Map<String, int> _hostObservation = const {};
  int? _hostObservationAtUs;
  int? _hostObservationEpoch;
  final _nativeObservation = <String, Object?>{};
  final _lastEncoderFacts = <String, Object?>{};
  bool? _observedHardwareDecoder;
  final _trackHostObservation = <String, Map<String, Object>>{};
  final _trackDecoderObservation = <String, Map<String, Object>>{};
  int _gateWaitCount = 0, _gateWaitMaxUs = 0;

  /// Collection changes locally now; Host negotiation changes next connection.
  void setObservabilityEnabled(bool enabled) {
    unawaited(StreamObservability.choose(enabled));
    _observabilityEnabled = enabled;
    if (!enabled) _inboxProbe?.disable();
    if (!_closed) notifyListeners();
  }

  Map<String, Object?> _observationValues(int nowUs) {
    int? us(double? value) => value != null && value.isFinite && value >= 0
        ? (value * 1000).round()
        : null;
    int? rate(double? value) => value != null && value.isFinite && value >= 0
        ? (value * 10).round()
        : null;
    final probe = _inboxProbe;
    final stable =
        frameReady &&
        !resizing &&
        nowUs >= _lastStatsUs &&
        nowUs - _lastStatsUs <= 3000000;
    final pipeline = stable && hostTelemetryFresh
        ? hostTelemetry?.pipeline
        : null;
    return {
      if (_hostObservationAtUs != null &&
          nowUs >= _hostObservationAtUs! &&
          nowUs - _hostObservationAtUs! <= 3000000 &&
          !resizing &&
          _hostObservationEpoch == (_tracks?.topology ?? _epoch))
        ..._hostObservation,
      if (probe != null && probe.enabled) ...{
        'clientInboxPackets': probe.packets,
        'clientInboxBytes': probe.bytes,
        'clientInboxOldestUs': probe.oldestUs,
      },
      'inputGateQueuePackets': _inputGate.queuedPackets,
      'inputGateWaitUs': _inputGate.preparingPaste
          ? _inputGate.observedWaitUs
          : 0,
      'inputGateWaitMaxUs': _gateWaitMaxUs,
      'inputGateWaitCount': _gateWaitCount,
      if (stable) ..._nativeObservation,
      if (stable) ...{
        'decodeUs': us(clientDecodeMs),
        'textureWaitUs': us(textureWaitMs),
        'textureUploadUs': us(textureUploadMs),
        'captureFps10': rate(capturedFps),
        'encodeFps10': rate(encodedFps),
        'decodeFps10': rate(decodedFps),
        'textureFps10': rate(textureFps),
        'bitrateKbps': appliedBitrate == null ? null : appliedBitrate! ~/ 1000,
        'appRttUs': us(rttMs),
      },
      'hostCaptureUs': us(pipeline?.captureMs),
      'hostConvertUs': us(pipeline?.convertMs),
      'hostEncodeUs': us(pipeline?.encodeMs),
      'hostWorkUs': us(pipeline?.workMs),
    };
  }

  void _observeSnapshot(
    int nowUs, {
    Map<String, dynamic>? native,
    bool ended = false,
  }) {
    if (!_observabilityEnabled) return;
    if (native != null) {
      final hardware = native['hardwareDecoder'] ?? native['hardwareDecode'];
      _observedHardwareDecoder = hardware is bool ? hardware : null;
      final age = metricNumber(native['queueOldestMs'], 60000);
      final presentAge = metricNumber(native['presentQueueOldestMs'], 60000);
      _nativeObservation
        ..clear()
        ..addAll({
          'decodeQueuePackets': native['queuedPackets'],
          'decodeQueueBytes': native['queuedBytes'],
          'decodeQueueOldestUs': age == null ? null : (age * 1000).round(),
          'presentQueuePackets': native['presentQueuePackets'],
          'presentQueueOldestUs': presentAge == null
              ? null
              : (presentAge * 1000).round(),
        });
      _trackDecoderObservation.clear();
      if (_tracks != null && native['tracks'] is List) {
        for (final raw in native['tracks'] as List) {
          final value = parseOutputObservation(raw);
          if (_tracks.tracks.any(
            (t) => t.id == value['id'] && t.epoch == value['epoch'],
          )) {
            _trackDecoderObservation[value['id'] as String] = value;
          }
        }
      }
    }
    final freshHost =
        _hostObservationAtUs != null &&
        nowUs >= _hostObservationAtUs! &&
        nowUs - _hostObservationAtUs! <= 3000000 &&
        _hostObservationEpoch == (_tracks?.topology ?? _epoch);
    observationReport.add(
      nowUs,
      ended
          ? 'closed'
          : resizing
          ? 'resizing'
          : frameReady
          ? 'streaming'
          : 'connecting',
      _observationValues(nowUs),
      epoch: _tracks?.topology ?? _epoch,
      outputs: !resizing && _tracks != null
          ? {
              for (final t in _tracks.tracks)
                t.id: {
                  if (freshHost &&
                      _trackHostObservation[t.id]?['epoch'] == t.epoch)
                    'host': _trackHostObservation[t.id],
                  if (_trackDecoderObservation[t.id]?['epoch'] == t.epoch)
                    'decoder': _trackDecoderObservation[t.id],
                },
            }
          : const {},
    );
  }

  String observabilityJson() => observationReport.toJson(
    sessionId: connection.sessionId,
    enabled: _observabilityEnabled,
    negotiated: connection.observability,
    inboxObserved: _inboxProbe?.enabled == true,
    outputs: _tracks?.tracks.length ?? 1,
    path: connection.observabilityPath,
    facts: {
      'captureBackend': captureBackend,
      'targetFps': connection.options.fps,
      'sourceWidth': sourceWidth,
      'sourceHeight': sourceHeight,
      'encodedWidth': width,
      'encodedHeight': height,
      ..._lastEncoderFacts,
      'decoderMode': decodeTier == null
          ? _observedHardwareDecoder == null
                ? 'unknown'
                : _observedHardwareDecoder!
                ? 'hardware'
                : 'software'
          : decodeTier == 0
          ? 'software'
          : 'hardware',
      'cursorNegotiatedMode': cursorSeparated ? 'separate' : 'composed',
    },
    distributions: {
      for (final entry in <String, LatencyPercentiles?>{
        'hostApply': inputLatency.hostApply,
        'roundTrip': inputLatency.roundTrip,
        'toHostEstimate': inputLatency.toHost,
        'inputToTextureProxy': inputLatency.toFrame,
        'framePush': inputLatency.framePush,
        'frameDecode': inputLatency.frameDecode,
        'transfer': inputLatency.transfer,
        'downlinkEstimate': inputLatency.downlink,
      }.entries)
        if (entry.value != null)
          entry.key: {
            'samples': entry.value!.samples,
            'p50Ms': entry.value!.p50Ms,
            'p95Ms': entry.value!.p95Ms,
          },
    },
    versions: {
      'client': clientProductVersion,
      ...connection.observabilityVersions,
    },
  );

  /// How long one packet may wait for room in the native decode queue before
  /// the stream ends as `media_backpressure` (see [_pushMedia]).
  final Duration mediaQueueWait;

  /// Dials the print socket (tests); null: the relay socket.
  final PrintDialer? printDialer;

  /// Builds the print-v1 session (tests; null: the real one over [printDialer]).
  final PrintSessionFactory? printFactory;
  PrintSession _printSession(Uri url, String? folderName) =>
      printFactory?.call(connection, _token, folderName) ??
      PrintSession(
        url: url,
        token: _token,
        sessionId: connection.sessionId,
        dialer: printDialer,
        folderName: folderName,
      );

  /// A new clipboard owner for an independently authorized pair handshake.
  final ClipboardSessionFactory? clipboardFactory;

  /// Builds the drive-v1 session when the broker granted it (optional).
  final DriveSessionFactory? driveFactory;

  /// Builds the usb-v1 session when the broker granted it (optional).
  final UsbSessionFactory? usbFactory;

  /// Builds the udp-media-v1 leg of a grant (tests only).
  final UdpLegFactory? udpLegFactory;
  UdpMedia? _udp;
  final _directInputSequence = DirectInputSequence();
  final _udpTracks = <String, UdpMedia>{};
  final _udpScopes = <String, UdpTrackScope>{};
  int _udpTrackGeneration = 0;

  /// Video reaches this session over UDP, as the leg last reported.
  bool get onUdp => _tracks == null
      ? _udp?.report['udpActivePath'] == 1
      : _udpTracks.length == _tracks.tracks.length &&
            _udpTracks.isNotEmpty &&
            _udpTracks.values.every((leg) => leg.report['udpActivePath'] == 1);
  bool get onDirect =>
      onUdp &&
      (_tracks == null
          ? _udp?.report['udpDirectPath'] == 2
          : _udpTracks.values.every((leg) => leg.report['udpDirectPath'] == 2));
  String outputPathLabel(String track) {
    final report = _udpTracks[track]?.report;
    return report?['udpActivePath'] == 1
        ? (report?['udpDirectPath'] == 2 ? 'P2P' : 'UDP')
        : 'TCP';
  }

  String get videoPathLabel => _closed
      ? l10n.disconnected
      : (_resumingTransport || _hostResuming)
      ? l10n.reconnecting2
      : _tracks != null && _udpTracks.isNotEmpty
      ? _tracks.tracks
            .map((t) => '${int.parse(t.id) + 1}:${outputPathLabel(t.id)}')
            .join(' / ')
      : onDirect
      ? l10n.p2pDirect
      : onUdp
      ? l10n.udpViaServer
      : l10n.tcpViaServer;
  final _statsWindow = StreamStatsWindow();
  final RemoteMedia _media;
  final SessionDialer _dialer;
  final SessionResumeDialer _resumeDialer;
  final Stopwatch _clock;

  // session-resume-v1 (session_resume.dart). The token is the broker's
  // single-use one for the next resume; never logged or stored.
  String? _resumeToken;
  Completer<void>? _abandonSocket;
  bool _resumingTransport = false,
      _hostResuming = false,
      _silenceResumed = false;
  int _lastArrivalUs = 0, _resumeBackstopUs = 0;

  /// No message for this long while pictures stream: the socket is replaced.
  /// The Host's statistics and the ping answers arrive about every second.
  static const resumeSilenceUs = 5000000;

  /// The socket or the Host is coming back; the last picture stays.
  bool get resuming => _resumingTransport || _hostResuming;
  // Independent outputs (tracks-resume-v1): once every output was shown and
  // while none is changing.
  bool get _resumeEligible =>
      connection.sessionResume &&
      _resumeToken != null &&
      !_closed &&
      textureId != null &&
      (_tracks == null
          ? !_initialFramePending
          : _tracks.ready && !_tracks.preparing) &&
      _resize == null &&
      _outputChange == null;
  final _androidObservation = AndroidWatchdogObservation();

  bool _streamAudioOpus = false;

  /// The stream's audio format as its stream info stated it: 1 PCM, 2 Opus
  /// (audio-opus-v1); null with sound off. Diagnostics only.
  int? get audioFormat => !connection.options.audio
      ? null
      : (_tracks?.opusAudio ?? _streamAudioOpus)
      ? 2
      : 1;

  /// The native player's last audio error as an unsigned 32-bit status (0
  /// none); null with sound off. Diagnostics only.
  int? audioErrorCode;

  /// Android-only opt-in. Call at attachment and each foreground transition.
  /// This does not manufacture media progress or change transport ownership.
  void setAndroidForeground(bool foreground) =>
      _androidObservation.setForeground(foreground, _clock.elapsedMicroseconds);

  bool get _androidObservationEligible =>
      _tracks == null && frameReady && !resizing;
  late final TrackStream? _tracks = connection.trackPlan == null
      ? null
      : TrackStream(
          connection.sessionId,
          connection.trackPlan!,
          connection.options,
          profile: connection.profileTracks,
          streamSizeV2: connection.streamSizeV2,
          decoderLimits: connection.streamSize,
          audioOpus: connection.audioOpus,
        );
  List<StreamTrack> get tracks => _tracks?.tracks ?? const [];
  Map<String, int> get trackTextures => _tracks?.textures ?? const {};
  bool get independentOutputs => _tracks != null;
  bool _initialFramePending = true;
  bool _hasStreamed = false;

  /// Historical first decoded readiness, retained after close or decoder swaps.
  /// Metadata, a relay reservation and a Host status snapshot are not readiness.
  bool get hasStreamed => _hasStreamed;

  /// Only initial, explicit Host preparation may use the view's bounded wait.
  /// This is deliberately separate from transport recovery after streaming.
  bool get awaitingInitialHostPreparation =>
      _closed &&
      !_hasStreamed &&
      canRetry &&
      !_transportRetryBlocked &&
      diagnosticCode == 'host_maintenance';
  int? _initialFrameDeadlineUs;
  int? _initialPreparationDeadlineUs;
  bool get frameReady =>
      !_closed &&
      (_signin == null || _signin.active) &&
      (_tracks == null ? !_initialFramePending : _tracks.ready);
  final _inputObservation = SessionInputObservation();

  /// A tip must not interrupt a remotely held key or mouse button.
  bool get remoteInputHeld => _inputObservation.held;
  bool get _inputReady => frameReady;
  WebSocket? _socket;
  StreamSubscription<Map<String, dynamic>>? _input;
  Object? _pointerOrderingOwner;
  VoidCallback? _beforeKeyInput, _discardPointerInput;
  Timer? _timer;
  Timer? _viewportFrameTimer;
  bool _closed = false, _polling = false;
  bool _endRecorded = false;
  Timer? _clipboardReconnectTimer;
  int _clipboardReconnectAttempts = 0;
  bool _clipboardReplacing = false;
  bool get clipboardRecovering =>
      _clipboardReconnectTimer != null || _clipboardReplacing;
  Future<void>? _closing;
  Future<void>? _starting;
  int _captureRevision = 0;
  int _mediaRevision = 0;
  int? textureId;
  int width = 0, height = 0;
  int? sourceWidth, sourceHeight, appliedFps, appliedBitrate;
  int receivedBytes = 0;
  bool captured = false;
  String status = l10n.connectingServer;
  String? error;
  String? diagnosticCode;
  int? _failureError;
  bool canRetry = true;
  double? decodedFps, rttMs, capturedFps, encodedFps, receivedMbps;

  /// tracks-v1: the slowest output's decoded rate in the last reading.
  double? decodedFpsLowest;
  final _lastOutputFrames = <String, int>{};
  double? textureFps, clientDecodeMs, clientDecodeMaxMs, clientQueueMs;
  int? clientQueuePackets, skippedDecodedFrames;

  /// texture-pacing-v1 (per statistics tick): mean decoded -> taken by
  /// Flutter wait, Flutter's mean texture upload, frames queued behind a
  /// waiting one and frames skipped in the tick. Null from older engines.
  double? textureWaitMs, textureUploadMs;
  int? textureQueuedFrames, skippedFramesTick;

  /// How this client renders the picture: a hardware GPU (false: WARP /
  /// basic render driver), the texture path (0 = CPU pixel buffer), the
  /// composition rate Flutter paces frames at and whether the client runs
  /// inside a Remote Desktop session. Null when the engine did not report it.
  bool? gpuHardware, clientRemoteSession;
  int? texturePath, displayHz;

  /// This client's own cost in tenths of a percent (0..1000), read from the
  /// native/Android sampler: this process's CPU, the whole PC's CPU (null on
  /// Android, which hides it), and this process's GPU video-decode / 3D engine.
  /// Null = not measured; never a guessed zero.
  int? appCpuPct10, systemCpuPct10, gpuDecodePct10, gpu3dPct10;

  /// Which adapter decodes (native \decodeTier\: 0 CPU, 1 the adapter Flutter
  /// renders on, 2 another one) and its PCI vendor ID (\decodeVendor\). Fixed
  /// numbers only, never an adapter name.
  int? decodeTier, decodeVendor;

  /// Android: the low-latency configuration its H.264 decoder accepted
  /// (native `decoderLowLatency` bits: 1 KEY_LOW_LATENCY, 2 real-time
  /// priority, 4 operating rate, 8 vendor keys, 16 FEATURE_LowLatency
  /// declared) and the vendor key set its name matched (`decoderVendorKeys`:
  /// 0 none, 1 Qualcomm, 2 Exynos, 3 Kirin, 4 Amlogic, 5 MediaTek).
  int? decoderLowLatency, decoderVendorKeys;
  String? captureBackend;
  HostStreamTelemetry? hostTelemetry;
  int? _hostTelemetryUs;
  bool get hostTelemetryFresh =>
      !_closed &&
      !resizing &&
      _hostTelemetryUs != null &&
      _clock.elapsedMicroseconds - _hostTelemetryUs! <= 5000000;
  String audioStatus = l10n.waiting2;

  /// Lossless tiles currently shown and their recent rate, when the host
  /// sends them (null when the host does not use lossless tiles).
  int? losslessTiles;
  double? losslessKbps;

  /// The host reported no playback device in the VM, so no sound can play.
  bool remoteAudioMissing = false;
  static String get remoteAudioMissingStatus => l10n.desktopHasNoSoundDevice;
  int _lastFrames = 0, _lastStatsUs = 0, _lastMediaUs = 0;
  int _lastVideoUs = 0, _lastDecodedUs = 0;
  int _lastStatsBytes = 0;
  int _lastTextures = 0, _lastDecodeCalls = 0;
  double _lastDecodeTotalMs = 0, _lastDecodeBusyMs = 0;
  int _lastUploads = 0, _lastQueued = 0, _lastSkipped = 0;
  double _lastWaitTotalMs = 0, _lastUploadTotalMs = 0;

  /// Backpressure that no longer ends the stream, per statistics tick:
  /// packets that waited for a full native decode queue, and audio frames
  /// the native playback queue dropped (lifetime totals kept for deltas).
  int? mediaWaitsTick, audioDroppedTick;
  int _mediaWaits = 0, _lastMediaWaits = 0, _lastAudioDropped = 0;

  /// audio-stats-v1: this tick's jitter buffer changes and the Host's audio
  /// members received since the previous tick.
  late final _audioStats = AudioStatsTracker(
    baselines: switch (_media) {
      final SharedAudioCounterMedia media => media.audioCounterBaselines,
      _ => null,
    },
  );
  AudioStatsReading? audioTick;

  /// audio-path-v1 of the latest native reading (audioPathValues); null with
  /// sound off, empty for an engine that does not report it (Windows).
  Map<String, Object>? audioPathTick;

  /// Longest wait between two video packets in the last 1 s. A still screen
  /// sends nothing, so it means a stall only while the picture is moving.
  double? videoGapMaxMsTick;
  int _lastVideoArrivalUs = 0, _videoGapMaxUs = 0;
  int _nonce = 0;
  int _epoch = 0, _resizeNonce = 0;
  bool _awaitingKeyFrame = false;
  _ViewportChange? _resize;
  _OutputChange? _outputChange;
  bool _trackDecoderStarting = false;
  String? resizeError;

  /// While a viewport resize is pending, the previous picture stays on screen
  /// (scaled to the window) until the new size's first frame: its texture and
  /// pixel size. Null when nothing is held (HeldPictureMedia).
  int? heldTextureId;
  (int, int)? heldFrame;
  Timer? _heldRelease;
  int? _heldReleasing;

  void _holdPicture(_ViewportChange pending) {
    heldTextureId = pending.heldTexture = textureId;
    heldFrame = connection.initialViewport != null
        ? (sourceWidth ?? width, sourceHeight ?? height)
        : (width, height);
  }

  /// The new picture is up: the view switches to its texture on its next
  /// frame, and only after that does the held one go.
  void _releaseHeldPicture({bool now = false}) {
    final held = heldTextureId ?? _heldReleasing;
    heldTextureId = null;
    heldFrame = null;
    _heldRelease?.cancel();
    _heldRelease = null;
    _heldReleasing = null;
    final media = _media;
    if (held == null || media is! HeldPictureMedia) return;
    final holder = media as HeldPictureMedia;
    if (now) {
      unawaited(holder.releaseHeld(held));
      return;
    }
    _heldReleasing = held;
    _heldRelease = Timer(const Duration(milliseconds: 250), () {
      _heldRelease = null;
      _heldReleasing = null;
      unawaited(holder.releaseHeld(held));
    });
  }

  /// The stream ended because a remote display change failed or timed out
  /// (the relay ends the session on any Host-side resize failure). The view
  /// reconnects once at the current window size instead of waiting for 재연결.
  bool endedByDisplayChange = false;

  /// The relay ended the stream because it lost the Host's connection
  /// (host_error, host_disconnected, host_timeout, ...). The Host service
  /// reconnects within seconds and restores its display first, so the view
  /// reconnects once and waits quietly for that instead of failing at once.
  bool endedByHostLoss = false;
  bool _hasPresentedFrame = false;
  bool get hasPresentedFrame => _hasPresentedFrame;
  bool get canRetryInitialPreparation =>
      _closed &&
      !_hasPresentedFrame &&
      canRetry &&
      !_transportRetryBlocked &&
      diagnosticCode == 'host_maintenance';

  /// A continuation only: callers must already be waiting on explicit Host
  /// maintenance. An isolated missing-display failure is not transient proof.
  bool get canRetryInitialDisplayPreparation =>
      _closed &&
      !_hasPresentedFrame &&
      connection.initialViewport != null &&
      canRetry &&
      !_transportRetryBlocked &&
      diagnosticCode == 'virtual_display_required';
  bool _transportRetryBlocked = false;
  bool get canAutomaticallyReconnect =>
      _closed &&
      canRetry &&
      !_transportRetryBlocked &&
      !SessionFailure.blocksAutomaticReconnect(diagnosticCode) &&
      (endedByHostLoss || SessionFailure.transientTransportEnd(diagnosticCode));
  bool get resizing =>
      _resize != null || _outputChange != null || _capacityPending;
  bool get canResizeTracks => connection.profileTracks && _tracks != null;
  bool get canResizeViewport =>
      _userPhase &&
      connection.epochMedia &&
      connection.viewportProvisioning &&
      (connection.initialViewport == null || frameReady) &&
      connection.options.monitor != 'all';
  String? _ping;
  int _pingUs = 0;
  // Adaptive bitrate (bitrate-adaptive-v1): video arrival times for the
  // host's controller, reported every 100 ms only in a session that negotiated
  // it (any other session would treat the report as invalid input).
  Timer? _feedbackTimer;
  final _arrivals = <List<int>>[];
  int _feedbackBytes = 0, _feedbackEpoch = 0, _feedbackSinceUs = 0;
  bool get adaptiveBitrate => connection.bitrateAdaptive && _tracks == null;
  bool _losslessOffered = false;

  /// 게임 마우스 holds the mouse: no lossless tiles. Its
  /// pings say "lt0-" instead of "lt1-", and the Host drops every tile with
  /// its next video frame; a Host without that keeps them.
  bool get losslessTilesPaused => _losslessPaused;
  bool _losslessPaused = false;

  /// Pauses or resumes the tiles at once with a ping of its own (one ping is
  /// outstanding at a time: RTT then measures with this one). While the
  /// display changes the next periodic ping carries it, as for the opt-in.
  void pauseLosslessTiles(bool paused) {
    if (paused == _losslessPaused) return;
    _losslessPaused = paused;
    if (!_losslessTilesSession ||
        !_losslessOffered ||
        _closed ||
        resizing ||
        _socket == null) {
      return;
    }
    _sendPing(_clock.elapsedMicroseconds);
  }

  bool get _losslessTilesSession =>
      _media is LosslessTileMedia &&
      (_media as LosslessTileMedia).losslessTiles;

  void _sendPing(int now) {
    final tiles = _losslessTilesSession;
    _ping =
        '${tiles ? (_losslessPaused ? losslessTilesPausePrefix : losslessTilesPingPrefix) : ''}${++_nonce}';
    _pingUs = now;
    _send({'type': 'ping', 'id': _ping!});
  }

  bool get closed => _closed;

  /// The connection is up and the Host is still setting the virtual display
  /// this connection asked for; ends when the stream's size arrives.
  bool get preparingInitialDisplay =>
      !_closed && _initialPreparationDeadlineUs != null;

  // input-latency-v1: only a connection whose broker granted it samples input
  // (independent outputs are not measured by this protocol).
  late final InputLatencyMeter? _latency =
      connection.inputLatency && _tracks == null ? InputLatencyMeter() : null;

  /// Recent input latency of this connection; `supported` is false when the
  /// broker did not grant input-latency-v1 (released broker or host).
  InputLatencySummary get inputLatency =>
      _latency?.summary(
        _clock.elapsedMicroseconds,
        frameSupported: _media is InputFrameWatchMedia,
      ) ??
      InputLatencySummary.unsupported;

  /// p50/p95 (ms) for diagnostics; null when unmeasured or not yet sampled.
  /// Input -> host injection is an ESTIMATE (see InputLatencySummary.toHost);
  /// input -> frame is a decode/texture proxy without display scan-out.
  double? get inputToHostP50Ms => inputLatency.toHost?.p50Ms;
  double? get inputToHostP95Ms => inputLatency.toHost?.p95Ms;
  double? get inputToFrameP50Ms => inputLatency.toFrame?.p50Ms;
  double? get inputToFrameP95Ms => inputLatency.toFrame?.p95Ms;

  /// The client's share of input -> frame: packet arrival -> native engine
  /// accepted it, and accepted -> handed to the texture (p50, ms).
  double? get inputPushP50Ms => inputLatency.framePush?.p50Ms;
  double? get inputDecodeP50Ms => inputLatency.frameDecode?.p50Ms;

  /// The tagged packet's bytes after its tag (InputLatencySummary.transfer,
  /// ms) and its size (bytes, p50).
  double? get inputTransferP50Ms => inputLatency.transfer?.p50Ms;
  double? get inputTransferP95Ms => inputLatency.transfer?.p95Ms;
  double? get inputFrameBytes => inputLatency.frameBytes?.p50Ms;

  /// Per input: the host's tag write -> the packet's arrival here
  /// (InputLatencySummary.downlink, ms).
  double? get inputDownlinkP50Ms => inputLatency.downlink?.p50Ms;
  double? get inputDownlinkP95Ms => inputLatency.downlink?.p95Ms;

  /// cursor-v1 was granted: the host leaves the cursor out of the video and
  /// this PC shows the remote shape as its own cursor (independent outputs
  /// only with cursor-tracks-v1; otherwise they keep the composed cursor).
  bool get cursorSeparated =>
      connection.cursor && (_tracks == null || connection.cursorTracks);

  /// text-input-v1 was granted: committed text may be sent as
  /// textInputMessages() through [input] (independent outputs keep keys).
  bool get textInput => connection.textInput && _tracks == null;

  /// touch-input-v1 was granted: touch frames may be sent with [sendTouch]
  /// (independent outputs never; the broker does not grant it for them).
  bool get touchInput => connection.touchInput && _tracks == null;

  /// relative-mouse-v1 was granted. Without cursor-v1 nothing says when the
  /// remote cursor is hidden, so nothing can start capture.
  bool get relativeMouse =>
      _userPhase && connection.relativeMouse && cursorSeparated;

  /// cursor-echo-v1 was granted (with cursor-v1 and relative-mouse-v1, one
  /// output): [echoCursor] asks the Host for the VM's real cursor position as
  /// it changes, while 게임 마우스 holds the mouse.
  bool get cursorEcho =>
      connection.cursorEcho && relativeMouse && _tracks == null;
  bool _cursorEchoing = false;
  bool get cursorEchoing => _cursorEchoing;

  /// Starts or stops the Host's cursor echo. False, with nothing sent, when
  /// not granted or not connected; a repeated value sends nothing.
  bool echoCursor(bool on) {
    if (!cursorEcho || _closed || _socket == null) return false;
    if (on == _cursorEchoing) return true;
    _cursorEchoing = on;
    _send(cursorEchoMessage(on));
    return true;
  }

  /// cursor-compose-v1 was granted (with cursor-v1 and relative-mouse-v1):
  /// [composeCursor] asks the Host to draw the VM's real cursor into the
  /// video while 게임 마우스 holds the mouse; preferred over the echo. On
  /// independent outputs (relative-mouse-tracks-v1) the output the cursor is
  /// over draws it, and it is the only way 게임 마우스 shows the cursor there.
  bool get cursorCompose => connection.cursorCompose && relativeMouse;
  bool _cursorComposing = false;
  bool get cursorComposing => _cursorComposing;

  /// Starts or stops the Host's composed cursor. False, with nothing sent,
  /// when not granted or not connected; a repeated value sends nothing.
  bool composeCursor(bool on) {
    if (!cursorCompose || _closed || _socket == null) return false;
    if (on == _cursorComposing) return true;
    _cursorComposing = on;
    _send(cursorComposeMessage(on));
    if (on) {
      _endComposeHold();
    } else {
      _startComposeHold();
    }
    return true;
  }

  /// After compose off the video still carries the VM's cursor until the
  /// Host's next tick (one round trip). Meanwhile no local cursor is shown:
  /// cursor records wait here and the view keeps its fallback pointer away.
  /// The Host's first real record (a shape, or a visible state; it composes
  /// only hidden states) names the last composed tick by its PTS; the first
  /// video frame after that tick given to the decoder ends the wait. A cursor
  /// that stays hidden sends nothing, so 250 ms beyond the measured round
  /// trip ends it in any case.
  bool get cursorSettling => _composeHeld != null;
  List<(int, Uint8List)>? _composeHeld;
  int? _composeAckPts;
  int _composeVideoPts = -1;
  Timer? _composeTimer;

  void _startComposeHold() {
    _composeHeld ??= [];
    _composeAckPts = null;
    _composeVideoPts = -1;
    _composeTimer?.cancel();
    final rtt = (rttMs ?? 0).clamp(0, 750).round();
    _composeTimer = Timer(Duration(milliseconds: 250 + rtt), _endComposeHold);
  }

  /// One cursor record of [pts] while the hold lasts.
  void _holdCursor(int epoch, Uint8List record, int pts) {
    final held = _composeHeld!..add((epoch, record));
    if (record[0] == cursorShapeRecord || record[1] & 1 != 0) {
      _composeAckPts ??= pts;
    }
    final ack = _composeAckPts;
    if ((ack != null && _composeVideoPts > ack) || held.length >= 64) {
      _endComposeHold();
    }
  }

  /// A video frame of [pts] reached the decoder.
  void _composeVideo(int pts) {
    if (_composeHeld == null) return;
    if (pts > _composeVideoPts) _composeVideoPts = pts;
    final ack = _composeAckPts;
    if (ack != null && pts > ack) _endComposeHold();
  }

  void _endComposeHold() {
    final held = _composeHeld;
    _composeTimer?.cancel();
    _composeTimer = null;
    if (held == null) return;
    _composeHeld = null;
    if (_closed) return;
    // Invoked in order now; the platform channel keeps that order with every
    // later record.
    for (final (epoch, record) in held) {
      unawaited(_presentCursor(epoch, record).catchError((Object _) {}));
    }
    notifyListeners();
  }

  /// Sends one touch frame (touch_input.dart) in order with other input.
  /// False, with nothing sent, when touch-input-v1 was not granted, the frame
  /// is invalid, or input is not accepted now (not captured, resizing, closed,
  /// before the first picture). The Host releases every contact on
  /// `input.reset` (capture loss, close) and ignores later move/up/cancel of
  /// released contacts.
  bool sendTouch(List<RemoteTouchContact> contacts) {
    if (!SessionHumanSource.inputAllowed) return false;
    final message = touchInput ? touchInputMessage(contacts) : null;
    if (message == null ||
        _clipboardTransferBlocked ||
        !captured ||
        _closed ||
        resizing ||
        !_inputReady ||
        _socket == null) {
      return false;
    }
    _inputGate.add(message);
    return true;
  }

  /// caret-v1 was granted (independent outputs never): [watchCaret] asks the
  /// Host for text-caret reports, which arrive in [caretUpdates]. False for an
  /// older Host or broker, whose clients follow taps instead.
  bool get caretSupported => connection.caret && _tracks == null;

  /// The latest caret report of the Host (stamped with its arrival time), or
  /// null before one / after watching stopped. Listeners fire per report.
  final ValueNotifier<RemoteCaret?> caretUpdates = ValueNotifier(null);
  bool _caretWatching = false;

  /// Starts or stops the Host's caret reports (the soft keyboard opened or
  /// closed). False, with nothing sent, when not supported or not connected.
  bool watchCaret(bool on) {
    if (!caretSupported || _closed || _socket == null) return false;
    if (on == _caretWatching) return true;
    _caretWatching = on;
    if (!on) caretUpdates.value = null;
    _send(caretWatchMessage(on));
    return true;
  }

  /// display-scale-v1 was granted: the Host sets the Windows display scale of
  /// the streamed output to [displayScaleWanted] (independent outputs: of
  /// every owned output, only with display-scale-outputs-v1).
  bool get displayScaleSupported =>
      _userPhase &&
      (connection.displayScaleControl || connection.displayScale != null) &&
      (_tracks == null || connection.displayScaleOutputs);

  /// The scale this client asked for last (the connect request's at first).
  late int? displayScaleWanted = displayScaleSupported
      ? connection.displayScale
      : null;

  /// The Host's latest result for this session's output (null until one).
  DisplayScaleApplied? displayScaleApplied;

  /// stream-size-v1 was granted: full-size pictures within this client's
  /// declared decoder limits, portrait outputs up to 2160x3840, mid-epoch
  /// picture changes and `stream.decoder` load reports.
  bool get streamSize => connection.streamSize != null && _tracks == null;

  /// stream-size-v1: the Host's picture scale of the source (per mille) and
  /// why it is smaller; null without the grant or before the Host said.
  int? encodeScale;
  EncodeLimit? encodeLimit;

  /// This client's decoder busy share in the last report (per mille).
  int? decoderBusy;

  /// The picture changes size within the epoch (stream-size-v1) until the
  /// restarted decoder has a frame; input continues (same source).
  bool get pictureChanging => _pictureChanging;
  bool _pictureChanging = false;

  void _readEncodeScale(Map<String, dynamic> event) {
    if (!streamSize) return;
    final scale = event['encodeScale'];
    encodeScale = scale is int && scale >= 1 && scale <= 1000 ? scale : null;
    encodeLimit = EncodeLimit.parse(event['encodeLimit']);
  }

  /// stream-size-v1: the Host restarted its encoder at another scale of the
  /// same source within this epoch (a measured overload, or headroom again).
  /// Only the decoder restarts, at the new size from the next IDR; input
  /// capture and the remote pointer state are kept.
  Future<void> _rescale(Map<String, dynamic> event, int w, int h) async {
    final limits = _activeDecoderLimits!;
    final source = (sourceWidth, sourceHeight);
    if (event['sourceWidth'] != source.$1 ||
        event['sourceHeight'] != source.$2 ||
        event['fpsTarget'] != connection.options.fps ||
        w > (source.$1 ?? 0) ||
        h > (source.$2 ?? 0) ||
        w >
            (connection.streamSizeV2
                ? limits.maxWidth
                : connection.options.maxWidth) ||
        h >
            (connection.streamSizeV2
                ? limits.maxHeight
                : connection.options.maxHeight) ||
        !limits.fits(w, h, connection.options.fps)) {
      throw const FormatException('Invalid picture change.');
    }
    _readEncodeScale(event);
    if (w == width && h == height) {
      notifyListeners();
      return;
    }
    final captureRevision = _captureRevision;
    _mediaRevision++;
    _latency?.resetFrames();
    _pictureChanging = true;
    final media = _media;
    final texture =
        await (media is PictureChangeMedia
                ? (media as PictureChangeMedia).restart(w, h)
                : media.start(w, h))
            .timeout(const Duration(seconds: 5));
    if (_closed) {
      try {
        await _media.stop().timeout(const Duration(seconds: 2));
      } catch (_) {}
      return;
    }
    textureId = texture;
    width = w;
    height = h;
    _awaitingKeyFrame = true;
    decodedFps = receivedMbps = textureFps = null;
    clientDecodeMs = clientDecodeMaxMs = clientQueueMs = null;
    clientQueuePackets = skippedDecodedFrames = null;
    textureWaitMs = textureUploadMs = null;
    textureQueuedFrames = skippedFramesTick = null;
    appCpuPct10 = systemCpuPct10 = gpuDecodePct10 = gpu3dPct10 = null;
    _lastTextures = _lastDecodeCalls = 0;
    _lastDecodeTotalMs = _lastDecodeBusyMs = 0;
    _lastUploads = _lastQueued = _lastSkipped = 0;
    _lastWaitTotalMs = _lastUploadTotalMs = 0;
    _lastAudioDropped = 0;
    _audioStats.reset();
    _lastFrames = 0;
    _lastOutputFrames.clear();
    _stallWatch.restart();
    decodedFpsLowest = null;
    _lastMediaUs = _lastVideoUs = _lastDecodedUs = _lastStatsUs =
        _clock.elapsedMicroseconds;
    _lastStatsBytes = receivedBytes;
    // The Host's picture change can release native capture. Restore only the
    // still-current ownership: focus loss, a menu or a fresh capture request
    // while the decoder restarted supersedes this old request. capture()
    // also fences its result against a later release/new owner.
    if (captured && captureRevision == _captureRevision && !_closed) {
      await capture(true);
    }
    if (!_closed) notifyListeners();
  }

  /// Asks the Host for another Windows display scale; false when not granted,
  /// already asked for or the session is over. The Host keeps it across
  /// resizes and puts the previous value back when the session ends.
  bool requestDisplayScale(int percent) {
    if (!displayScaleSupported ||
        _closed ||
        _socket == null ||
        !validDisplayScale(percent) ||
        percent == displayScaleWanted) {
      return false;
    }
    displayScaleWanted = percent;
    _send({'type': 'display.scale', 'scale': percent});
    notifyListeners();
    return true;
  }

  CursorView? _cursorView;
  int? _cursorViewTexture;
  bool _cursorFailureLogged = false;
  bool _cursorStateReady = false;
  int? _cursorStateTexture;
  bool _cursorHidden = false;
  Timer? _cursorRetry;
  int _cursorStateRevision = 0;
  static const _cursorRetryDelays = [
    Duration(milliseconds: 100),
    Duration(milliseconds: 250),
    Duration(milliseconds: 500),
  ];

  void _cancelCursorRetry() {
    _cursorRetry?.cancel();
    _cursorRetry = null;
  }

  /// The current accepted cursor state says hidden. Missing or failed state is
  /// not hidden: only an explicit record is proof.
  bool get remoteCursorHidden => _cursorHidden && !localCursorFallback;

  /// A separated cursor needs one successfully accepted current native state.
  /// Shape-only, missing data and native failures keep a usable local pointer.
  /// A successful explicit-hidden state is ready; baked legacy cursors keep
  /// their existing video-only presentation.
  bool get localCursorFallback =>
      cursorSeparated &&
      (!_cursorStateReady || _cursorStateTexture != textureId);

  /// Picture geometry for the native cursor presenter; repeated identical
  /// geometry for the same decoder is sent once.
  Future<void> cursorView(CursorView view) async {
    final media = _media;
    final texture = textureId;
    if (!cursorSeparated ||
        _closed ||
        media is! RemoteCursorMedia ||
        texture == null ||
        !view.valid ||
        (view == _cursorView && texture == _cursorViewTexture)) {
      return;
    }
    _cursorView = view;
    _cursorViewTexture = texture;
    try {
      await (media as RemoteCursorMedia)
          .cursorView(view)
          .timeout(const Duration(seconds: 2));
    } catch (_) {
      // Retried with the next layout; a cursor view never ends the session.
      if (identical(_cursorView, view)) _cursorView = null;
    }
  }

  Future<void> _presentCursor(int epoch, Uint8List record) async {
    // Compose-off may hold records across an ordinary decoder restart.
    if (_tracks == null && epoch != _epoch) return;
    final media = _media;
    if (media is! RemoteCursorMedia) return;
    final texture = textureId;
    if (texture == null) return;
    final state = record[0] == cursorStateRecord;
    if (state) {
      _cancelCursorRetry();
      _cursorStateRevision++;
    }
    final revision = _cursorStateRevision;
    final mediaRevision = _mediaRevision, mediaEpoch = _epoch;
    final topology = _tracks?.topology, phaseRevision = _signin?.revision;
    final userPhase = _userPhase;
    bool current({bool retry = false}) =>
        !_closed &&
        _closing == null &&
        texture == textureId &&
        revision == _cursorStateRevision &&
        mediaRevision == _mediaRevision &&
        mediaEpoch == _epoch &&
        topology == _tracks?.topology &&
        phaseRevision == _signin?.revision &&
        userPhase == _userPhase &&
        (!retry ||
            (userPhase &&
                !resizing &&
                !_trackDecoderStarting &&
                (_tracks != null || !_awaitingKeyFrame) &&
                !cursorSettling));
    // Only a state may be retried: native shape IDs strictly increase. A
    // replayed position would move the pointer back, so retry visibility and
    // shape identity only, never an old application warp or cursor echo.
    final retryState = state ? Uint8List.fromList(record) : null;
    if (retryState != null) {
      retryState[1] &= 1;
      retryState.fillRange(8, 16, 0);
    }
    Future<void> present(int attempt) async {
      if (!current(retry: attempt != 0)) return;
      try {
        await (media as RemoteCursorMedia)
            .cursor(epoch, attempt == 0 ? record : retryState!)
            .timeout(const Duration(seconds: 2));
        if (!current(retry: attempt != 0)) return;
        if (state) {
          final hidden = record[1] & 1 == 0;
          final changed = localCursorFallback || hidden != _cursorHidden;
          _cursorStateReady = true;
          _cursorStateTexture = texture;
          _cursorHidden = hidden;
          if (changed) notifyListeners();
        }
        return;
      } on PlatformException catch (error) {
        if (!current(retry: attempt != 0)) return;
        if (error.code == 'session_replaced') {
          if (attempt == 0) rethrow;
          diagnosticCode = error.code;
          await _fail(l10n.disconnectedReconnect);
          return;
        }
        _cursorFailed();
      } on TimeoutException {
        if (!current(retry: attempt != 0)) return;
        _cursorFailed();
      }
      if (retryState == null ||
          attempt >= _cursorRetryDelays.length ||
          !current(retry: true)) {
        return;
      }
      _cursorRetry = Timer(_cursorRetryDelays[attempt], () {
        _cursorRetry = null;
        if (!current(retry: true)) return;
        unawaited(present(attempt + 1).catchError((Object _) {}));
      });
    }

    await present(0);
  }

  void _cursorFailed() {
    // The picture/input keep working and Flutter supplies a visible fallback.
    final changed = !localCursorFallback;
    _cursorStateReady = false;
    _cursorStateTexture = null;
    _cursorHidden = false;
    if (changed && !_closed) notifyListeners();
    // Debug text is fixed, never a native exception or clipboard content.
    if (_cursorFailureLogged) return;
    _cursorFailureLogged = true;
    assert(() {
      debugPrint('Remote debug: native cursor presentation failed');
      return true;
    }());
  }

  /// The current view owns move ordering. Revoking an old view cannot remove a
  /// newer view's binding, and resets discard rather than flush stale moves.
  VoidCallback bindPointerOrdering({
    required VoidCallback beforeKey,
    required VoidCallback discard,
  }) {
    _discardPointerInput?.call();
    if (_closed) {
      discard();
      return () {};
    }
    final owner = Object();
    _pointerOrderingOwner = owner;
    _beforeKeyInput = beforeKey;
    _discardPointerInput = discard;
    return () {
      if (!identical(_pointerOrderingOwner, owner)) return;
      _pointerOrderingOwner = null;
      _beforeKeyInput = null;
      _discardPointerInput = null;
      discard();
    };
  }

  ClipboardSession? clipboard;

  /// print-v1: what the VM printed to its Multitap printer, offered for local
  /// printing (null when not granted or before the first picture). Its own
  /// notifications are not repeated here; the view listens to it directly.
  PrintSession? printer;

  /// channel-resume-v1 (channel_redial.dart): the broker granted it, so
  /// printing, shared folders and USB devices that ended for a transient
  /// reason are dialled again in this session, and the Host does the same.
  bool get channelResume => _channelResume;
  bool _channelResume = false;
  late final ChannelRedial _printRedial = _redial(_replacePrint),
      _driveRedial = _redial(_replaceDrive),
      _usbRedial = _redial(_replaceUsb);
  ChannelRedial _redial(Future<bool> Function() dial) => ChannelRedial(
    enabled: () => _channelResume && !_closed,
    away: () => resuming,
    dial: dial,
    changed: _redialChanged,
  );

  /// The ended channel each redial already heard of, and how many print
  /// channels this session dialled (each keeps its documents in its own folder).
  Object? _printSettled, _driveSettled, _usbSettled;
  int _printDials = 0;

  /// A channel is down and coming back by itself (or waits for the stream):
  /// the ended object says so to the views, which then show no "connect
  /// again" notice. It is cleared when nothing more is tried.
  void _redialChanged() {
    if (_closed) return;
    final print = printer, folders = drive, devices = usb;
    if (print != null) {
      print.recovering = print.closed && _printRedial.recovering;
    }
    if (folders != null) {
      folders.recovering = folders.closed && _driveRedial.recovering;
    }
    if (devices != null) {
      devices.recovering = devices.closed && _usbRedial.recovering;
    }
    notifyListeners();
  }

  /// A side channel ended with [error] (null: this session closed it).
  void _channelEnded(
    ChannelRedial redial,
    String kind,
    String? error,
    bool refused,
  ) {
    if (error == null) return;
    if (!refused && channelEndTransient(kind, error)) {
      redial.ended();
    } else {
      redial.refused();
    }
  }

  void _printerChanged() {
    final current = printer;
    if (_closed || current == null) return;
    if (current.closed) {
      if (identical(current, _printSettled)) return;
      _printSettled = current;
      _channelEnded(_printRedial, 'print', current.error, current.dialRefused);
    } else if (current.ready) {
      _printRedial.paired();
    }
  }

  Future<bool> _replacePrint() async {
    final previous = printer, url = connection.printWebSocketUrl;
    if (_closed ||
        !_userPhase ||
        previous == null ||
        !previous.closed ||
        url == null) {
      return true;
    }
    previous.removeListener(_printerChanged);
    previous.recovering = false;
    final fresh = _printSession(
      url,
      '${connection.sessionId}-r${++_printDials}',
    )..addListener(_printerChanged);
    printer = fresh;
    notifyListeners();
    unawaited(fresh.start());
    return true;
  }

  Future<bool> _replaceDrive() async {
    final previous = drive, factory = driveFactory;
    if (_closed ||
        !_userPhase ||
        previous == null ||
        !previous.closed ||
        factory == null) {
      return true;
    }
    previous.removeListener(_driveChanged);
    previous.recovering = false;
    // The same folders, ids and names: the desktop maps them as before.
    final fresh = factory(connection, _token)..inherit(previous);
    drive = fresh..addListener(_driveChanged);
    notifyListeners();
    unawaited(fresh.start().catchError((Object _) {}));
    return true;
  }

  Future<bool> _replaceUsb() async {
    final previous = usb, factory = usbFactory;
    if (_closed ||
        !_userPhase ||
        previous == null ||
        !previous.closed ||
        factory == null) {
      return true;
    }
    // Never two owners of one device: the ended channel first gives every
    // device back to the PC. A timeout is uncertainty, so nothing is dialled
    // in this attempt.
    try {
      await previous.settled.timeout(const Duration(seconds: 6));
    } on TimeoutException {
      return false;
    }
    if (_closed || !_userPhase || !identical(usb, previous)) return true;
    previous.removeListener(_usbChanged);
    previous.recovering = false;
    // The devices that were connected then, if they are still plugged in.
    final fresh = factory(connection, _token)..resume(previous.sharedAtEnd);
    usb = fresh..addListener(_usbChanged);
    notifyListeners();
    unawaited(fresh.start().catchError((Object _) {}));
    return true;
  }

  /// Folders shared with the VM (drive-v1); null when not granted.
  DriveSession? drive;
  void _driveChanged() {
    final current = drive;
    if (_closed || current == null) return;
    if (current.closed) {
      if (!identical(current, _driveSettled)) {
        _driveSettled = current;
        _channelEnded(
          _driveRedial,
          'drive',
          current.error,
          current.dialRefused,
        );
      }
    } else if (current.ready) {
      _driveRedial.paired();
    }
    notifyListeners();
  }

  /// USB devices of this PC lent to the VM (usb-v1); null when not granted.
  UsbSession? usb;
  bool _clipboardTransferBlocked = false,
      _clipboardTransferBlockRequested = false;
  bool get clipboardTransferBlocked => _clipboardTransferBlocked;
  bool get preparingClipboardPaste => _inputGate.preparingPaste;

  /// The UI may block new actions while preserving one already authorized
  /// Ctrl+V. Real focus loss still uses capture(false), which revokes it.
  void setClipboardTransferBlocked(bool blocked) {
    _clipboardTransferBlockRequested = blocked;
    _clipboardTransferBlocked =
        blocked || (_clipboardTransferBlocked && _inputGate.preparingPaste);
    _inputGate.setClipboardBlocked(_clipboardTransferBlocked);
  }

  void _clipboardPasteSettled() {
    _clipboardTransferBlocked =
        _clipboardTransferBlockRequested ||
        (_clipboardTransferBlocked && _inputGate.preparingPaste);
    _inputGate.setClipboardBlocked(_clipboardTransferBlocked);
    if (!_closed) notifyListeners();
  }

  void _cancelBlockedClipboardPaste() {
    unawaited(clipboard?.cancelTransfer());
    if (!_closed) notifyListeners();
  }

  void _usbChanged() {
    final current = usb;
    if (_closed || current == null) return;
    if (current.closed) {
      if (!identical(current, _usbSettled)) {
        _usbSettled = current;
        _channelEnded(_usbRedial, 'usb', current.error, current.dialRefused);
      }
    } else if (current.ready) {
      _usbRedial.paired();
    }
    notifyListeners();
  }

  RemoteInputGate? _ownedInputGate;
  RemoteInputGate get _inputGate => _ownedInputGate ??= _makeInputGate();
  RemoteInputGate _makeInputGate() {
    // The closure belongs to the observed phase, not whatever phase happens to
    // be current when a delayed clipboard preparation finally completes.
    final phaseOwner = _signin?.inputOwner;
    return RemoteInputGate(
      send: (event) => _sendInput(event, owner: phaseOwner),
      pasteEnabled: () =>
          !_closed &&
          !resizing &&
          captured &&
          clipboard?.automatic == true &&
          clipboard?.canSend == true,
      preparePaste: () => clipboard?.preparePaste() ?? Future.value(true),
      onPasteBlocked: () => clipboard?.pasteBlocked(),
      onPasteSettled: _clipboardPasteSettled,
      onBlockedEscape: _cancelBlockedClipboardPaste,
      observationEnabled: () => _observabilityEnabled,
      nowUs: () => _clock.elapsedMicroseconds,
      onWaitObserved: (us, queued) {
        if (!_observabilityEnabled || _closed) return;
        _gateWaitCount++;
        if (us > _gateWaitMaxUs) _gateWaitMaxUs = us;
      },
    );
  }

  void _retirePhaseInputGate() {
    _ownedInputGate?.reset();
    _ownedInputGate = null;
  }

  void _clipboardChanged() {
    if (!_closed) {
      final current = clipboard;
      if (current != null && current.closed) {
        _scheduleClipboardReconnect(current);
      } else {
        unawaited(current?.setSharingActive(captured));
      }
      notifyListeners();
    }
  }

  Future<void> start() => _starting ??= _start();
  Future<void> _start() async {
    if (_closed) return;
    // monitors = independent outputs ("모든 모니터"), so a failing
    // all-monitor session is distinguishable in the diagnostics.
    connectTiming.mark('start', _clock.elapsedMicroseconds);
    ProductLog.instance.record(LogEvent.streamStart, {
      if (connection.trackPlan != null)
        'monitors': connection.trackPlan!.outputs.length,
    });
    try {
      if (_tracks != null && _media is! RemoteTrackMedia) {
        throw PlatformException(code: 'independent_outputs_unavailable');
      }
      final viewport = connection.initialViewport?.viewport;
      final plan = connection.trackPlan;
      _prepareDecoder(
        viewport?.width ?? plan?.outputs.first.width ?? 1920,
        viewport?.height ?? plan?.outputs.first.height ?? 1080,
        viewport?.fps ?? connection.options.fps,
        plan?.outputs.length ?? 1,
      );
      _socket = await _dialer(connection.webSocketUrl, _token);
      connectTiming.mark('socket', _clock.elapsedMicroseconds);
      if (_closed) {
        await _socket!.close();
        return;
      }
      _socket!.pingInterval = const Duration(seconds: 10);
      _input = _media.input.listen((event) {
        if (event['type'] == 'capture.released') {
          _discardPointerInput?.call();
          _captureRevision++;
          captured = false;
          _inputGate.reset();
          unawaited(clipboard?.setSharingActive(false));
          _send({'type': 'input.reset'});
          notifyListeners();
        } else {
          if (event['event'] == 'text' &&
              (connection.signin != null ||
                  !textInput ||
                  !validTextInputMessage(event) ||
                  !captured ||
                  _closed ||
                  resizing ||
                  !_inputReady)) {
            return;
          }
          if (event['event'] == 'moveRelative' && !relativeMouse) return;
          if (event['type'] == 'input.reset') {
            _discardPointerInput?.call();
          } else if (event['type'] == 'input' &&
              (event['event'] == 'key' || event['event'] == 'text')) {
            _beforeKeyInput?.call();
          }
          if (event['type'] == 'input.reset' ||
              (captured && !_closed && !resizing && _inputReady)) {
            _inputGate.add(event);
          }
        }
      }, onError: (_) => _fail(l10n.couldntPrepareKeyboardInputSo));
      _lastMediaUs = _lastArrivalUs = _clock.elapsedMicroseconds;
      if (connection.initialViewport != null ||
          connection.profileTracks ||
          _signin != null) {
        _initialPreparationDeadlineUs = _lastMediaUs + 90000000;
      }
      _lastVideoUs = _lastDecodedUs = _lastMediaUs;
      _timer = Timer.periodic(
        const Duration(seconds: 1),
        (_) => unawaited(_poll()),
      );
      if (adaptiveBitrate) {
        _feedbackSinceUs = _clock.elapsedMicroseconds;
        _feedbackTimer = Timer.periodic(
          const Duration(milliseconds: 100),
          (_) => _sendFeedback(),
        );
      }
      status = connection.initialViewport != null
          ? l10n.preparingVirtualDisplayResolution
          : l10n.waitingHostScreen;
      notifyListeners();
      await for (final message in _resumableInbox()) {
        if (_closed) break;
        if (message is String) {
          if (message.length > 65536) throw const FormatException();
          final event = jsonDecode(message) as Map<String, dynamic>;
          assert(() {
            if (event['type'] == 'stream.info') {
              debugPrint(
                'Remote debug: stream.info ${event['width']}x${event['height']}',
              );
            } else if (event['type'] == 'session.ended') {
              final reason = event['reason'];
              final safeReason =
                  reason is String && RegExp(r'^[a-z_]{1,64}$').hasMatch(reason)
                  ? reason
                  : 'unspecified';
              debugPrint('Remote debug: session.ended reason=$safeReason');
            }
            return true;
          }());
          await _control(event);
        } else if (message is List<int>) {
          // Dequeue time: client-side backlog counts as path delay too.
          final arrivedUs = _clock.elapsedMicroseconds;
          if (_tracks != null) {
            final bytes = message is Uint8List
                ? message
                : Uint8List.fromList(message);
            // cursor-tracks-v1 records (TVT1 kind 3) exist only in a granted
            // connection; anywhere else they stay an invalid output packet.
            if (cursorSeparated &&
                bytes.length > 4 &&
                bytes[4] == cursorMediaKind) {
              final record = _tracks.cursorRecord(bytes);
              // Never a position: the pointer is already over its output.
              if (record != null &&
                  (!validCursorRecord(record) ||
                      (record[0] == cursorStateRecord && record[1] & 2 != 0))) {
                throw const FormatException();
              }
              receivedBytes += bytes.length;
              if (record != null) await _presentCursor(0, record);
              continue;
            }
            if (!_tracks.accept(bytes, _clock.elapsedMicroseconds)) continue;
            _lastMediaUs = _clock.elapsedMicroseconds;
            if (bytes[4] == 1) _lastVideoUs = _lastMediaUs;
            receivedBytes += bytes.length;
            await _pushMedia(() => _media.push(bytes));
            continue;
          }
          final wireEpoch = message.length >= 4
              ? message[2] | (message[3] << 8)
              : -1;
          // cursor-v1 records (kind 3) exist only in a granted connection;
          // anywhere else they stay an invalid stream message.
          final cursorRecord =
              cursorSeparated &&
              message.isNotEmpty &&
              message[0] == cursorMediaKind;
          if (message.length < 13 ||
              message.length > 4 * 1024 * 1024 ||
              (!connection.epochMedia && wireEpoch != 0) ||
              (cursorRecord
                  ? message[1] != 0
                  : (message[0] == 2
                        ? message[1] != 0
                        : message[1] & ~1 != 0)) ||
              (!cursorRecord && message[0] != 1 && message[0] != 2)) {
            throw const FormatException();
          }
          if (_signin != null &&
              !_signin.active &&
              (!_signin.infoAccepted || !_signinPrivacyReady)) {
            continue;
          }
          if (connection.epochMedia &&
              ((_resize != null && !_resize!.decoderStarted) ||
                  wireEpoch < _epoch)) {
            continue;
          }
          if (wireEpoch != _epoch || textureId == null) {
            throw const FormatException();
          }
          if (cursorRecord) {
            // Not video: no key-frame, stall, arrival or latency accounting.
            final bytes = message is Uint8List
                ? message
                : Uint8List.fromList(message);
            final record = Uint8List.sublistView(bytes, 12);
            if (!validCursorRecord(record)) throw const FormatException();
            receivedBytes += message.length;
            // The host starts after the epoch's first key frame; anything
            // earlier belongs to no picture yet.
            if (_awaitingKeyFrame) continue;
            if (_composeHeld != null) {
              _holdCursor(wireEpoch, record, _packetPts(bytes));
              continue;
            }
            // cursor-echo-v1: an echoed position (a state with a position
            // that keeps the accepted visibility) is handed on without
            // waiting, so a few hundred a second never hold the video behind
            // them; the platform channel keeps their order with every other
            // record.
            if (_cursorEchoing &&
                record[0] == cursorStateRecord &&
                record[1] & 2 != 0 &&
                _cursorStateReady &&
                _cursorStateTexture == textureId &&
                (record[1] & 1 == 0) == _cursorHidden) {
              unawaited(
                _presentCursor(wireEpoch, record).catchError((Object _) {}),
              );
              continue;
            }
            await _presentCursor(wireEpoch, record);
            continue;
          }
          if (adaptiveBitrate) _noteArrival(message, wireEpoch, arrivedUs);
          if (message[0] == 1) _noteVideoArrival(arrivedUs);
          final firstSigninIdr =
              _signin != null &&
              !_signin.active &&
              signinIdrPacket(message, wireEpoch);
          if (_awaitingKeyFrame) {
            if (message[0] == 2) {
              receivedBytes += message.length;
              _lastMediaUs = _clock.elapsedMicroseconds;
              continue;
            }
            if (message[0] != 1 ||
                (message[1] & 1) == 0 ||
                (_signin != null && !_signin.active && !firstSigninIdr)) {
              continue;
            }
            _awaitingKeyFrame = false;
            connectTiming.mark('keyFrame', arrivedUs);
          }
          _lastMediaUs = _clock.elapsedMicroseconds;
          if (message[0] == 1) _lastVideoUs = _lastMediaUs;
          receivedBytes += message.length;
          if (message[0] == 2 && !connection.options.audio) continue;
          final packet = message is Uint8List
              ? message
              : Uint8List.fromList(message);
          // Epoch belongs to the relay envelope. The native codec continues to
          // receive its stable reserved-zero media-v1 input contract.
          if (connection.epochMedia) {
            packet[2] = 0;
            packet[3] = 0;
          }
          // input-latency-v1: the packet the host tagged is pushed with a
          // frame watch; every packet keeps its order and its single push.
          final latency = _latency;
          final media = _media;
          final pts = latency != null && packet[0] == 1
              ? _packetPts(packet)
              : 0;
          if (latency != null &&
              media is InputFrameWatchMedia &&
              packet[0] == 1 &&
              latency.watch(
                pts,
                _clock.elapsedMicroseconds,
                bytes: packet.length,
              )) {
            await _pushMedia(
              () => (media as InputFrameWatchMedia).pushWatched(packet),
            );
            latency.pushed(pts, _clock.elapsedMicroseconds);
          } else {
            await _pushMedia(() => _media.push(packet));
          }
          if (firstSigninIdr) {
            _signin.acceptedIdr(wireEpoch);
          }
          if (_composeHeld != null && packet[0] == 1) {
            _composeVideo(_packetPts(packet));
          }
          if (_initialFramePending) unawaited(_poll(readinessOnly: true));
          _scheduleViewportFrameCheck();
        } else {
          throw const FormatException();
        }
      }
      if (!_closed) {
        _transportRetryBlocked = !SessionFailure.transientCloseCode(
          _socket?.closeCode,
        );
        final reason = SessionFailure.transportCloseReason(
          _socket?.closeReason,
        );
        final failure = reason == null
            ? SessionFailure('relay_closed', l10n.disconnectedReconnect)
            : SessionFailure.ended(reason);
        diagnosticCode = relayEndCode(reason, failure.code);
        endedByHostLoss = failure.code == 'host_disconnected';
        canRetry = failure.canRetry;
        error = status = failure.message;
        await close(notifyHost: false);
      }
    } catch (failure) {
      if (_closed) return;
      diagnosticCode = failure is PlatformException
          ? failure.code
          : failure is FormatException
          ? 'invalid_stream_message'
          : failure is WebSocketException
          ? 'websocket_handshake_or_transport'
          : failure is SocketException
          ? 'network_error'
          : failure is TimeoutException
          ? 'media_timeout'
          : failure.runtimeType.toString();
      // The fixed code reaches the diagnostics as it is (a native code outside
      // the fixed list is recorded as media_platform_error), with the native
      // engine's own error number when it reports one.
      if (failure is PlatformException) {
        if (!diagnosticCodes.contains(diagnosticCode)) {
          diagnosticCode = 'media_platform_error';
        }
        try {
          final native = await _media.stats().timeout(
            const Duration(seconds: 2),
          );
          if (native['error'] is int) _failureError = native['error'] as int;
        } catch (_) {}
      }
      if (!_closed) await _fail(l10n.disconnectedReconnect);
    }
  }

  /// The session socket's messages in order. session-resume-v1: when the socket
  /// is lost (or abandoned after [resumeSilenceUs] of silence) it is replaced
  /// within the broker's grace window and the messages continue; otherwise the
  /// stream ends exactly as it did without resume.
  Stream<dynamic> _resumableInbox() async* {
    while (true) {
      final abandon = _abandonSocket = Completer<void>();
      Object? lost;
      try {
        await for (final message in relayInbox(
          _socket!,
          answer: _answerAtArrival,
          media: _audioAtArrival,
          probe: _inboxProbe = _observabilityEnabled
              ? RelayInboxProbe(() => _clock.elapsedMicroseconds)
              : null,
          until: abandon.future,
        )) {
          _lastArrivalUs = _clock.elapsedMicroseconds;
          _silenceResumed = false;
          yield message;
        }
      } on WebSocketException catch (error) {
        lost = error;
      } on SocketException catch (error) {
        lost = error;
      } on HttpException catch (error) {
        lost = error;
      }
      if (_closed ||
          !await _resumeTransport(
            abandoned: abandon.isCompleted,
            failed: lost != null,
          )) {
        if (lost != null) throw lost;
        return;
      }
    }
  }

  /// session-resume-v1: dials the session again with the broker's single-use
  /// token. True: [_socket] is the new socket and the stream goes on (the
  /// broker restarts the picture at an IDR). False: the session ends as before;
  /// a socket the broker closed on purpose (with a reason) is never resumed.
  Future<bool> _resumeTransport({
    required bool abandoned,
    required bool failed,
  }) async {
    final token = _resumeToken, old = _socket;
    if (!_resumeEligible ||
        token == null ||
        (!abandoned && !failed && (old?.closeReason ?? '').isNotEmpty)) {
      return false;
    }
    // Spent by the attempt; the new socket's session.resumed brings the next.
    _resumeToken = null;
    _resumingTransport = true;
    _ping = null;
    notifyListeners();
    final deadline =
        _clock.elapsedMicroseconds + connection.resumeGraceMs * 1000;
    var wait = const Duration(milliseconds: 250);
    var replaced = false;
    try {
      while (!_closed && _clock.elapsedMicroseconds < deadline) {
        try {
          final socket = await _resumeDialer(
            connection.webSocketUrl,
            _token,
            token,
          );
          if (_closed) {
            unawaited(socket.close());
            return false;
          }
          socket.pingInterval = const Duration(seconds: 10);
          _socket = socket;
          // The broker already replaced the old socket; closing it is local.
          if (old != null) unawaited(old.close().catchError((Object _) {}));
          _resumeClocks();
          replaced = true;
          return true;
        } on WebSocketException catch (error) {
          // The broker answered (spent token, ended session): never again.
          if (error.httpStatusCode != null) return false;
        } catch (_) {
          // No network yet: try again within the window.
        }
        await Future<void>.delayed(wait);
        if (wait < const Duration(seconds: 2)) wait *= 2;
      }
      return false;
    } finally {
      _resumingTransport = false;
      if (!_closed) {
        if (replaced) _channelsAfterResume();
        notifyListeners();
      }
    }
  }

  /// The silence before a resume is not a stall of the stream.
  void _resumeClocks() {
    final now = _clock.elapsedMicroseconds;
    _lastMediaUs = _lastVideoUs = _lastDecodedUs = _lastArrivalUs = now;
    _ping = null;
    // Every output restarts at its own IDR (tracks-resume-v1).
    _tracks?.resumed(now);
  }

  /// Called by the receive inbox for each text message the moment it arrives
  /// (relay_inbox.dart): the answers that need no order with the media. They
  /// are timed on arrival, not when the reader gets to them behind the media
  /// pushes. Anything else (or anything malformed) takes the ordered path.
  bool _answerAtArrival(String message) {
    _lastArrivalUs = _clock.elapsedMicroseconds;
    _silenceResumed = false;
    if (_closed || message.length > 1024) return false;
    final pong = message.startsWith('{"type":"pong"');
    if (!pong && !message.startsWith('{"type":"input.ack"')) return false;
    final Object? event;
    try {
      event = jsonDecode(message);
    } catch (_) {
      return false;
    }
    if (event is! Map<String, dynamic>) return false;
    if (pong) {
      if (_ping != null && event['id'] == _ping) {
        rttMs = (_clock.elapsedMicroseconds - _pingUs) / 1000;
        _ping = null;
        notifyListeners();
      }
    } else if (event['sessionId'] == connection.sessionId) {
      _latency?.acknowledged(event, _clock.elapsedMicroseconds);
    }
    return true;
  }

  /// An audio packet at its arrival, while the reader may still wait for
  /// video room in the decoder (relayInbox): taken only when the reader would
  /// play it as it stands (same epoch, a decoder with its key frame, single
  /// output), with the same accounting. Audio is never a reason to end a
  /// session: a push that fails here (its engine was replaced meanwhile)
  /// drops that packet.
  bool _audioAtArrival(List<int> message) {
    if (_closed ||
        _tracks != null ||
        message.length < 13 ||
        message.length > 4 * 1024 * 1024 ||
        message[0] != 2 ||
        message[1] != 0 ||
        textureId == null ||
        _awaitingKeyFrame) {
      return false;
    }
    final wireEpoch = message[2] | (message[3] << 8);
    if (connection.epochMedia
        ? wireEpoch != _epoch || (_resize != null && !_resize!.decoderStarted)
        : wireEpoch != 0) {
      return false;
    }
    final arrivedUs = _clock.elapsedMicroseconds;
    // session-resume-v1: consumed here, it never reaches _resumableInbox.
    _lastArrivalUs = arrivedUs;
    _silenceResumed = false;
    if (adaptiveBitrate) _noteArrival(message, wireEpoch, arrivedUs);
    _lastMediaUs = arrivedUs;
    receivedBytes += message.length;
    if (!connection.options.audio) return true;
    final packet = message is Uint8List ? message : Uint8List.fromList(message);
    if (connection.epochMedia) {
      packet[2] = 0;
      packet[3] = 0;
    }
    _media.push(packet).then((_) {}, onError: (Object _) {});
    return true;
  }

  /// The native decode queue is bounded. Packets that waited in the network
  /// during a short stall arrive together, faster than they decode; the
  /// engine then answers `media_queue_full`. That is backpressure, not a
  /// failure (it used to end the stream as `media_backpressure`): wait for
  /// room and offer the same packet again. Awaiting here also pauses this
  /// socket reader, so the relay connection applies TCP flow control. Only a
  /// queue that stays full for [mediaQueueWait] ends the stream.
  Future<void> _pushMedia(Future<void> Function() push) async {
    Stopwatch? waiting;
    var delayMs = 1;
    while (true) {
      try {
        await push().timeout(const Duration(seconds: 5));
        return;
      } on PlatformException catch (error) {
        if (error.code != 'media_queue_full' || _closed) rethrow;
        if (waiting == null) {
          waiting = Stopwatch()..start();
          _mediaWaits++;
        }
        if (waiting.elapsed >= mediaQueueWait) {
          throw PlatformException(code: 'media_backpressure');
        }
        await Future<void>.delayed(Duration(milliseconds: delayMs));
        if (delayMs < 8) delayMs *= 2;
      }
    }
  }

  Future<void> _retireSigninRedirects() async {
    _signinRedirectOwner = null;
    _clipboardReconnectTimer?.cancel();
    _clipboardReconnectTimer = null;
    final oldClipboard = clipboard,
        oldDrive = drive,
        oldUsb = usb,
        oldPrint = printer;
    oldClipboard?.removeListener(_clipboardChanged);
    oldDrive?.removeListener(_driveChanged);
    oldUsb?.removeListener(_usbChanged);
    oldPrint?.removeListener(_printerChanged);
    _printRedial.reset();
    _driveRedial.reset();
    _usbRedial.reset();
    clipboard = null;
    drive = null;
    usb = null;
    printer = null;
    // A timeout is uncertainty, not permission to overlap a new owner.
    await Future.wait([
      if (oldClipboard != null)
        oldClipboard.close().timeout(const Duration(seconds: 6)),
      if (oldDrive != null)
        oldDrive.close().timeout(const Duration(seconds: 6)),
      if (oldUsb != null) oldUsb.close().timeout(const Duration(seconds: 6)),
      if (oldPrint != null)
        oldPrint.close().timeout(const Duration(seconds: 6)),
    ]);
  }

  Future<void> _signinPrepare(Map<String, dynamic> event) async {
    final phase = _signin!;
    if (!phase.prepare(event, _clock.elapsedMicroseconds)) return;
    final revision = phase.revision;
    _mediaRevision++;
    _captureRevision++;
    _signinPrivacyReady = false;
    _initialFramePending = true;
    _initialFrameDeadlineUs = null;
    _initialPreparationDeadlineUs = phase.preparationDeadlineUs;
    _retirePhaseInputGate();
    _discardPointerInput?.call();
    captured = false;
    _caretWatching = false;
    caretUpdates.value = null;
    final resize = _resize;
    _resize = null;
    if (resize != null && !resize.done.isCompleted) resize.done.complete(false);
    _viewportFrameTimer?.cancel();
    _releaseHeldPicture(now: true);
    _endComposeHold();
    _closeUdp();
    textureId = null; // Never hold the unlocked desktop over a locked screen.
    status = l10n.preparingSwitchWindowsDisplay;
    notifyListeners();
    if (_media is! RemoteThumbnailPrivacy) {
      throw PlatformException(code: 'thumbnail_privacy_unknown');
    }
    await Future.wait([
      _retireSigninRedirects(),
      _media.capture(false).timeout(const Duration(seconds: 2)),
      (_media as RemoteThumbnailPrivacy).clearThumbnail().timeout(
        const Duration(seconds: 2),
      ),
    ]);
    if (_closed || phase.closed || revision != phase.revision) return;
    _inputObservation.observe({'type': 'input.reset'});
    await _media.stop().timeout(const Duration(seconds: 2));
  }

  Future<void> _signinInfo(Map<String, dynamic> event) async {
    final phase = _signin!,
        frame = phase.acceptInfo(event, _clock.elapsedMicroseconds);
    if (frame == null) return;
    final revision = phase.revision;
    if (frame.resize) {
      if ((clipboard != null ||
              drive != null ||
              usb != null ||
              printer != null) &&
          !phase.retainsRedirects(_signinRedirectOwner)) {
        throw const FormatException('Retired desktop channel owner.');
      }
      _mediaRevision++;
      _captureRevision++;
      _retirePhaseInputGate();
      _discardPointerInput?.call();
      captured = false;
      _caretWatching = false;
      caretUpdates.value = null;
      _inputObservation.observe({'type': 'input.reset'});
      await _media.capture(false).timeout(const Duration(seconds: 2));
      if (_closed || phase.closed || revision != phase.revision) return;
    }
    _epoch = frame.binding.mediaScope.epoch;
    _initialFramePending = true;
    _signinPrivacyReady = false;
    var decoderStarted = false;
    await _control(
      frame.info,
      phaseInfo: true,
      onPhaseDecoderStarted: () => decoderStarted = true,
    );
    if (_closed || phase.closed || revision != phase.revision) return;
    if (!decoderStarted) {
      throw const FormatException('Uncorrelated sign-in decoder metadata.');
    }
    if (_media is! RemoteThumbnailPrivacy) {
      throw PlatformException(code: 'thumbnail_privacy_unknown');
    }
    // A fresh decoder owns a fresh native thumbnail lease. Retire that target
    // before the reader can accept even its first sign-in pixel packet.
    await (_media as RemoteThumbnailPrivacy).clearThumbnail().timeout(
      const Duration(seconds: 2),
    );
    if (_closed || phase.closed || revision != phase.revision) return;
    _signinPrivacyReady = true;
    status = phase.binding.phase == SigninPhase.signin
        ? l10n.preparingWindowsSignScreen
        : l10n.preparingWindowsDesktop;
    notifyListeners();
  }

  Future<void> _control(
    Map<String, dynamic> event, {
    bool phaseInfo = false,
    void Function()? onPhaseDecoderStarted,
  }) async {
    if (_signin != null && !phaseInfo) {
      if (event['type'] == 'stream.phase.prepare') {
        await _signinPrepare(event);
        return;
      }
      if (event['type'] == 'stream.phase.info') {
        await _signinInfo(event);
        return;
      }
      if (event['type'] == 'stream.info') {
        throw const FormatException('Unwrapped sign-in metadata.');
      }
    }
    if (_observabilityEnabled &&
        connection.observability &&
        !resizing &&
        event['sessionId'] == connection.sessionId &&
        ((event['type'] == 'stream.stats' &&
                _tracks == null &&
                event['epoch'] == _epoch) ||
            (event['type'] == 'stream.tracks.stats' &&
                _tracks != null &&
                event['topologyEpoch'] == _tracks.topology))) {
      _hostObservation = parseHostObservation(event['observability']);
      _hostObservationAtUs = _clock.elapsedMicroseconds;
      _hostObservationEpoch = _tracks?.topology ?? _epoch;
      if (_tracks != null && event['tracks'] is List) {
        _trackHostObservation.clear();
        for (final raw in event['tracks'] as List) {
          final value = parseOutputObservation(raw);
          if (_tracks.tracks.any(
            (t) => t.id == value['id'] && t.epoch == value['epoch'],
          )) {
            _trackHostObservation[value['id'] as String] = value;
          }
        }
      }
    }
    switch (event['type']) {
      case 'stream.tracks.info':
        final outputs = _tracks;
        if (outputs == null) {
          throw const FormatException('Unnegotiated independent outputs.');
        }
        if (_initialPreparationDeadlineUs != null &&
            _clock.elapsedMicroseconds > _initialPreparationDeadlineUs!) {
          diagnosticCode = 'initial_track_viewport_timeout';
          await _fail(l10n.virtualDisplayTookTooLong);
          return;
        }
        final waitingOutput = _outputChange;
        if (waitingOutput != null &&
            _clock.elapsedMicroseconds > waitingOutput.deadlineUs) {
          diagnosticCode = 'track_resize_timeout';
          endedByDisplayChange = true;
          await _fail(l10n.couldntConfirmVirtualDisplayChange);
          return;
        }
        final restored = outputs.restoreReason;
        if (!outputs.configure(event)) return;
        _closeTrackUdp();
        // The decoder budget starts at metadata receipt, not after a slow
        // native start. Metadata cannot renew an expired hardware deadline.
        final outputMetadataAt = _clock.elapsedMicroseconds;
        connectTiming.mark('info', outputMetadataAt);
        if (waitingOutput != null) {
          waitingOutput.deadlineUs = outputMetadataAt + 15000000;
        }
        _mediaRevision++;
        _trackDecoderStarting = true;
        _videoConfiguration(
          outputs.primary.width,
          outputs.primary.height,
          outputs.fps,
        );
        final textures = await (_media as RemoteTrackMedia)
            .startTracks(outputs.nativeConfiguration)
            .timeout(const Duration(seconds: 5));
        if (_closed) {
          try {
            await _media.stop().timeout(const Duration(seconds: 2));
          } catch (_) {}
          return;
        }
        outputs.bindTextures(textures, _clock.elapsedMicroseconds);
        connectTiming.mark('decoder', _clock.elapsedMicroseconds);
        _initialPreparationDeadlineUs = null;
        _trackDecoderStarting = false;
        final outputChange = _outputChange;
        if (outputChange != null) {
          outputChange.restored = restored != null;
          outputChange.metadataReady = true;
        }
        final primary = outputs.primary;
        textureId = textures[primary.id];
        width = sourceWidth = primary.width;
        height = sourceHeight = primary.height;
        appliedFps = outputs.fps;
        appliedBitrate = outputs.tracks.fold<int>(
          0,
          (sum, t) => sum + t.bitrate,
        );
        _lastMediaUs = _lastVideoUs = _lastDecodedUs = _lastStatsUs =
            _clock.elapsedMicroseconds;
        _lastStatsBytes = receivedBytes;
        audioStatus = outputs.audio ? l10n.waiting2 : l10n.off;
        status = l10n.preparingRemoteScreen;
        notifyListeners();
      case 'stream.tracks.resizing':
        final outputs = _tracks;
        if (outputs == null || !connection.profileTracks) {
          throw const FormatException('Unnegotiated output resize.');
        }
        if (outputs.acceptResizing(event, _clock.elapsedMicroseconds)) {
          _closeTrackUdp();
          status = l10n.resizingVirtualDisplay;
          notifyListeners();
        }
      case 'stream.tracks.resize.restored':
        final outputs = _tracks;
        if (outputs == null || !connection.profileTracks) {
          throw const FormatException('Unnegotiated output restore.');
        }
        if (outputs.acceptRestored(event, _clock.elapsedMicroseconds)) {
          status = l10n.restoringPreviousScreenSize;
          notifyListeners();
        }
      case 'stream.tracks.resize.failed':
        final outputChange = _outputChange;
        if (outputChange == null ||
            event['requestId'] != outputChange.requestId) {
          return;
        }
        if (event['unchanged'] == true && _tracks!.rejectUnchanged(event)) {
          _outputChange = null;
          resizeError =
              resolutionFailureMessage(event['reason'], unchanged: true) ??
              keptSizeMessage;
          if (!outputChange.done.isCompleted) outputChange.done.complete(false);
          status = l10n.connected;
          notifyListeners();
        } else {
          diagnosticCode = 'track_resize_failed';
          endedByDisplayChange = true;
          await _fail(l10n.couldntResizeVirtualDisplayReconnect);
        }
      case 'stream.info':
        connectTiming.mark('info', _clock.elapsedMicroseconds);
        connectTiming.host(event['startStages']);
        if (_initialPreparationDeadlineUs != null &&
            _clock.elapsedMicroseconds > _initialPreparationDeadlineUs!) {
          diagnosticCode = 'initial_viewport_timeout';
          await _fail(l10n.couldntPrepareVirtualDisplayResolution);
          return;
        }
        if (_tracks != null) {
          throw const FormatException('Unexpected composite stream.');
        }
        final w = event['width'], h = event['height'];
        if (event['codec'] != 'h264' ||
            w is! int ||
            h is! int ||
            !encodedPictureSize(w, h, portrait: streamSize)) {
          throw const FormatException();
        }
        // audio-opus-v1: Opus only where this connection was granted it.
        final audioOpus = streamAudioOpus(
          event['audio'],
          granted: connection.audioOpus,
        );
        final infoEpoch = event['epoch'] ?? 0;
        if (infoEpoch is! int || infoEpoch < 0 || infoEpoch > 65535) {
          throw const FormatException();
        }
        final pending = _resize;
        if (infoEpoch == _epoch) _udp?.stream(_epoch);
        if (!phaseInfo &&
            streamSize &&
            pending == null &&
            textureId != null &&
            infoEpoch == _epoch &&
            infoEpoch > 0 &&
            event['sessionId'] == connection.sessionId) {
          await _rescale(event, w, h);
          return;
        }
        if (connection.epochMedia &&
            (infoEpoch < _epoch ||
                (!phaseInfo && textureId != null && infoEpoch == _epoch))) {
          return;
        }
        if (!phaseInfo &&
            pending == null &&
            textureId == null &&
            connection.initialViewport != null &&
            !connection.initialViewport!.matchesInfo(
              event,
              connection.options.maxWidth,
              connection.options.maxHeight,
            )) {
          diagnosticCode = 'initial_viewport_mismatch';
          await _fail(l10n.virtualDisplayOpenedDifferentResolution);
          return;
        }
        if (pending != null) {
          if (event['sessionId'] != connection.sessionId ||
              event['requestId'] != pending.requestId ||
              event['resizeId'] != pending.resizeId ||
              infoEpoch != pending.epoch) {
            return;
          }
          final geometryMatches = connection.viewportEncoderLimits
              ? w >= 16 &&
                    h >= 16 &&
                    w <= pending.width &&
                    h <= pending.height &&
                    w <=
                        (pending.decoderLimits?.maxWidth ??
                            (connection.streamSizeV2
                                ? _activeDecoderLimits!.maxWidth
                                : connection.options.maxWidth)) &&
                    h <=
                        (pending.decoderLimits?.maxHeight ??
                            (connection.streamSizeV2
                                ? _activeDecoderLimits!.maxHeight
                                : connection.options.maxHeight)) &&
                    event['sourceWidth'] == pending.width &&
                    event['sourceHeight'] == pending.height
              : w == pending.width &&
                    h == pending.height &&
                    event['sourceWidth'] == w &&
                    event['sourceHeight'] == h;
          if (!geometryMatches ||
              event['fpsTarget'] != connection.options.fps) {
            throw const FormatException('Resize geometry mismatch.');
          }
        } else if ((!phaseInfo && textureId != null) ||
            infoEpoch !=
                (phaseInfo
                    ? _signin!.binding.mediaScope.epoch
                    : (connection.initialViewport != null ? 1 : _epoch))) {
          throw const FormatException('Unexpected stream initialization.');
        }
        final nextLimits = pending?.decoderLimits ?? _activeDecoderLimits;
        if (nextLimits != null &&
            !nextLimits.fits(w, h, connection.options.fps)) {
          throw const FormatException('Decoder size mismatch.');
        }
        if (pending?.decoderLimits != null) {
          _activeDecoderLimits = pending!.decoderLimits;
        }
        if (pending != null) {
          pending.infoUs = _clock.elapsedMicroseconds;
          pending.hostStages = hostResizeStages(event['resizeStages']);
        }
        width = w;
        height = h;
        sourceWidth = (event['sourceWidth'] as num?)?.toInt();
        sourceHeight = (event['sourceHeight'] as num?)?.toInt();
        appliedFps = (event['fpsTarget'] as num?)?.toInt();
        appliedBitrate = (event['bitrate'] as num?)?.toInt();
        final backend = event['captureBackend'];
        captureBackend = ['wgc-warp', 'wgc-d3d11', 'gdi'].contains(backend)
            ? backend as String
            : null;
        _pictureChanging = false;
        _readEncodeScale(event);
        hostTelemetry = null;
        _hostTelemetryUs = null;
        capturedFps = encodedFps = decodedFps = receivedMbps = textureFps =
            null;
        clientDecodeMs = clientDecodeMaxMs = clientQueueMs = null;
        clientQueuePackets = skippedDecodedFrames = null;
        textureWaitMs = textureUploadMs = null;
        textureQueuedFrames = skippedFramesTick = null;
        _lastTextures = _lastDecodeCalls = 0;
        _lastDecodeTotalMs = _lastDecodeBusyMs = 0;
        _lastUploads = _lastQueued = _lastSkipped = 0;
        _stallWatch.restart();
        _lastWaitTotalMs = _lastUploadTotalMs = 0;
        _lastAudioDropped = 0;
        _audioStats.reset();
        _mediaRevision++;
        _latency?.resetFrames();
        _discardPointerInput?.call();
        if (!connection.options.audio) {
          audioStatus = l10n.off;
        } else if (event['audio'] is Map &&
            (event['audio'] as Map)['available'] == false) {
          // The host found no playback device to capture: a Proxmox VM
          // without an audio device has nothing to send.
          audioStatus = remoteAudioMissingStatus;
          remoteAudioMissing = true;
        }
        _videoConfiguration(
          sourceWidth ?? w,
          sourceHeight ?? h,
          appliedFps ?? connection.options.fps,
        );
        if (infoEpoch != _epoch) _udp?.stream(infoEpoch);
        final media = _media;
        _streamAudioOpus = audioOpus;
        if (media case final AudioFormatMedia player) {
          player.audioOpus = audioOpus;
        }
        final texture =
            await (pending?.heldTexture != null && media is HeldPictureMedia
                    ? (media as HeldPictureMedia).startHolding(w, h)
                    : media.start(w, h))
                .timeout(const Duration(seconds: 5));
        pending?.decoderUs = _clock.elapsedMicroseconds;
        if (_closed) {
          try {
            await _media.stop().timeout(const Duration(seconds: 2));
          } catch (_) {}
          return;
        }
        textureId = texture;
        connectTiming.mark('decoder', _clock.elapsedMicroseconds);
        _initialPreparationDeadlineUs = null;
        _epoch = infoEpoch;
        if (phaseInfo) onPhaseDecoderStarted?.call();
        _awaitingKeyFrame = true;
        _lastFrames = 0;
        _lastMediaUs = _clock.elapsedMicroseconds;
        _lastVideoUs = _lastDecodedUs = _lastMediaUs;
        if (pending != null) {
          // An accepted geometry and a registered texture are not a frame.
          // Feed this epoch while keeping input and further resize requests
          // paused until the fresh decoder has published actual RGBA output.
          pending.decoderStarted = true;
          pending.deadlineUs = _lastMediaUs + 15000000;
          status = l10n.preparingScreenNewResolution;
        } else {
          _initialFrameDeadlineUs = _lastMediaUs + 15000000;
          status = l10n.preparingFirstFrame;
        }
        _lastStatsUs = _clock.elapsedMicroseconds;
        _lastStatsBytes = receivedBytes;
        notifyListeners();
      case 'stream.resizing':
        if (_tracks != null) {
          throw const FormatException('Unexpected composite resize.');
        }
        final pending = _resize;
        if (!connection.epochMedia ||
            pending == null ||
            event['requestId'] != pending.requestId) {
          return;
        }
        final epoch = event['epoch'],
            resizeId = event['resizeId'],
            timeout = event['timeoutMs'] ?? 15000;
        if (epoch is! int ||
            epoch <= _epoch ||
            epoch > 65535 ||
            resizeId is! String ||
            !RegExp(r'^[a-zA-Z0-9-]{1,80}$').hasMatch(resizeId) ||
            event['width'] != pending.width ||
            event['height'] != pending.height ||
            timeout is! int ||
            timeout < 1 ||
            timeout > 90000 ||
            event['monitor'] != connection.options.monitor ||
            event['fps'] != connection.options.fps) {
          throw const FormatException('Invalid resize acknowledgement.');
        }
        if (pending.resizeId != null) return;
        pending.resizeId = resizeId;
        pending.epoch = epoch;
        pending.ackUs = _clock.elapsedMicroseconds;
        pending.deadlineUs = _clock.elapsedMicroseconds + timeout * 1000;
        _mediaRevision++;
        _latency?.resetFrames();
        await capture(false);
        final media = _media;
        if (media is HeldPictureMedia &&
            (media as HeldPictureMedia).holdsPicture &&
            textureId != null &&
            !_closed) {
          // The previous picture stays, scaled to the window, until the new
          // size's first frame; its engine receives nothing meanwhile.
          _holdPicture(pending);
        } else {
          await _media.stop();
          textureId = null;
        }
        status = l10n.changingRemoteScreenResolution;
        notifyListeners();
      case 'stream.resize.failed':
        final pending = _resize;
        if (pending == null || event['requestId'] != pending.requestId) return;
        resizeError = event['unchanged'] == true && pending.resizeId == null
            ? resolutionFailureMessage(event['reason'], unchanged: true) ??
                  keptSizeMessage
            : keptSizeMessage;
        _resize = null;
        if (!pending.done.isCompleted) pending.done.complete(false);
        if (event['unchanged'] != true || pending.resizeId != null) {
          // The relay's fixed reason: without it an SDK resize failure is
          // recorded only as unknown_failure.
          final reason = event['reason'];
          diagnosticCode = reason is String && diagnosticCodes.contains(reason)
              ? reason
              : 'resize_failed';
          // A dispatched change that failed ends the relay session.
          endedByDisplayChange = true;
          await _fail(l10n.couldntResizeRemoteScreenSo);
          return;
        }
        status = l10n.connected;
        notifyListeners();
      case 'stream.tracks.stats':
        // tracks-v1: the Host's capture/encode rates per output (statistics
        // and diagnostics used to show none for 모든 모니터 사용). Summed like
        // the decoded rate; the lowest output kept apart.
        final outputs = _tracks;
        if (outputs == null ||
            resizing ||
            event['sessionId'] != connection.sessionId ||
            event['topologyEpoch'] != outputs.topology) {
          return;
        }
        final telemetry = HostStreamTelemetry.parseTracks(event);
        if (telemetry == null) return;
        hostTelemetry = telemetry;
        _hostTelemetryUs = _clock.elapsedMicroseconds;
        capturedFps = telemetry.capturedFps;
        encodedFps = telemetry.encodedFps;
        notifyListeners();
      case 'stream.stats':
        if (resizing ||
            (event['sessionId'] != null &&
                event['sessionId'] != connection.sessionId) ||
            (event['epoch'] != null && event['epoch'] != _epoch) ||
            (connection.epochMedia &&
                event['pipeline'] != null &&
                (event['epoch'] != _epoch ||
                    event['sessionId'] != connection.sessionId))) {
          return;
        }
        final telemetry = HostStreamTelemetry.parse(event);
        if (telemetry == null) return;
        hostTelemetry = telemetry;
        _audioStats.host(event);
        _stallWatch.host(event, _clock.elapsedMicroseconds);
        final fpsTarget = event['fpsTarget'];
        hostFpsTarget = fpsTarget is int && fpsTarget > 0 && fpsTarget <= 240
            ? fpsTarget
            : null;
        if (_observabilityEnabled) {
          _lastEncoderFacts
            ..clear()
            ..addAll({
              'encoderBackend':
                  telemetry.encoder?.backend ?? telemetry.legacyEncoder,
              'encoderGpuFallbackCode': telemetry.encoder?.gpuFallbackCode,
              'encoderSoftwareFallbackCode':
                  telemetry.encoder?.softwareFallbackCode,
              'encoderObservedAtMs': _clock.elapsedMicroseconds ~/ 1000,
              'encoderObservedEpoch': _epoch,
            });
        }
        _readEncodeScale(event);
        _hostTelemetryUs = _clock.elapsedMicroseconds;
        capturedFps = telemetry.capturedFps;
        encodedFps = telemetry.encodedFps;
        // ToTBSeamless lossless tiles: how much of the picture
        // is currently pixel-exact and what the tiles cost.
        final lossless = event['lossless'];
        if (lossless is Map && lossless['protocol'] == 'tiles-v1') {
          final tiles = lossless['presentTiles'], kbps = lossless['kbps'];
          losslessTiles = tiles is int && tiles >= 0 ? tiles : null;
          losslessKbps = kbps is num && kbps.isFinite && kbps >= 0
              ? kbps.toDouble()
              : null;
        } else {
          losslessTiles = losslessKbps = null;
        }
        notifyListeners();
      case 'pong':
        if (_ping != null && event['id'] == _ping) {
          rttMs = (_clock.elapsedMicroseconds - _pingUs) / 1000;
          _ping = null;
          notifyListeners();
        }
      // input-latency-v1 (only relayed for a granted connection; otherwise ignored).
      case 'input.ack':
        if (event['sessionId'] == connection.sessionId) {
          _latency?.acknowledged(event, _clock.elapsedMicroseconds);
        }
      case 'input.frame':
        if (event['sessionId'] == connection.sessionId &&
            event['epoch'] == _epoch &&
            !resizing &&
            textureId != null) {
          _latency?.tagged(event, _clock.elapsedMicroseconds);
        }
      // caret-v1 (only relayed for a granted connection).
      case 'caret':
        if (!caretSupported || event['sessionId'] != connection.sessionId) {
          return;
        }
        final caret = RemoteCaret.parse(event);
        if (caret == null) throw const FormatException();
        if (_caretWatching) {
          caretUpdates.value = caret.stamped(_clock.elapsed);
        }
      // display-scale-v1 (only relayed for a granted connection).
      case 'display.scale.applied':
        if (!displayScaleSupported ||
            event['sessionId'] != connection.sessionId) {
          return;
        }
        final applied = DisplayScaleApplied.parse(
          event,
          outputs: _tracks != null,
        );
        if (applied == null) throw const FormatException();
        displayScaleApplied = applied;
        notifyListeners();
      case 'session.ended':
        final failure = SessionFailure.ended(event['reason']);
        diagnosticCode = relayEndCode(
          event['reason'],
          failure.code,
          event['detail'],
        );
        endedByHostLoss = failure.code == 'host_disconnected';
        canRetry = failure.canRetry;
        // Who took it, when the server says (2026-11-09 edge-case audit).
        status = failure.code == 'taken_over'
            ? takenOverMessage(event['detail'], event['by'])
            : failure.message;
        await close(notifyHost: false);
      case 'stream.unavailable':
        if (event['sessionId'] != connection.sessionId) {
          throw const FormatException();
        }
        final failure = SessionFailure.ended(event['reason']);
        diagnosticCode = relayEndCode(event['reason'], failure.code);
        canRetry = failure.canRetry;
        await _fail(failure.message);
      case 'udp.tracks.grant':
        _startTrackUdp(event);
      case 'udp.tracks.key':
        final scope = _udpScopes[event['track']];
        if (scope != null &&
            scope.matches(event) &&
            event['sessionId'] == connection.sessionId) {
          _udpTracks[scope.track]?.hostKey(
            event,
            hasKeyFrame: _tracks?.ready == true,
          );
        }
      case 'udp.tracks.peer':
        final scope = _udpScopes[event['track']];
        if (scope != null && scope.matches(event)) {
          _udpTracks[scope.track]?.peer(event);
        }
      case 'udp.grant':
        _startUdp(event);
      case 'udp.key':
        _udpHostKey(event);
      case 'udp.peer':
        _udp?.peer(event);
      // session-resume-v1 (only sent to a session that negotiated it): the
      // token for the next resume, and the Host leaving and coming back.
      case 'session.resume.token' || 'session.resumed':
        if (!connection.sessionResume ||
            event['sessionId'] != connection.sessionId) {
          return;
        }
        _resumeToken = resumeToken(event['token']) ?? _resumeToken;
        // channel-resume-v1: granted with the first token, for the session.
        if (channelResumeGranted(event)) _channelResume = true;
        if (event['type'] == 'session.resumed' && _hostResuming) {
          _hostResuming = false;
          _resumeClocks();
          _channelsAfterResume();
          notifyListeners();
        }
      case 'session.resuming':
        if (!connection.sessionResume ||
            event['sessionId'] != connection.sessionId) {
          return;
        }
        _hostResuming = true;
        // The broker ends the session if the Host does not return; this only
        // bounds a broker that never says so.
        _resumeBackstopUs = _clock.elapsedMicroseconds + 75000000;
        _ping = null;
        notifyListeners();
      // notice-v1: see [onBrokerNotices].
      case 'notices':
        onBrokerNotices?.call();
    }
  }

  /// udp-media-v1: a leg beside the stream, started and never awaited. A
  /// malformed grant, or one this session did not ask for, changes nothing. The
  /// Broker grants once per session, so a repeat changes nothing either.
  void _startUdp(Map<String, dynamic> event) {
    if (!connection.udpMedia || _tracks != null || _closed || _udp != null) {
      return;
    }
    try {
      final grant = UdpGrant.parse(event);
      if (grant == null) return;
      final udp = _udp = UdpMedia(
        grant,
        udpLegFactory?.call() ?? NativeUdpLeg(),
        sessionId: connection.sessionId,
        nowUs: () => _clock.elapsedMicroseconds,
        mediaAllowed: adaptiveBitrate && _media is UdpPathMedia,
        sendKey: (key) => _send({
          'type': 'udp.key',
          'protocol': udpMediaProtocol,
          'publicKey': base64.encode(key),
        }),
        sendLocal: (address, port) => _send({
          'type': 'udp.local',
          'protocol': udpDirectProtocol,
          'address': address,
          'port': port,
        }),
        sendReflexive: (address, port) => _send({
          'type': 'udp.local',
          'protocol': udpDirectProtocol,
          'candidateType': 'srflx',
          'address': address,
          'port': port,
        }),
        inputSequence: _directInputSequence,
        delivered: _udpDelivered,
        keyed: () => _awaitingKeyFrame = false,
      );
      udp.start(_epoch);
    } catch (_) {}
  }

  UdpMedia? _inputLeg(Map<String, dynamic> event) {
    if (_signin != null) {
      return null; // No unstamped/direct input in this contract.
    }
    if (_tracks == null) return _udp;
    final id = event['track'];
    if (id is String) return _udpTracks[id];
    for (final scope in _udpScopes.values) {
      if (scope.primary) return _udpTracks[scope.track];
    }
    return null;
  }

  void _startTrackUdp(Map<String, dynamic> event) {
    final outputs = _tracks;
    if (_closed ||
        !connection.udpTracks ||
        outputs == null ||
        !outputs.ready ||
        outputs.preparing ||
        event['protocol'] != udpTracksProtocol ||
        event['sessionId'] != connection.sessionId ||
        event['topologyEpoch'] != outputs.topology) {
      return;
    }
    final generation = event['generation'], rows = event['tracks'];
    if (generation is! int ||
        generation <= _udpTrackGeneration ||
        generation > 0x7fffffff ||
        rows is! List ||
        rows.length != outputs.tracks.length) {
      return;
    }
    final parsed = <(UdpTrackScope, UdpGrant)>[];
    final seen = <String>{};
    for (final row in rows) {
      if (row is! Map<String, dynamic>) return;
      final track = outputs.tracks
          .where((t) => t.id == row['track'])
          .firstOrNull;
      final raw = row['grant'];
      if (track == null ||
          !seen.add(track.id) ||
          row['epoch'] != track.epoch ||
          track.epoch < 1 ||
          track.epoch > 65535 ||
          row['primary'] != track.primary ||
          row['bitrateTarget'] != track.bitrate ||
          raw is! Map<String, dynamic>) {
        return;
      }
      final grant = UdpGrant.parse(raw);
      if (grant == null || !grant.keys) return;
      parsed.add((
        UdpTrackScope(
          track.id,
          outputs.topology,
          track.epoch,
          generation,
          track.primary,
        ),
        grant,
      ));
    }
    _closeTrackUdp();
    _udpTrackGeneration = generation;
    for (final (scope, grant) in parsed) {
      _udpScopes[scope.track] = scope;
      void control(String type, String protocol, Map<String, Object> values) =>
          _send({
            'type': type,
            'protocol': protocol,
            'sessionId': connection.sessionId,
            ...scope.fields,
            ...values,
          });
      final leg = UdpMedia(
        grant,
        udpLegFactory?.call() ?? NativeUdpLeg(scope: scope),
        sessionId: connection.sessionId,
        nowUs: () => _clock.elapsedMicroseconds,
        mediaAllowed: _media is RemoteTrackMedia,
        inputSequence: _directInputSequence,
        sendKey: (key) => control('udp.tracks.key', udpMediaProtocol, {
          'publicKey': base64.encode(key),
        }),
        sendLocal: (address, port) => control(
          'udp.tracks.local',
          udpDirectProtocol,
          {'address': address, 'port': port},
        ),
        sendReflexive: (address, port) => control(
          'udp.tracks.local',
          udpDirectProtocol,
          {'address': address, 'port': port, 'candidateType': 'srflx'},
        ),
        delivered: (reading) => _trackUdpDelivered(scope, reading),
        keyed: () {},
      );
      _udpTracks[scope.track] = leg;
      leg.start(scope.epoch);
    }
  }

  void _trackUdpDelivered(UdpTrackScope scope, UdpLegReading reading) {
    if (_closed || !identical(_udpScopes[scope.track], scope)) return;
    final now = _clock.elapsedMicroseconds;
    final arrivals = reading.arrivals;
    // Only real video admission may renew this output's video watchdog.
    if (arrivals.isNotEmpty &&
        _tracks?.observeUdp(scope.track, scope.epoch, now) != true) {
      return;
    }
    final videoBytes = arrivals.fold<int>(0, (sum, frame) => sum + frame.$3);
    receivedBytes += videoBytes + reading.audioBytes;
    if (videoBytes + reading.audioBytes > 0) _lastMediaUs = now;
    if (arrivals.isEmpty) return;
    _lastVideoUs = now;
    _send({
      'type': 'udp.tracks.feedback',
      'protocol': udpTracksProtocol,
      'topologyEpoch': scope.topology,
      'generation': scope.generation,
      'tracks': [
        {
          'track': scope.track,
          'epoch': scope.epoch,
          'pts': arrivals.last.$1,
          'frames': arrivals.length.clamp(1, 240),
          'bytes': videoBytes.clamp(1, 64 * 1024 * 1024),
        },
      ],
    });
  }

  void _udpHostKey(Map<String, dynamic> event) {
    try {
      final hasKeyFrame = textureId != null && !_awaitingKeyFrame;
      if (_udp?.hostKey(event, hasKeyFrame: hasKeyFrame) != true) return;
      (_media as UdpPathMedia).udpPathRule = true;
    } catch (_) {}
  }

  void _closeTrackUdp() {
    for (final leg in _udpTracks.values) {
      leg.close();
    }
    _udpTracks.clear();
    _udpScopes.clear();
  }

  void _closeUdp() {
    _closeTrackUdp();
    _udp?.close();
    final media = _media;
    if (media is UdpPathMedia) (media as UdpPathMedia).udpPathRule = false;
  }

  /// What the leg gave the decoder since its last reading gets the accounting a
  /// WebSocket packet gets. Only delivered media counts: frames the path rule
  /// dropped must not keep a frozen picture past the video watchdog.
  void _udpDelivered(UdpLegReading reading) {
    if (_closed) return;
    final now = _clock.elapsedMicroseconds;
    if (reading.audioBytes > 0) {
      receivedBytes += reading.audioBytes;
      _lastMediaUs = now;
      if (adaptiveBitrate) _noteFeedback(_epoch, reading.audioBytes, null, now);
    }
    if (reading.arrivals.isEmpty) return;
    final latency = _latency;
    for (final (pts, arrivedUs, bytes) in reading.arrivals) {
      receivedBytes += bytes;
      if (adaptiveBitrate) _noteFeedback(_epoch, bytes, pts, arrivedUs);
      _noteVideoArrival(arrivedUs);
      _composeVideo(pts);
      if (latency != null && latency.watch(pts, arrivedUs, bytes: bytes)) {
        latency.pushed(pts, arrivedUs);
      }
    }
    _lastMediaUs = _lastVideoUs = now;
    if (_initialFramePending) unawaited(_poll(readinessOnly: true));
    _scheduleViewportFrameCheck();
  }

  Future<void> _poll({bool readinessOnly = false}) async {
    if (_polling || _closed) return;
    _polling = true;
    final revision = _mediaRevision;
    final observationEpoch = _androidObservation.epoch;
    try {
      final now = _clock.elapsedMicroseconds;
      if (!frameReady || resizing) _observeSnapshot(now);
      // session-resume-v1: while the socket or the Host comes back nothing is
      // a stall. A socket silent for resumeSilenceUs is replaced once (until
      // something arrives again); the broker keeps the stream meanwhile.
      if (_resumingTransport || _hostResuming) {
        if (_hostResuming && now > _resumeBackstopUs) {
          diagnosticCode = 'host_timeout';
          await _fail(l10n.hostStoppedRespondingSoConnection);
        }
        return;
      }
      final abandon = _abandonSocket;
      if (abandon != null &&
          !abandon.isCompleted &&
          !_silenceResumed &&
          _resumeEligible &&
          now - _lastArrivalUs > resumeSilenceUs) {
        _silenceResumed = true;
        abandon.complete();
        return;
      }
      final preparing = _initialPreparationDeadlineUs;
      if (preparing != null && textureId == null) {
        if (now > preparing) {
          diagnosticCode = 'initial_viewport_timeout';
          await _fail(l10n.couldntPrepareVirtualDisplayResolution);
        }
        // A bounded hardware transaction precedes capture. WebSocket-level
        // liveness remains active; media/ping deadlines start after metadata.
        return;
      }
      if (_tracks == null &&
          _initialFramePending &&
          _resize == null &&
          _initialFrameDeadlineUs != null &&
          now > _initialFrameDeadlineUs!) {
        diagnosticCode = 'initial_frame_timeout';
        await _fail(l10n.didntReceiveFirstFrameReconnect);
        return;
      }
      final pending = _resize;
      final outputChange = _outputChange;
      if (outputChange != null && now > outputChange.deadlineUs) {
        diagnosticCode = 'track_resize_timeout';
        endedByDisplayChange = true;
        await _fail(l10n.couldntConfirmVirtualDisplayChange);
        return;
      }
      if (pending != null && now > pending.deadlineUs) {
        await _viewportTimedOut(pending);
        return;
      }
      if ((!resizing &&
              _androidObservation.stalled(
                now,
                _lastMediaUs,
                eligible: _androidObservationEligible,
              )) ||
          (_ping != null &&
              _androidObservation.stalled(
                now,
                _pingUs,
                eligible: _androidObservationEligible,
              ))) {
        diagnosticCode = 'host_timeout';
        await _fail(l10n.hostStoppedRespondingSoConnection);
        return;
      }
      if (!resizing &&
          _androidObservation.stalled(
            now,
            _lastVideoUs,
            eligible: _androidObservationEligible,
          )) {
        diagnosticCode = 'video_timeout';
        await _fail(l10n.screenStoppedArrivingSoConnection);
        return;
      }
      if (_tracks?.videoStalled(now) == true) {
        diagnosticCode = 'output_video_timeout';
        await _fail(l10n.oneMonitorsScreenStoppedArriving);
        return;
      }
      final tiles = _losslessTilesSession;
      // A tiles-capable session offers its opt-in with the first media rather
      // than waiting for the periodic ping. No ping while the remote display
      // changes: older Hosts read the opt-in it carries (or, for
      // independent outputs, the ping itself) inside their display-change
      // RPC wait and fail the change, which ends the stream as
      // display_change_failed during rapid window/fullscreen changes.
      // The relay socket keeps its own keep-alive meanwhile.
      if (_ping == null &&
          !resizing &&
          (!readinessOnly ||
              (tiles && !_losslessOffered && textureId != null))) {
        _losslessOffered = _losslessOffered || tiles;
        _sendPing(now);
      }
      if (_tracks?.preparing == true || _trackDecoderStarting) return;
      if (textureId != null) {
        final stats = await _media.stats().timeout(const Duration(seconds: 3));
        if (_closed ||
            (_signin != null && !_signin.active && !_signinPrivacyReady) ||
            !_androidObservation.current(observationEpoch) ||
            (_resize != null && !_resize!.decoderStarted) ||
            _tracks?.preparing == true ||
            _trackDecoderStarting ||
            revision != _mediaRevision) {
          return;
        }
        final statsAt = _clock.elapsedMicroseconds;
        final trackDeadline = _outputChange;
        if (trackDeadline != null && statsAt > trackDeadline.deadlineUs) {
          diagnosticCode = 'track_resize_timeout';
          endedByDisplayChange = true;
          await _fail(l10n.couldntConfirmVirtualDisplayChange);
          return;
        }
        if (_tracks == null &&
            _initialFramePending &&
            _resize == null &&
            _initialFrameDeadlineUs != null &&
            statsAt > _initialFrameDeadlineUs!) {
          diagnosticCode = 'initial_frame_timeout';
          await _fail(l10n.didntReceiveFirstFrameReconnect);
          return;
        }
        final frameDeadline = _resize;
        if (frameDeadline != null && statsAt > frameDeadline.deadlineUs) {
          await _viewportTimedOut(frameDeadline);
          return;
        }
        if ((stats['error'] as num? ?? 0) < 0) {
          diagnosticCode = 'decoder_failed';
          await _fail(l10n.couldntShowScreenSoConnection);
          return;
        }
        if (_tracks != null) {
          final acknowledgement = _tracks.acknowledge(stats, statsAt);
          if (acknowledgement != null) {
            _hasPresentedFrame = true;
            _send(acknowledgement);
            final change = _outputChange;
            if (change != null) {
              if (!change.metadataReady) {
                throw const FormatException(
                  'Output readiness before metadata.',
                );
              }
              _outputChange = null;
              resizeError = change.restored
                  ? l10n.couldntApplyRequestedSizeSo
                  : null;
              if (!change.done.isCompleted) {
                change.done.complete(!change.restored);
              }
            }
            status = l10n.connected;
            _startClipboard();
            _hasStreamed = true;
            ProductLog.instance.record(LogEvent.streamReady, {
              'ok': true,
              'width': width,
              'height': height,
              'audioFormat': ?audioFormat,
              ...connectTiming.ready(statsAt),
            });
          }
          if (_tracks.decoderStalled(statsAt)) {
            diagnosticCode = 'output_decoder_stalled';
            await _fail(l10n.oneMonitorsDisplayFrozeSo);
            return;
          }
        }
        final frames = (stats['decodedFrames'] as num).toInt();
        // stream-size-v1: the restarted decoder has its first picture.
        if (_pictureChanging && !_awaitingKeyFrame && frames > 0) {
          _pictureChanging = false;
        }
        if (_tracks == null &&
            _initialFramePending &&
            !_awaitingKeyFrame &&
            stats['decodedFrames'] is int &&
            frames > 0) {
          if (_signin != null) {
            final acknowledgement = _signin.decoded();
            if (acknowledgement == null) return;
            _send(acknowledgement);
            _retirePhaseInputGate();
          }
          _initialFramePending = false;
          _initialFrameDeadlineUs = null;
          if (_resize == null) {
            _startClipboard();
            _streamReady(stats);
          }
        }
        final framePending = _resize;
        if (framePending != null &&
            framePending.decoderStarted &&
            framePending.epoch == _epoch &&
            !_awaitingKeyFrame &&
            stats['decodedFrames'] is int &&
            frames > 0) {
          // RemoteEngine::Start owns a new frame store. published counts
          // complete decoded pixel buffers, independently of whether a
          // minimized Flutter window has consumed its texture yet.
          _resize = null;
          framePending.frameUs = statsAt;
          _releaseHeldPicture();
          if (!framePending.done.isCompleted) framePending.done.complete(true);
          _startClipboard();
          _streamReady(stats);
        }
        // input-latency-v1: every reading forwards its frame reports, the
        // short readiness reads too. An engine may report a frame only once
        // (Android hands them over), and a readiness check armed before the
        // first frame or a resize finished can still run after input resumed.
        _latency?.delivered(stats['inputFrames'], statsAt);
        if (readinessOnly) {
          notifyListeners();
          return;
        }
        if (frames > _lastFrames) _lastDecodedUs = statsAt;
        if (!resizing &&
            _androidObservation.stalled(
              statsAt,
              _lastDecodedUs,
              eligible: _androidObservationEligible,
            )) {
          diagnosticCode = 'decoder_stalled';
          await _fail(l10n.displayFrozeSoConnectionEnded);
          return;
        }
        final elapsed = statsAt - _lastStatsUs;
        decodedFps = elapsed > 0
            ? (frames - _lastFrames) * 1000000 / elapsed
            : null;
        decodedFpsLowest = _tracks != null && elapsed > 0
            ? _lowestOutputRate(stats['tracks'], elapsed)
            : null;
        receivedMbps = elapsed > 0
            ? (receivedBytes - _lastStatsBytes) * 8 / elapsed
            : null;
        final textures = stats['textureFrames'];
        textureFps = textures is int && textures >= _lastTextures && elapsed > 0
            ? (textures - _lastTextures) * 1000000 / elapsed
            : null;
        _samplePacing(stats, textures);
        _sampleBackpressure(stats);
        _sampleStalls(stats, statsAt);
        if (textures is int && textures >= 0) _lastTextures = textures;
        final calls = stats['decodeCalls'],
            total = metricNumber(stats['decodeTotalMs'], 1e12);
        // stream-size-v1: how much of this tick the decoder was busy and how
        // old the oldest waiting packet is, for the Host's scale controller.
        if (streamSize &&
            frameReady &&
            !resizing &&
            !_pictureChanging &&
            elapsed > 0) {
          // Utilisation (wall time with a frame inside the decoder) when the
          // engine reports it (Android); otherwise the summed decode latency
          // (Windows decodes one frame at a time, so the sum is the busy time).
          final busyTotal = metricNumber(stats['decodeBusyMs'], 1e12);
          final report = decoderReport(
            decodeMs: busyTotal != null
                ? (busyTotal >= _lastDecodeBusyMs
                      ? busyTotal - _lastDecodeBusyMs
                      : null)
                : total != null && total >= _lastDecodeTotalMs
                ? total - _lastDecodeTotalMs
                : null,
            elapsedMs: elapsed / 1000,
            backlogMs: metricNumber(stats['queueOldestMs'], 60000),
          );
          final busy = report['busy'] as int;
          decoderBusy = busy >= 0 ? busy : null;
          _send(report);
        }
        clientDecodeMs =
            calls is int &&
                calls > _lastDecodeCalls &&
                total != null &&
                total >= _lastDecodeTotalMs
            ? (total - _lastDecodeTotalMs) / (calls - _lastDecodeCalls)
            : null;
        if (calls is int && calls >= 0) _lastDecodeCalls = calls;
        if (total != null) _lastDecodeTotalMs = total;
        final busyNow = metricNumber(stats['decodeBusyMs'], 1e12);
        if (busyNow != null) _lastDecodeBusyMs = busyNow;
        clientDecodeMaxMs = metricNumber(stats['decodeMaxMs'], 60000);
        clientQueueMs = metricNumber(stats['queueOldestMs'], 60000);
        final queued = stats['queuedPackets'],
            skipped = stats['skippedDecodedFrames'];
        clientQueuePackets = queued is int && queued >= 0 && queued <= 8
            ? queued
            : null;
        skippedDecodedFrames = skipped is int && skipped >= 0 ? skipped : null;
        _lastStatsBytes = receivedBytes;
        _lastFrames = frames;
        _lastStatsUs = statsAt;
        final audioError = stats['audioError'];
        audioErrorCode = connection.options.audio && audioError is int
            ? audioError & 0xffffffff
            : null;
        audioStatus = !connection.options.audio
            ? l10n.off
            : (stats['audioError'] as num? ?? 0) < 0
            ? l10n.soundOutputDeviceIsntAvailable
            : (stats['audioPlayedFrames'] as num? ?? 0) > 0
            ? l10n.playing
            : remoteAudioMissing
            ? remoteAudioMissingStatus
            : l10n.waiting2;
        _sampleStats(statsAt);
        _observeSnapshot(statsAt, native: stats);
        notifyListeners();
      }
    } catch (_) {
      if (!_closed &&
          _androidObservation.current(observationEpoch) &&
          !_androidObservation.deferStatsFailure(
            _clock.elapsedMicroseconds,
            eligible: _androidObservationEligible,
          ) &&
          (_resize == null || _resize!.decoderStarted) &&
          revision == _mediaRevision) {
        diagnosticCode = 'media_status_failed';
        await _fail(l10n.couldntCheckDisplayStateSo);
      }
    } finally {
      _polling = false;
      _scheduleViewportFrameCheck();
    }
  }

  /// texture-pacing-v1 and render facts from one engine statistics reading;
  /// lifetime totals become per-tick values (before `_lastTextures` moves).
  void _samplePacing(Map<String, dynamic> stats, Object? textures) {
    final wait = metricNumber(stats['textureWaitTotalMs'], 1e12),
        upload = metricNumber(stats['textureUploadTotalMs'], 1e12);
    final uploads = stats['textureUploads'],
        queued = stats['textureQueued'],
        skipped = stats['skippedDecodedFrames'];
    textureWaitMs =
        textures is int &&
            textures > _lastTextures &&
            wait != null &&
            wait >= _lastWaitTotalMs
        ? (wait - _lastWaitTotalMs) / (textures - _lastTextures)
        : null;
    textureUploadMs =
        uploads is int &&
            uploads > _lastUploads &&
            upload != null &&
            upload >= _lastUploadTotalMs
        ? (upload - _lastUploadTotalMs) / (uploads - _lastUploads)
        : null;
    textureQueuedFrames = queued is int && queued >= _lastQueued
        ? queued - _lastQueued
        : null;
    skippedFramesTick = skipped is int && skipped >= _lastSkipped
        ? skipped - _lastSkipped
        : null;
    if (wait != null) _lastWaitTotalMs = wait;
    if (upload != null) _lastUploadTotalMs = upload;
    if (uploads is int && uploads >= 0) _lastUploads = uploads;
    if (queued is int && queued >= 0) _lastQueued = queued;
    if (skipped is int && skipped >= 0) _lastSkipped = skipped;
    final gpu = stats['gpuHardware'], remote = stats['remoteSession'];
    final path = stats['texturePath'], hz = stats['displayHz'];
    gpuHardware = gpu is bool ? gpu : null;
    clientRemoteSession = remote is bool ? remote : null;
    texturePath = path is int && path >= 0 && path <= 15 ? path : null;
    final tier = stats['decodeTier'], vendor = stats['decodeVendor'];
    decodeTier = tier is int && tier >= 0 && tier <= 2 ? tier : null;
    decodeVendor = vendor is int && vendor >= 0 && vendor <= 0xffff
        ? vendor
        : null;
    final lowLatency = stats['decoderLowLatency'],
        vendorKeys = stats['decoderVendorKeys'];
    decoderLowLatency = lowLatency is int && lowLatency >= 0 && lowLatency <= 31
        ? lowLatency
        : null;
    decoderVendorKeys = vendorKeys is int && vendorKeys >= 0 && vendorKeys <= 15
        ? vendorKeys
        : null;
    displayHz = hz is int && hz > 0 && hz <= 1000 ? hz : null;
    appCpuPct10 = _tenths(stats['appCpuPct10']);
    systemCpuPct10 = _tenths(stats['systemCpuPct10']);
    gpuDecodePct10 = _tenths(stats['gpuDecodePct10']);
    gpu3dPct10 = _tenths(stats['gpu3dPct10']);
  }

  static int? _tenths(Object? value) =>
      value is int && value >= 0 && value <= 1000 ? value : null;

  /// tracks-v1: the decoded rate of the output that decoded the fewest
  /// pictures since the last reading (decodedFps sums the outputs, which
  /// hides one monitor falling behind). A new engine restarts the counts.
  double? _lowestOutputRate(Object? rows, int elapsedUs) {
    if (rows is! List) return null;
    double? lowest;
    for (final row in rows) {
      if (row is! Map) continue;
      final id = row['id'], count = row['decodedFrames'];
      if (id is! String || count is! int || count < 0) continue;
      final last = _lastOutputFrames[id];
      _lastOutputFrames[id] = count;
      if (last == null || count < last) continue;
      final rate = (count - last) * 1000000 / elapsedUs;
      if (lowest == null || rate < lowest) lowest = rate;
    }
    return lowest;
  }

  /// stream-stall-v1: the engine's new stall records become events once the
  /// Host statistics covering them arrived. Diagnostics never affect the session.
  void _sampleStalls(Map<String, dynamic> stats, int nowUs) {
    try {
      final udp = _udp?.report;
      _stallWatch.engine(stats['stalls'], udp, nowUs);
      final rtt = rttMs;
      for (final event in _stallWatch.take(
        nowUs,
        rttMs: rtt == null || !rtt.isFinite ? null : rtt.round(),
        decoderBusy: decoderBusy,
        udpActivePath: udp?['udpActivePath'],
      )) {
        stallSink?.call(event);
      }
    } catch (_) {}
  }

  void _sampleBackpressure(Map<String, dynamic> stats) {
    mediaWaitsTick = _mediaWaits - _lastMediaWaits;
    _lastMediaWaits = _mediaWaits;
    videoGapMaxMsTick = _videoGapMaxUs == 0 ? null : _videoGapMaxUs / 1000;
    _videoGapMaxUs = 0;
    final dropped = stats['audioDroppedFrames'];
    audioDroppedTick = dropped is int && dropped >= _lastAudioDropped
        ? dropped - _lastAudioDropped
        : null;
    if (dropped is int && dropped >= 0) _lastAudioDropped = dropped;
    audioTick = _audioStats.read(stats);
    audioPathTick = connection.options.audio ? audioPathValues(stats) : null;
  }

  /// Only steady streaming counts: a resize or the first frame would skew
  /// the window's averages. Independent outputs use the native sums: decoded
  /// FPS is across outputs, not a per-monitor rate. Texture FPS counts Flutter
  /// texture takes only; it does not prove the native companion was presented.
  void _sampleStats(int nowUs) {
    final sink = statsSink;
    if (sink == null || !frameReady || resizing) return;
    final stalls = _stallWatch.takeWindow();
    final size = _statsWindow.size;
    if (size != null && size != (width, height)) _flushStats(nowUs);
    _statsWindow.add(
      StreamStatsSample(
        observability: _observabilityEnabled ? _observationValues(nowUs) : null,
        width: width,
        height: height,
        sourceWidth: sourceWidth,
        sourceHeight: sourceHeight,
        decodedFps: decodedFps,
        decodedFpsLowest: decodedFpsLowest,
        textureFps: textureFps,
        receivedMbps: receivedMbps,
        rttMs: rttMs,
        clientDecodeMs: clientDecodeMs,
        clientDecodeMaxMs: clientDecodeMaxMs,
        clientQueueMs: clientQueueMs,
        skippedDecodedFrames: skippedDecodedFrames,
        appliedBitrate: appliedBitrate,
        losslessTiles: losslessTiles,
        losslessKbps: losslessKbps,
        host: hostTelemetryFresh ? hostTelemetry : null,
        inputHostP50Ms: inputToHostP50Ms,
        inputHostP95Ms: inputToHostP95Ms,
        inputFrameP50Ms: inputToFrameP50Ms,
        inputFrameP95Ms: inputToFrameP95Ms,
        inputPushP50Ms: inputPushP50Ms,
        inputDecodeP50Ms: inputDecodeP50Ms,
        inputTransferP50Ms: inputTransferP50Ms,
        inputTransferP95Ms: inputTransferP95Ms,
        inputFrameBytes: inputFrameBytes,
        inputDownlinkP50Ms: inputDownlinkP50Ms,
        inputDownlinkP95Ms: inputDownlinkP95Ms,
        textureWaitMs: textureWaitMs,
        textureUploadMs: textureUploadMs,
        textureQueuedFrames: textureQueuedFrames,
        skippedFrames: skippedFramesTick,
        stalls: stalls.$1,
        stallMaxMs: stalls.$2,
        fpsTarget: hostFpsTarget,
        gpuHardware: gpuHardware,
        remoteSession: clientRemoteSession,
        texturePath: texturePath,
        displayHz: displayHz,
        mediaWaits: mediaWaitsTick,
        videoGapMaxMs: videoGapMaxMsTick,
        udpProbe: _udp?.report,
        audioDroppedFrames: audioDroppedTick,
        audio: audioTick,
        audioFormat: audioFormat,
        audioError: audioErrorCode,
        audioPath: audioPathTick,
        encodeScale: encodeScale,
        encodeLimit: encodeLimit?.code,
        decoderBusy: decoderBusy,
        appCpuPct10: appCpuPct10,
        systemCpuPct10: systemCpuPct10,
        gpuDecodePct10: gpuDecodePct10,
        gpu3dPct10: gpu3dPct10,
        decodeTier: decodeTier,
        decodeVendor: decodeVendor,
        decoderLowLatency: decoderLowLatency,
        decoderVendorKeys: decoderVendorKeys,
      ),
      nowUs,
    );
    if (_statsWindow.due(nowUs)) _flushStats(nowUs);
  }

  /// Sends the current partial window now, e.g. before its window closes
  /// the diagnostics channel (close() alone would run too late for that).
  void flushStats() {
    if (!_closed && _statsWindow.samples >= 3) {
      _flushStats(_clock.elapsedMicroseconds);
    }
    if (!_closed) _flushStallSummary();
  }

  /// stream-stall-v1: the stalls no row carried, as one closing row.
  void _flushStallSummary() {
    try {
      final summary = _stallWatch.summary();
      if (summary != null) stallSink?.call(summary);
    } catch (_) {}
  }

  void _flushStats(int nowUs) {
    final values = _statsWindow.take(nowUs);
    if (values == null) return;
    try {
      statsSink?.call({
        ...values,
        if (_tracks != null) 'monitors': _tracks.tracks.length,
      });
    } catch (_) {
      // Diagnostics never affect the session.
    }
  }

  void _scheduleViewportFrameCheck() {
    if (_closed ||
        (_resize?.decoderStarted != true &&
            !(_tracks == null && _initialFramePending && textureId != null) &&
            !_pictureChanging) ||
        _awaitingKeyFrame ||
        _viewportFrameTimer?.isActive == true) {
      return;
    }
    // First-frame and resize readiness do not wait for the statistics tick.
    // Only these bounded decoder-start phases add a short timer.
    _viewportFrameTimer = Timer(const Duration(milliseconds: 50), () {
      _viewportFrameTimer = null;
      unawaited(_poll(readinessOnly: true));
    });
  }

  Future<void> _viewportTimedOut(_ViewportChange pending) {
    diagnosticCode = pending.decoderStarted
        ? 'viewport_frame_timeout'
        : 'viewport_apply_timeout';
    endedByDisplayChange = true;
    return _fail(
      pending.decoderStarted
          ? l10n.couldntPrepareVideoNewResolution
          : l10n.remoteResolutionTookTooLong,
    );
  }

  void _streamReady(Map<String, dynamic> stats) {
    _hasStreamed = true;
    _hasPresentedFrame = true;
    status = l10n.connected;
    final prestarted = stats['decoderPrestarted'];
    ProductLog.instance.record(LogEvent.streamReady, {
      'ok': true,
      'width': width,
      'height': height,
      'audioFormat': ?audioFormat,
      // Windows decoder-prestart-v1: its decoder started before stream.info.
      if (prestarted is bool) 'decoderPrestarted': prestarted,
      ...connectTiming.ready(_clock.elapsedMicroseconds),
    });
  }

  static int _packetPts(List<int> packet) {
    var pts = 0;
    for (var i = 0; i < 8; i++) {
      pts |= packet[4 + i] << (8 * i);
    }
    return pts;
  }

  void _noteArrival(List<int> packet, int epoch, int arrivedUs) =>
      _noteFeedback(
        epoch,
        packet.length,
        packet[0] == 1 ? _packetPts(packet) : null,
        arrivedUs,
      );

  void _noteFeedback(int epoch, int bytes, int? pts, int arrivedUs) {
    if (epoch != _feedbackEpoch || _arrivals.length >= 64) _sendFeedback();
    _feedbackEpoch = epoch;
    _feedbackBytes += bytes;
    if (pts != null && pts >= 0) _arrivals.add([pts, arrivedUs]);
  }

  void _noteVideoArrival(int arrivedUs) {
    _stallWatch.arrival(arrivedUs);
    if (_lastVideoArrivalUs != 0 &&
        arrivedUs - _lastVideoArrivalUs > _videoGapMaxUs) {
      _videoGapMaxUs = arrivedUs - _lastVideoArrivalUs;
    }
    _lastVideoArrivalUs = arrivedUs;
  }

  void _sendFeedback() {
    final now = _clock.elapsedMicroseconds;
    if (_closed || (_arrivals.isEmpty && _feedbackBytes == 0)) {
      // Nothing arrived: the next report covers only its own interval.
      _feedbackSinceUs = now;
      return;
    }
    _send({
      'type': 'stream.feedback',
      'epoch': _feedbackEpoch,
      'frames': List<List<int>>.of(_arrivals),
      'bytes': _feedbackBytes.clamp(0, 1000000000),
      'intervalUs': (now - _feedbackSinceUs).clamp(1, 10000000),
    });
    _arrivals.clear();
    _feedbackBytes = 0;
    _feedbackSinceUs = now;
  }

  void _send(Map<String, dynamic> event, {SigninInputOwner? inputOwner}) {
    // A socket being replaced (session-resume-v1) takes nothing more.
    if (!_closed && _socket != null && !_resumingTransport) {
      try {
        final observed = event;
        if (_signin != null) {
          if (['input', 'input.reset', 'input.fence'].contains(event['type'])) {
            final wrapped = _signin.wrapInput(
              event,
              inputOwner ?? _signin.inputOwner,
            );
            if (wrapped == null) return;
            event = wrapped;
          }
          if (!_userPhase &&
              !{
                'input.phase',
                'stream.phase.decoded',
                'ping',
                'session.stop',
                'stream.feedback',
              }.contains(event['type'])) {
            return;
          }
        }
        final inputLeg = _inputLeg(event);
        final inputEpoch = _tracks == null
            ? _epoch
            : _udpScopes.entries
                      .where(
                        (entry) => identical(_udpTracks[entry.key], inputLeg),
                      )
                      .firstOrNull
                      ?.value
                      .epoch ??
                  0;
        final direct = inputLeg?.wrapInput(event, inputEpoch);
        _socket!.add(jsonEncode(direct ?? event));
        if (direct != null) inputLeg?.fastInput(direct);
        if ([
          'cursor.compose',
          'cursor.echo',
          'caret.watch',
          'display.scale',
          'stream.resize',
          'stream.tracks.resize',
          'stream.tracks.ready',
        ].contains(event['type'])) {
          final fence = inputLeg?.wrapInput({
            'type': 'input.fence',
          }, inputEpoch);
          if (fence != null) _socket!.add(jsonEncode(fence));
        }
        _inputObservation.observe(observed);
      } catch (_) {
        /* Transport cleanup still releases native input and media. */
      }
    }
  }

  void _sendInput(Map<String, dynamic> event, {SigninInputOwner? owner}) {
    if (event['type'] == 'input' && !SessionHumanSource.inputAllowed) return;
    // Recheck at send time too: clipboard preparation can defer a packet.
    if (event['event'] == 'text' &&
        (!textInput ||
            !validTextInputMessage(event) ||
            !captured ||
            _closed ||
            resizing ||
            !_inputReady)) {
      return;
    }
    if (_signin != null && !_signin.owns(owner)) return;
    final message = _tracks == null ? event : _tracks.input(event);
    if (message == null) return;
    // A sampled event is the same message with one sequence number; it is sent
    // at the same point in the same order. Its send time is the measurement origin.
    final sample = _closed || _socket == null
        ? null
        : _latency?.sample(message, _clock.elapsedMicroseconds);
    _send(
      sample == null ? message : {...message, 'sample': sample},
      inputOwner: owner,
    );
  }

  // print-v1 starts with the first picture, like the clipboard channel.
  void _startPrint() {
    final url = connection.printWebSocketUrl;
    if (!_userPhase || url == null || printer != null || _closed) return;
    printer = _printSession(url, null)..addListener(_printerChanged);
    unawaited(printer!.start());
  }

  /// Starts drive-v1 with the first picture, like the clipboard: a separate
  /// socket that never waits for (or delays) video and input.
  void _startDrive() {
    final factory = driveFactory;
    if (factory == null ||
        !_userPhase ||
        connection.driveWebSocketUrl == null ||
        drive != null ||
        _closed) {
      return;
    }
    drive = factory(connection, _token)..addListener(_driveChanged);
    unawaited(drive!.start().catchError((Object _) {}));
  }

  /// Starts usb-v1 with the first picture, like the clipboard: a separate
  /// socket that never waits for (or delays) video and input.
  void _startUsb() {
    final factory = usbFactory;
    if (factory == null ||
        !_userPhase ||
        connection.usbWebSocketUrl == null ||
        usb != null ||
        _closed) {
      return;
    }
    usb = factory(connection, _token)..addListener(_usbChanged);
    unawaited(usb!.start().catchError((Object _) {}));
  }

  void _startClipboard() {
    if (!_userPhase) return;
    _signinRedirectOwner ??= _signin?.redirectOwner;
    _startPrint();
    _startDrive();
    _startUsb();
    final url = connection.clipboardWebSocketUrl;
    if (_closed || url == null || !connection.clipboardPolicy.any) {
      return;
    }
    final current = clipboard;
    if (current != null) {
      _scheduleClipboardReconnect(current);
      return;
    }
    clipboard =
        (clipboardFactory?.call(connection, _token) ??
              ClipboardSession(
                url: url,
                token: _token,
                sessionId: connection.sessionId,
                policy: connection.clipboardPolicy,
                syncSupported: connection.clipboardSync,
              ))
          ..addListener(_clipboardChanged);
    unawaited(clipboard!.start().catchError((Object _) {}));
  }

  void _scheduleClipboardReconnect(ClipboardSession previous) {
    if (_closed ||
        !_userPhase ||
        !frameReady ||
        resizing ||
        !identical(clipboard, previous) ||
        !previous.canReconnect ||
        clipboardRecovering ||
        // session-resume-v1: while the stream itself is away the channel is
        // lost with it and a dial says nothing about the channel, so it costs
        // no attempt. [_clipboardAfterResume] starts it again.
        resuming ||
        _clipboardReconnectAttempts >= 3) {
      return;
    }
    final delay = Duration(seconds: 1 << _clipboardReconnectAttempts++);
    _clipboardReconnectTimer = Timer(delay, () {
      _clipboardReconnectTimer = null;
      if (resuming) {
        // The stream went away during the delay: the attempt is not spent.
        if (_clipboardReconnectAttempts > 0) _clipboardReconnectAttempts--;
        return;
      }
      unawaited(_replaceClipboard(previous));
    });
  }

  /// session-resume-v1: the stream continues (the client's socket was replaced
  /// or the Host came back). A clipboard channel that was lost for a transient
  /// reason gets one fresh set of the same three attempts: the ones spent in
  /// the first seconds of the outage said nothing about the channel, and the
  /// resumed connection used to keep a dead clipboard. A refusal, a policy
  /// change or a failed cleanup ([ClipboardSession.canReconnect] false) stays
  /// closed, and a channel that is really broken ends after these three: the
  /// views then show [clipboardStopNotice].
  void _channelsAfterResume() {
    _clipboardAfterResume();
    if (_closed || resuming) return;
    // channel-resume-v1: printing, folders and USB that fell with the stream
    // (or ran out of attempts during the outage) are dialled again.
    _printRedial.resumed();
    _driveRedial.resumed();
    _usbRedial.resumed();
  }

  void _clipboardAfterResume() {
    final current = clipboard;
    if (_closed ||
        resuming ||
        current == null ||
        !current.closed ||
        !current.canReconnect) {
      return;
    }
    _clipboardReconnectAttempts = 0;
    _scheduleClipboardReconnect(current);
  }

  Future<void> _replaceClipboard(ClipboardSession previous) async {
    if (_closed ||
        !_userPhase ||
        !frameReady ||
        resizing ||
        !identical(clipboard, previous) ||
        !previous.canReconnect) {
      return;
    }
    _clipboardReplacing = true;
    notifyListeners();
    try {
      // Never overlap native owners. A timed-out old cleanup stays retired;
      // its outstanding Windows work cannot be made safe by a new handshake.
      await previous.close().timeout(const Duration(seconds: 6));
      if (_closed ||
          !_userPhase ||
          !frameReady ||
          resizing ||
          !identical(clipboard, previous) ||
          !previous.canReconnect) {
        return;
      }
      previous.removeListener(_clipboardChanged);
      final fresh =
          clipboardFactory?.call(connection, _token) ??
          ClipboardSession(
            url: connection.clipboardWebSocketUrl!,
            token: _token,
            sessionId: connection.sessionId,
            policy: connection.clipboardPolicy,
            syncSupported: connection.clipboardSync,
          );
      fresh.inheritRecoveryFrom(previous);
      clipboard = fresh..addListener(_clipboardChanged);
      unawaited(fresh.start().catchError((Object _) {}));
    } catch (_) {
      // A feature's cleanup/construction failure cannot end video or input.
      // Keep the old owner; the remaining bounded attempts still await it.
      if (!_closed && identical(clipboard, previous)) {
        previous.removeListener(_clipboardChanged);
        previous.addListener(_clipboardChanged);
      }
    } finally {
      _clipboardReplacing = false;
      if (!_closed) {
        final current = clipboard;
        if (current != null) _scheduleClipboardReconnect(current);
        notifyListeners();
      }
    }
  }

  void input(Map<String, dynamic> event) {
    if (!SessionHumanSource.inputAllowed) return;
    if (captured && !_closed && !resizing && _inputReady) _inputGate.add(event);
  }

  Future<bool> resizeViewport(
    int targetWidth,
    int targetHeight, {
    bool Function()? isCurrent,
    void Function()? onDispatched,
  }) async {
    final phaseOwner = _signin?.inputOwner;
    if (isCurrent?.call() == false ||
        _clipboardTransferBlocked ||
        _closed ||
        _socket == null ||
        !canResizeViewport ||
        textureId == null ||
        resizing ||
        resuming ||
        !ownedOutputSize(targetWidth, targetHeight, portrait: streamSize)) {
      return false;
    }
    if ((connection.initialViewport != null ||
            (width == targetWidth && height == targetHeight)) &&
        sourceWidth == targetWidth &&
        sourceHeight == targetHeight) {
      return true;
    }
    final beganUs = _clock.elapsedMicroseconds;
    await capture(false);
    if (isCurrent?.call() == false ||
        (_signin != null && !_signin.owns(phaseOwner)) ||
        _clipboardTransferBlocked ||
        _closed ||
        resizing) {
      return false;
    }
    DecoderLimits? limits;
    if (connection.streamSizeV2) {
      _capacityPending = true;
      notifyListeners();
      try {
        limits = await _probeCapacity([
          StreamSourceSize(targetWidth, targetHeight),
        ], connection.options.fps);
      } on StreamCapacityException catch (error) {
        resizeError = streamCapacityMessage(error.reason);
        return false;
      } finally {
        _capacityPending = false;
        notifyListeners();
      }
      if (isCurrent?.call() == false ||
          (_signin != null && !_signin.owns(phaseOwner)) ||
          _clipboardTransferBlocked ||
          _closed ||
          resizing ||
          _socket == null) {
        return false;
      }
    } else if (!legacyOwnedOutputSize(
      targetWidth,
      targetHeight,
      portrait: streamSize,
    )) {
      resizeError = l10n.largerResolutionsNeedHostServer;
      notifyListeners();
      return false;
    }
    final pending = _ViewportChange(
      'viewport-${++_resizeNonce}',
      targetWidth,
      targetHeight,
      _clock.elapsedMicroseconds + 15000000,
    );
    pending.decoderLimits = limits;
    pending.beganUs = beganUs;
    pending.dispatchedUs = _clock.elapsedMicroseconds;
    _resize = pending;
    ProductLog.instance.record(LogEvent.viewportRequest, {
      'width': targetWidth,
      'height': targetHeight,
    });
    resizeError = null;
    onDispatched?.call();
    _prepareDecoder(targetWidth, targetHeight, connection.options.fps, 1);
    _send({
      'type': 'stream.resize',
      'requestId': pending.requestId,
      'width': targetWidth,
      'height': targetHeight,
      'fps': connection.options.fps,
      'monitor': connection.options.monitor,
      if (limits != null) 'decoderLimits': limits.toJson(),
    });
    notifyListeners();
    final applied = await pending.done.future;
    ProductLog.instance.record(LogEvent.viewportResult, {
      'ok': applied,
      'width': targetWidth,
      'height': targetHeight,
      ...pending.stageValues(),
    });
    return applied;
  }

  /// Resize all already-authorized outputs, keeping input revoked until the
  /// new topology has produced a decoded frame for every output.
  Future<bool> resizeTracks(TrackPlan requested, {int? fps}) async {
    final outputs = _tracks;
    if (_clipboardTransferBlocked ||
        _closed ||
        _socket == null ||
        !canResizeTracks ||
        outputs == null ||
        !outputs.ready ||
        resizing) {
      return false;
    }
    final targetFps = fps ?? outputs.fps;
    if (outputs.plan.matches(requested.toJson()) && targetFps == outputs.fps) {
      return true;
    }
    await capture(false);
    if (_clipboardTransferBlocked || _closed || resizing || !outputs.ready) {
      return false;
    }
    DecoderLimits? limits;
    if (connection.streamSizeV2) {
      _capacityPending = true;
      notifyListeners();
      try {
        limits = await _probeCapacity([
          for (final t in requested.outputs)
            StreamSourceSize(t.width, t.height),
        ], targetFps);
      } on StreamCapacityException catch (error) {
        resizeError = streamCapacityMessage(error.reason);
        return false;
      } finally {
        _capacityPending = false;
        notifyListeners();
      }
      if (_clipboardTransferBlocked ||
          _closed ||
          resizing ||
          _socket == null ||
          !outputs.ready) {
        return false;
      }
    } else if (!requested.legacyCompatible) {
      resizeError = l10n.largerResolutionsNeedHostServer;
      notifyListeners();
      return false;
    }
    final change = _OutputChange(
      'outputs-${++_resizeNonce}',
      _clock.elapsedMicroseconds + 90000000,
    );
    final command = outputs.requestResize(
      requested,
      targetFps,
      change.requestId,
      _clock.elapsedMicroseconds,
      nextDecoderLimits: limits,
    );
    _outputChange = change;
    // Invalidate any stats already in flight for the previous output owner.
    _mediaRevision++;
    resizeError = null;
    status = l10n.resizingVirtualDisplay;
    _prepareDecoder(
      requested.outputs.first.width,
      requested.outputs.first.height,
      targetFps,
      requested.outputs.length,
    );
    _send(command);
    notifyListeners();
    return change.done.future;
  }

  /// User-invoked toolbar shortcuts are scoped to this already authorized session.
  Future<bool> shortcut(String command) async {
    final phaseOwner = _signin?.inputOwner;
    final keys = switch (command) {
      'windows' => const [0x5B],
      'altTab' => const [0xA4, 0x09],
      'taskManager' => const [0xA2, 0xA0, 0x1B],
      _ => const <int>[],
    };
    if (keys.isEmpty ||
        _clipboardTransferBlocked ||
        !_inputReady ||
        _closed ||
        resizing ||
        _socket == null ||
        textureId == null) {
      return false;
    }
    await capture(false);
    if (_clipboardTransferBlocked ||
        _closed ||
        (_signin != null && !_signin.owns(phaseOwner))) {
      return false;
    }
    _send({'type': 'input.reset'}, inputOwner: phaseOwner);
    for (final down in [true, false]) {
      for (final key in down ? keys : keys.reversed) {
        _sendInput({
          'type': 'input',
          'event': 'key',
          'vk': key,
          'scan': 0,
          'down': down,
          'extended': key == 0x5B,
        }, owner: phaseOwner);
      }
    }
    _send({'type': 'input.reset'}, inputOwner: phaseOwner);
    return !_closed;
  }

  Future<void> capture(bool enabled) async {
    if (enabled && _clipboardTransferBlocked) return;
    if ((_closed || resizing || !_inputReady) && enabled) return;
    final revision = ++_captureRevision;
    final clipboardAtRequest = clipboard;
    if (!enabled) {
      _inputGate.reset();
      unawaited(clipboard?.setSharingActive(false));
      _discardPointerInput?.call();
      captured = false;
      _send({'type': 'input.reset'});
      if (!_closed) notifyListeners();
    }
    try {
      final accepted = await _media
          .capture(enabled)
          .timeout(const Duration(seconds: 2));
      if (_closed || revision != _captureRevision) return;
      captured = accepted && enabled && !_closed;
    } catch (_) {
      if (_closed || revision != _captureRevision) return;
      captured = false;
    }
    unawaited(clipboard?.setSharingActive(captured));
    if (captured && identical(clipboard, clipboardAtRequest)) {
      unawaited(clipboard?.retrySharingInitialization());
    }
    if (!_closed) notifyListeners();
  }

  Future<void> _fail(String message) async {
    if (_closed) return;
    error = message;
    status = l10n.connectionEnded2;
    await close();
  }

  /// An observed OS network loss is not the user's explicit disconnect.
  /// Keep an earlier failure authoritative and release only this reservation.
  Future<void> networkLost() {
    if (!_closed && _closing == null && diagnosticCode == null) {
      diagnosticCode = 'network_error';
      error = status = l10n.networkConnectionDropped;
    }
    return close();
  }

  Future<void> close({bool notifyHost = true}) {
    final previous = _closing;
    if (previous != null) return previous;
    _cancelCursorRetry();
    final result = Completer<void>();
    _closing = result.future;
    _close(notifyHost).then(result.complete, onError: result.completeError);
    return result.future;
  }

  Future<void> _close(bool notifyHost) async {
    if (_closed) return;
    final ordinaryDisconnect =
        diagnosticCode == null && error == null && notifyHost;
    Future<void>? thumbnailRevocation;
    if (!ordinaryDisconnect && _media is RemoteThumbnailPrivacy) {
      // Invoke before stop/notifications can retire the last native frame.
      try {
        thumbnailRevocation = (_media as RemoteThumbnailPrivacy)
            .clearThumbnail();
      } catch (_) {}
    }
    // One cause per session, recorded before auxiliary/native cleanup can hang
    // or report its own errors. EOF and a local stop used to leave no end record.
    if (!_endRecorded) {
      _endRecorded = true;
      final localStop = diagnosticCode == null && error == null && notifyHost;
      diagnosticCode ??= localStop ? 'client_stopped' : 'unknown_failure';
      ProductLog.instance.recordFailure(LogEvent.streamEnd, diagnosticCode, {
        'ok': localStop || normalStreamEnds.contains(diagnosticCode),
        if (connection.trackPlan != null)
          'monitors': connection.trackPlan!.outputs.length,
        'error': ?_failureError,
      });
    }
    // A short session's partial window is still worth one report.
    if (_statsWindow.samples >= 3) _flushStats(_clock.elapsedMicroseconds);
    _flushStallSummary();
    _observeSnapshot(_clock.elapsedMicroseconds, ended: true);
    _inputGate.reset();
    _discardPointerInput?.call();
    _pointerOrderingOwner = null;
    _beforeKeyInput = _discardPointerInput = null;
    _send({'type': 'input.reset'});
    if (notifyHost) _send({'type': 'session.stop'});
    _closed = true;
    _signin?.close();
    _mediaRevision++;
    _captureRevision++;
    final pending = _resize;
    _resize = null;
    final outputChange = _outputChange;
    _outputChange = null;
    if (outputChange != null && !outputChange.done.isCompleted) {
      outputChange.done.complete(false);
    }
    if (pending != null && !pending.done.isCompleted) {
      pending.done.complete(false);
    }
    clipboard?.removeListener(_clipboardChanged);
    drive?.removeListener(_driveChanged);
    usb?.removeListener(_usbChanged);
    printer?.removeListener(_printerChanged);
    _printRedial.reset();
    _driveRedial.reset();
    _usbRedial.reset();
    captured = false;
    _caretWatching = false;
    caretUpdates.value = null;
    _timer?.cancel();
    _clipboardReconnectTimer?.cancel();
    _clipboardReconnectTimer = null;
    _feedbackTimer?.cancel();
    _viewportFrameTimer?.cancel();
    _releaseHeldPicture(now: true);
    _endComposeHold();
    _closeUdp();
    notifyListeners();
    Future<void> bounded(
      Future<dynamic> Function() cleanup, [
      Duration timeout = const Duration(seconds: 2),
    ]) async {
      try {
        await cleanup().timeout(timeout);
      } catch (_) {}
    }

    // Revoke is ordered before retirement on the same native method channel.
    // Timeout is not proof of disk deletion/native completion.
    final release = notifyHost
        ? bounded(connection.release, const Duration(seconds: 3))
        : null;
    if (thumbnailRevocation != null) await bounded(() => thumbnailRevocation!);
    // Reservation release already started; local retirement follows revocation.
    await Future.wait([
      ?release,
      bounded(() => _media.capture(false)),
      bounded(() async => _socket?.close()),
      bounded(() async => _input?.cancel()),
      bounded(() => _media.stop()),
      // A normal end, told apart from a broken channel in the diagnostics.
      bounded(() async => clipboard?.close(reason: 'clipboard_session_closed')),
      // Unprinted documents of this session are deleted with it.
      bounded(() async => printer?.close()),
      bounded(() async => drive?.close()),
      // Every USB device goes back to this PC (also when the stream broke).
      bounded(() async => usb?.close(), const Duration(seconds: 6)),
    ]);
    notifyListeners();
  }
}

class _OutputChange {
  _OutputChange(this.requestId, this.deadlineUs);
  final String requestId;
  int deadlineUs;
  bool restored = false, metadataReady = false;
  final done = Completer<bool>();
}
