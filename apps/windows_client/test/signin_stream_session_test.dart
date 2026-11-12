import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/services.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/initial_viewport.dart';

import 'remote_session_test.dart' show SessionMedia, SessionSocket, flush;
import 'signin_stream_contract_test.dart'
    show signinSessionId, signinConnectionJson, signinBindingJson, preparePhase;

class SigninMedia extends SessionMedia implements RemoteThumbnailPrivacy {
  int decoded = 0, privacyClears = 0, stops = 0;
  final calls = <String>[];
  Completer<Map<String, dynamic>>? statsReply;
  Completer<bool>? releaseCapture;
  Completer<void>? privacyReply;
  bool privacyFails = false;
  @override
  Future<int> start(int width, int height) async {
    calls.add('start');
    decoded = 0;
    return super.start(width, height);
  }

  @override
  Future<void> clearThumbnail() async {
    privacyClears++;
    calls.add('privacy');
    if (privacyFails) {
      throw PlatformException(code: 'thumbnail_privacy_unknown');
    }
    await privacyReply?.future;
  }

  @override
  Future<bool> capture(bool enabled) {
    if (!enabled && releaseCapture != null) return releaseCapture!.future;
    return super.capture(enabled);
  }

  @override
  Future<void> stop() async {
    stops++;
    calls.add('stop');
  }

  @override
  Future<Map<String, dynamic>> stats() async =>
      statsReply?.future ??
      Future.value({
        'decodedFrames': decoded,
        'error': 0,
        'audioError': -1,
        'audioPlayedFrames': 0,
      });
}

Map<String, dynamic> phaseInfo({
  int generation = 1,
  int epoch = 1,
  String phase = 'signin',
  String? transition,
}) => {
  'type': 'stream.phase.info',
  ...signinBindingJson(),
  'sessionId': signinSessionId,
  'workerGeneration': generation,
  'transitionId': transition ?? (generation == 1 ? '1' : '2') * 32,
  'phase': phase,
  'mediaScope': {'kind': 'composite', 'epoch': epoch},
  'firstFrameId': (generation == 1 ? 'a' : 'b') * 32,
  'info': {
    'type': 'stream.info',
    'sessionId': signinSessionId,
    'epoch': epoch,
    'codec': 'h264',
    'width': 1280,
    'height': 720,
    'sourceWidth': 1280,
    'sourceHeight': 720,
    'fpsTarget': 60,
  },
};
Uint8List signinFrame([int epoch = 1]) => Uint8List.fromList([
  1,
  1,
  epoch & 255,
  epoch >> 8,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0x67,
  1,
  0,
  0,
  0,
  1,
  0x68,
  1,
  0,
  0,
  0,
  1,
  0x65,
  1,
]);
BrokerConnection phaseConnection() => BrokerConnection.fromJson(
  {...signinConnectionJson(), 'signin': signinBindingJson()},
  Uri.parse('https://cloud.example.com'),
  requestedSignin: true,
);

class _Clipboard extends Fake implements ClipboardSession {
  int starts = 0, closes = 0;
  Completer<bool>? paste;
  @override
  bool get closed => closes > 0;
  @override
  bool get automatic => true;
  @override
  bool get canSend => true;
  @override
  bool get canReconnect => false;
  @override
  void addListener(void Function() listener) {}
  @override
  void removeListener(void Function() listener) {}
  @override
  Future<void> start() async {
    starts++;
  }

  @override
  Future<void> close({String reason = 'clipboard_session_closed'}) async {
    closes++;
  }

  @override
  Future<void> setSharingActive(bool active) async {}
  @override
  Future<void> retrySharingInitialization() async {}
  @override
  Future<bool> preparePaste() => paste?.future ?? Future.value(true);
}

class _Drive extends Fake implements DriveSession {
  int starts = 0, closes = 0;
  @override
  void addListener(void Function() listener) {}
  @override
  void removeListener(void Function() listener) {}
  @override
  Future<void> start() async {
    starts++;
  }

  @override
  Future<void> close({String reason = 'drive_session_closed'}) async {
    closes++;
  }
}

class _Usb extends Fake implements UsbSession {
  int starts = 0, closes = 0;
  Completer<void>? closing;
  @override
  void addListener(void Function() listener) {}
  @override
  void removeListener(void Function() listener) {}
  @override
  Future<void> start() async {
    starts++;
  }

  @override
  Future<void> close({String reason = 'usb_session_closed'}) async {
    closes++;
    await closing?.future;
  }
}

class _Harness {
  _Harness({
    bool redirects = false,
    bool initiallyDesktop = false,
    bool initialViewport = false,
    bool encoderLimits = false,
    Stopwatch? clock,
  }) {
    final binding = SigninBinding.parse({
      ...signinBindingJson(),
      if (initiallyDesktop) 'phase': 'desktop',
    });
    final base = Uri.parse(
      'wss://cloud.example.com/api/v1/sessions/$signinSessionId',
    );
    final connection = BrokerConnection(
      sessionId: signinSessionId,
      webSocketUrl: base,
      epochMedia: true,
      viewportProvisioning: true,
      viewportEncoderLimits: encoderLimits,
      initialViewport: initialViewport
          ? InitialViewportPreparation.fromJson(
              {
                'protocol': 'epoch-v1',
                'sessionId': signinSessionId,
                'requestId': '11111111-1111-4111-8111-111111111111',
                'resizeId': '22222222-2222-4222-8222-222222222222',
                'monitor': '0',
                'width': 1280,
                'height': 720,
                'fps': 60,
                'epoch': 1,
              },
              sessionId: signinSessionId,
              requested: InitialViewport(width: 1280, height: 720, fps: 60),
            )
          : null,
      signin: binding,
      clipboardWebSocketUrl: redirects
          ? base.replace(path: '${base.path}/clipboard')
          : null,
      clipboardPolicy: redirects
          ? const ClipboardPolicy.all()
          : const ClipboardPolicy(),
      driveWebSocketUrl: redirects
          ? base.replace(path: '${base.path}/drive')
          : null,
      usbWebSocketUrl: redirects
          ? base.replace(path: '${base.path}/usb')
          : null,
      printWebSocketUrl: redirects
          ? base.replace(path: '${base.path}/print')
          : null,
    );
    session = RemoteSession(
      connection: connection,
      token: 'synthetic-only',
      media: media,
      clock: clock,
      dialer: (_, _) async {
        dials++;
        return socket;
      },
      clipboardFactory: (_, _) => clipboard,
      driveFactory: (_, _) => drive,
      usbFactory: (_, _) => usb,
      printDialer: (_, _) async {
        printDials++;
        return printSocket;
      },
    );
    startPhase = initiallyDesktop ? 'desktop' : 'signin';
  }
  final socket = SessionSocket(),
      printSocket = SessionSocket(),
      media = SigninMedia();
  final clipboard = _Clipboard(), drive = _Drive(), usb = _Usb();
  late final RemoteSession session;
  late String startPhase;
  int dials = 0, printDials = 0;
  Future<void> start() async {
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(phaseInfo(phase: startPhase)));
    await flush();
  }

  Future<void> decoded([int epoch = 1]) async {
    socket.incoming.add(signinFrame(epoch));
    await flush();
    media.decoded = 1;
    await Future<void>.delayed(const Duration(milliseconds: 65));
  }

  Future<void> prepare({
    int generation = 2,
    int epoch = 2,
    String phase = 'desktop',
    String? transition,
  }) async {
    socket.incoming.add(
      jsonEncode(
        preparePhase(
          generation: generation,
          epoch: epoch,
          phase: phase,
          transition: transition,
        ),
      ),
    );
    await flush();
    expect(
      session.closed,
      isFalse,
      reason: 'phase prepare: ${session.diagnosticCode} / ${media.calls}',
    );
  }

  Future<void> info({
    int generation = 2,
    int epoch = 2,
    String phase = 'desktop',
    String? transition,
  }) async {
    socket.incoming.add(
      jsonEncode(
        phaseInfo(
          generation: generation,
          epoch: epoch,
          phase: phase,
          transition: transition,
        ),
      ),
    );
    await flush();
    expect(
      session.closed,
      isFalse,
      reason:
          'phase info: ${session.diagnosticCode} / ${session.error} / ${media.calls}',
    );
  }

  List<Map<String, dynamic>> get acks =>
      socket.sent.where((e) => e['type'] == 'stream.phase.decoded').toList();
  List<Map<String, dynamic>> get inputs =>
      socket.sent.where((e) => e['type'] == 'input.phase').toList();
  Future<void> close() => session.close();
}

class _ResizeClock extends Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

Map<String, dynamic> desktopResizeInfo({int epoch = 2}) => {
  ...phaseInfo(phase: 'desktop', epoch: epoch),
  'firstFrameId': 'c' * 32,
};

void main() {
  test(
    'Closing during a same-worker privacy ACK cannot activate retired pixels',
    () async {
      final f = _Harness(redirects: true, initiallyDesktop: true);
      try {
        await f.start();
        await f.decoded();
        f.media.privacyReply = Completer<void>();
        f.socket.incoming.add(jsonEncode(desktopResizeInfo()));
        await flush();
        expect(f.session.frameReady, isFalse);
        await f.close();
        f.media.privacyReply!.complete();
        await flush();
        expect(f.acks, hasLength(1));
        expect(f.session.closed, isTrue);
        expect([f.clipboard.closes, f.drive.closes, f.usb.closes], [1, 1, 1]);
        expect(f.printSocket.wasClosed, isTrue);
      } finally {
        if (f.media.privacyReply?.isCompleted == false) {
          f.media.privacyReply!.complete();
        }
        await f.close();
      }
    },
  );
  test('A role change during unqualified resize fails closed and retires every redirect', () async {
    final f = _Harness(redirects: true, initiallyDesktop: true);
    try {
      await f.start();
      await f.decoded();
      f.socket.incoming.add(jsonEncode(desktopResizeInfo()));
      await flush();
      f.socket.incoming.add(
        jsonEncode(preparePhase(generation: 2, epoch: 3, phase: 'signin')),
      );
      await flush();
      expect(f.session.closed, isTrue);
      expect(f.acks, hasLength(1));
      expect([f.clipboard.closes, f.drive.closes, f.usb.closes], [1, 1, 1]);
      expect(f.printSocket.wasClosed, isTrue);
    } finally {
      await f.close();
    }
  });
  test(
    'Mismatched pending resize cannot reuse the previous decoder for ACK',
    () async {
      final f = _Harness(initiallyDesktop: true);
      try {
        await f.start();
        await f.decoded();
        final pending = f.session.resizeViewport(1600, 900);
        await flush();
        final info = desktopResizeInfo();
        info['info'] = {
          ...info['info'] as Map,
          'requestId': 'foreign-request',
          'resizeId': 'foreign-resize',
          'sourceWidth': 1600,
          'sourceHeight': 900,
        };
        f.socket.incoming.add(jsonEncode(info));
        await flush();
        if (!f.session.closed) {
          f.socket.incoming.add(signinFrame(2));
        }
        await flush();
        await Future<void>.delayed(const Duration(milliseconds: 65));
        expect(f.acks, hasLength(1));
        expect(f.session.closed, isTrue);
        expect(await pending, isFalse);
      } finally {
        await f.close();
      }
    },
  );
  test('Same-worker resize retains all four desktop channel owners until fresh decoded ACK', () async {
    final f = _Harness(redirects: true, initiallyDesktop: true);
    try {
      await f.start();
      await f.decoded();
      final oldClipboard = f.session.clipboard,
          oldDrive = f.session.drive,
          oldUsb = f.session.usb,
          oldPrint = f.session.printer;
      await f.session.capture(true);
      f.session.input({
        'type': 'input',
        'event': 'key',
        'vk': 65,
        'down': true,
      });
      f.socket.incoming.add(jsonEncode(desktopResizeInfo()));
      await flush();
      expect(f.session.closed, isFalse);
      expect(f.session.frameReady, isFalse);
      expect(f.session.captured, isFalse);
      expect(f.session.remoteInputHeld, isFalse);
      expect(f.media.starts, 2);
      expect(f.acks, hasLength(1));
      final inputCount = f.inputs.length;
      f.session.input({
        'type': 'input',
        'event': 'key',
        'vk': 66,
        'down': true,
      });
      f.media.decoded = 1;
      await Future<void>.delayed(const Duration(milliseconds: 65));
      expect(
        f.acks,
        hasLength(1),
        reason: 'Old pixels or metadata cannot ACK a new epoch',
      );
      f.media.decoded = 0;
      await f.decoded(2);
      expect(f.session.frameReady, isTrue);
      expect(f.acks, hasLength(2));
      expect(f.acks.last['workerGeneration'], 1);
      expect(f.acks.last['transitionId'], '1' * 32);
      expect(f.acks.last['mediaScope'], {'kind': 'composite', 'epoch': 2});
      expect(f.inputs, hasLength(inputCount));
      expect(identical(f.session.clipboard, oldClipboard), isTrue);
      expect(identical(f.session.drive, oldDrive), isTrue);
      expect(identical(f.session.usb, oldUsb), isTrue);
      expect(identical(f.session.printer, oldPrint), isTrue);
      expect([f.clipboard.closes, f.drive.closes, f.usb.closes], [0, 0, 0]);
      expect(
        [f.clipboard.starts, f.drive.starts, f.usb.starts, f.printDials],
        [1, 1, 1, 1],
      );
      expect(f.printSocket.wasClosed, isFalse);
      f.socket.incoming.add(jsonEncode(desktopResizeInfo()));
      await flush();
      expect(f.media.starts, 2);
      expect(f.acks, hasLength(2));
      await f.prepare(
        generation: 2,
        epoch: 3,
        phase: 'signin',
        transition: '2' * 32,
      );
      expect([f.clipboard.closes, f.drive.closes, f.usb.closes], [1, 1, 1]);
      expect(f.printSocket.wasClosed, isTrue);
    } finally {
      await f.close();
    }
  });
  test(
    'Wrapped resize keeps request geometry and intentional encoder downscale',
    () async {
      final f = _Harness(initiallyDesktop: true, encoderLimits: true);
      try {
        await f.start();
        await f.decoded();
        final result = f.session.resizeViewport(1600, 900);
        await flush();
        final request = f.socket.sent.lastWhere(
          (e) => e['type'] == 'stream.resize',
        );
        f.socket.incoming.add(
          jsonEncode({
            'type': 'stream.resizing',
            'requestId': request['requestId'],
            'resizeId': 'resize-2',
            'epoch': 2,
            'width': 1600,
            'height': 900,
            'fps': 60,
            'monitor': '0',
          }),
        );
        await flush();
        final info = desktopResizeInfo();
        info['info'] = {
          ...info['info'] as Map,
          'requestId': request['requestId'],
          'resizeId': 'resize-2',
          'sourceWidth': 1600,
          'sourceHeight': 900,
        };
        f.socket.incoming.add(jsonEncode(info));
        await flush();
        expect(f.session.closed, isFalse);
        expect(f.session.resizing, isTrue);
        expect(f.session.frameReady, isFalse);
        await f.decoded(2);
        expect(await result, isTrue);
        expect(f.session.resizing, isFalse);
        expect(f.session.sourceWidth, 1600);
        expect(f.session.width, 1280);
        expect(f.acks.last['mediaScope'], {'kind': 'composite', 'epoch': 2});
      } finally {
        await f.close();
      }
    },
  );
  test('Initial viewport epoch one does not constrain later scoped desktop metadata', () async {
    final f = _Harness(initialViewport: true);
    try {
      await f.start();
      await f.decoded();
      await f.prepare();
      await f.info();
      await f.decoded(2);
      final info = {
        ...phaseInfo(generation: 2, phase: 'desktop', epoch: 3),
        'firstFrameId': 'c' * 32,
      };
      f.socket.incoming.add(jsonEncode(info));
      await flush();
      await f.decoded(3);
      expect(f.session.closed, isFalse);
      expect(f.session.frameReady, isTrue);
      expect(f.acks, hasLength(3));
      expect(f.session.canResizeViewport, isTrue);
    } finally {
      await f.close();
    }
  });
  test('Negotiated desktop rejects raw normal resize metadata', () async {
    final f = _Harness(initiallyDesktop: true);
    try {
      await f.start();
      await f.decoded();
      f.socket.incoming.add(jsonEncode(desktopResizeInfo()['info']));
      await flush();
      expect(f.session.closed, isTrue);
      expect(f.acks, hasLength(1));
    } finally {
      await f.close();
    }
  });
  test(
    'Duplicate resize info does not extend the bounded decoder wait',
    () async {
      final clock = _ResizeClock();
      final current = _Harness(initiallyDesktop: true, clock: clock);
      try {
        await current.start();
        await current.decoded();
        current.socket.incoming.add(jsonEncode(desktopResizeInfo()));
        await flush();
        clock.now = 14000000;
        current.socket.incoming.add(jsonEncode(desktopResizeInfo()));
        await flush();
        expect(current.media.starts, 2);
        clock.now = 15000001;
        current.socket.incoming.add(signinFrame(2));
        await flush();
        expect(current.session.closed, isTrue);
        expect(current.acks, hasLength(1));
      } finally {
        await current.close();
      }
    },
  );
  test(
    'Delayed previous decoder stats cannot ACK a same-worker resize',
    () async {
      final f = _Harness(initiallyDesktop: true);
      try {
        await f.start();
        await f.decoded();
        f.media.statsReply = Completer<Map<String, dynamic>>();
        await Future<void>.delayed(const Duration(milliseconds: 1100));
        f.socket.incoming.add(jsonEncode(desktopResizeInfo()));
        await flush();
        f.socket.incoming.add(signinFrame(2));
        await flush();
        f.media.statsReply!.complete({'decodedFrames': 9, 'error': 0});
        f.media.statsReply = null;
        await flush();
        expect(f.acks, hasLength(1));
        expect(f.session.frameReady, isFalse);
        f.media.decoded = 1;
        await Future<void>.delayed(const Duration(milliseconds: 65));
        expect(f.acks, hasLength(2));
      } finally {
        await f.close();
      }
    },
  );
  test('Negotiated sign-in refuses unwrapped ordinary metadata', () async {
    final socket = SessionSocket(), media = SigninMedia();
    final session = RemoteSession(
      connection: phaseConnection(),
      token: 'synthetic-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    try {
      unawaited(session.start());
      await flush();
      socket.incoming.add(
        jsonEncode({...phaseInfo()['info'] as Map, 'epoch': 0}),
      );
      await flush();
      expect(session.closed, isTrue);
      expect(media.starts, 0);
    } finally {
      await session.close();
    }
  });
  test('Actual scoped sign-in first decode acknowledges without replacing the session', () async {
    final socket = SessionSocket(), media = SigninMedia();
    final session = RemoteSession(
      connection: phaseConnection(),
      token: 'synthetic-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    try {
      unawaited(session.start());
      await flush();
      socket.incoming.add(jsonEncode(phaseInfo()));
      await flush();
      expect(session.closed, isFalse);
      expect(session.frameReady, isFalse);
      socket.incoming.add(signinFrame());
      await flush();
      media.decoded = 1;
      await Future<void>.delayed(const Duration(milliseconds: 65));
      expect(session.closed, isFalse);
      expect(session.frameReady, isTrue);
      expect(
        socket.sent.where((e) => e['type'] == 'stream.phase.decoded'),
        hasLength(1),
      );
      expect(media.privacyClears, greaterThanOrEqualTo(1));
    } finally {
      await session.close();
    }
  });
  test('Ordinary old-peer stream retains its first frame behavior', () async {
    final socket = SessionSocket(), media = SigninMedia();
    final connection = BrokerConnection.fromJson(
      signinConnectionJson(),
      Uri.parse('https://cloud.example.com'),
    );
    final session = RemoteSession(
      connection: connection,
      token: 'synthetic-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    try {
      unawaited(session.start());
      await flush();
      socket.incoming.add(
        jsonEncode({...phaseInfo()['info'] as Map, 'epoch': 0}),
      );
      await flush();
      socket.incoming.add(signinFrame(0));
      media.decoded = 1;
      await Future<void>.delayed(const Duration(milliseconds: 65));
      expect(session.closed, isFalse);
      expect(session.frameReady, isTrue);
      expect(
        socket.sent.where((e) => e['type'] == 'stream.phase.decoded'),
        isEmpty,
      );
    } finally {
      await session.close();
    }
  });
  test('Sign-in encoder reinitialization stays closed until native privacy is qualified', () async {
    final f = _Harness();
    try {
      await f.start();
      await f.decoded();
      f.socket.incoming.add(
        jsonEncode({
          ...phaseInfo()['info'] as Map,
          'width': 640,
          'height': 360,
        }),
      );
      await flush();
      expect(f.session.closed, isTrue);
      expect(f.media.starts, 1);
    } finally {
      await f.close();
    }
  });
  test('Sign-in to desktop keeps one session/socket and fences input through actual decode', () async {
    final f = _Harness();
    try {
      await f.start();
      await f.decoded();
      final identity = f.session;
      await f.session.capture(true);
      f.session.input({
        'type': 'input',
        'event': 'key',
        'vk': 65,
        'down': true,
      });
      expect(f.inputs.last['workerGeneration'], 1);
      expect(f.session.remoteInputHeld, isTrue);
      await f.prepare();
      expect(f.session.frameReady, isFalse);
      expect(f.session.captured, isFalse);
      expect(f.session.remoteInputHeld, isFalse);
      final inputCount = f.inputs.length;
      f.session.input({
        'type': 'input',
        'event': 'key',
        'vk': 66,
        'down': true,
      });
      expect(f.inputs, inputCount == 0 ? isEmpty : hasLength(inputCount));
      await f.info();
      expect(f.session.frameReady, isFalse);
      expect(f.acks, hasLength(1));
      f.socket.incoming.add(signinFrame(1));
      await flush();
      expect(f.media.packets, hasLength(1));
      await f.decoded(2);
      expect(f.session.frameReady, isTrue);
      expect(f.session.closed, isFalse);
      expect(identical(f.session, identity), isTrue);
      expect(f.session.connection.sessionId, signinSessionId);
      expect(f.dials, 1);
      expect(f.acks, hasLength(2));
      expect(f.acks.last['workerGeneration'], 2);
      await f.session.capture(true);
      f.session.input({
        'type': 'input',
        'event': 'key',
        'vk': 67,
        'down': true,
      });
      expect(f.inputs.last['mediaScope'], {'kind': 'composite', 'epoch': 2});
      expect(
        f.socket.sent.where(
          (e) => [
            'input',
            'input.reset',
            'input.fence',
            'input.direct',
          ].contains(e['type']),
        ),
        isEmpty,
      );
      expect(
        f.media.calls.indexOf('privacy'),
        lessThan(f.media.calls.indexOf('stop')),
      );
    } finally {
      await f.close();
    }
  });
  test('Metadata, keyframe flag and queued pixels cannot replace a real fresh decode', () async {
    final f = _Harness();
    try {
      await f.start();
      final bad = signinFrame();
      bad[16] = 0x61;
      f.socket.incoming.add(bad);
      await flush();
      f.media.decoded = 1;
      await Future<void>.delayed(const Duration(milliseconds: 65));
      expect(f.acks, isEmpty);
      expect(f.session.frameReady, isFalse);
      expect(f.media.packets, isEmpty);
      f.media.decoded = 0;
      f.socket.incoming.add(signinFrame());
      await flush();
      await Future<void>.delayed(const Duration(milliseconds: 65));
      expect(f.acks, isEmpty);
      f.media.decoded = 1;
      await Future<void>.delayed(const Duration(milliseconds: 65));
      expect(f.acks, hasLength(1));
      expect(f.session.frameReady, isTrue);
    } finally {
      await f.close();
    }
  });
  test('All redirect starts are denied in sign-in and old owners retire before a later lock', () async {
    final f = _Harness(redirects: true);
    try {
      await f.start();
      await f.decoded();
      expect(f.clipboard.starts, 0);
      expect(f.drive.starts, 0);
      expect(f.usb.starts, 0);
      expect(f.printDials, 0);
      await f.prepare();
      await f.info();
      await f.decoded(2);
      expect(f.clipboard.starts, 1);
      expect(f.drive.starts, 1);
      expect(f.usb.starts, 1);
      expect(f.printDials, 1);
      f.usb.closing = Completer<void>();
      await f.prepare(
        generation: 3,
        epoch: 3,
        phase: 'signin',
        transition: '3' * 32,
      );
      expect(f.session.frameReady, isFalse);
      expect(f.clipboard.closes, 1);
      expect(f.drive.closes, 1);
      expect(f.usb.closes, 1);
      await f.info(
        generation: 3,
        epoch: 3,
        phase: 'signin',
        transition: '3' * 32,
      );
      expect(
        f.media.starts,
        2,
        reason: 'Held old cleanup blocks the next decoder',
      );
      f.usb.closing!.complete();
      await flush();
      await f.decoded(3);
      expect(f.session.frameReady, isTrue);
      expect(f.media.starts, 3);
      expect(f.clipboard.starts, 1);
      expect(f.drive.starts, 1);
      expect(f.usb.starts, 1);
      expect(f.printDials, 1);
      expect(f.printSocket.wasClosed, isTrue);
    } finally {
      if (f.usb.closing?.isCompleted == false) f.usb.closing!.complete();
      await f.close();
    }
  });
  test(
    'A delayed old decoder result cannot acknowledge the successor phase',
    () async {
      final f = _Harness();
      try {
        await f.start();
        await f.decoded();
        f.media.statsReply = Completer<Map<String, dynamic>>();
        await Future<void>.delayed(const Duration(milliseconds: 1100));
        await f.prepare();
        await f.info();
        f.socket.incoming.add(signinFrame(2));
        await flush();
        f.media.statsReply!.complete({'decodedFrames': 9, 'error': 0});
        f.media.statsReply = null;
        await flush();
        expect(f.acks, hasLength(1));
        expect(f.session.frameReady, isFalse);
        f.media.decoded = 1;
        await Future<void>.delayed(const Duration(milliseconds: 65));
        expect(f.acks, hasLength(2));
      } finally {
        await f.close();
      }
    },
  );
  test(
    'A delayed shortcut cannot be restamped after a phase replacement',
    () async {
      final f = _Harness();
      try {
        await f.start();
        await f.decoded();
        await f.session.capture(true);
        final held = Completer<bool>();
        f.media.releaseCapture = held;
        final oldShortcut = f.session.shortcut('windows');
        await flush();
        f.media.releaseCapture = null;
        await f.prepare();
        await f.info();
        await f.decoded(2);
        final count = f.inputs.length;
        held.complete(true);
        expect(await oldShortcut, isFalse);
        expect(f.inputs, hasLength(count));
      } finally {
        await f.close();
      }
    },
  );
  test(
    'A pending paste from a retired desktop never injects into sign-in',
    () async {
      final f = _Harness(redirects: true, initiallyDesktop: true);
      try {
        await f.start();
        await f.decoded();
        await f.session.capture(true);
        f.clipboard.paste = Completer<bool>();
        f.session.input({
          'type': 'input',
          'event': 'key',
          'vk': 0x11,
          'down': true,
        });
        f.session.input({
          'type': 'input',
          'event': 'key',
          'vk': 0x56,
          'down': true,
        });
        await f.prepare(phase: 'signin');
        await f.info(phase: 'signin');
        await f.decoded(2);
        final count = f.inputs.length;
        f.clipboard.paste!.complete(true);
        await flush();
        expect(f.inputs, hasLength(count));
        expect(f.session.frameReady, isTrue);
      } finally {
        await f.close();
      }
    },
  );
  test(
    'Privacy failure closes the exact phase before pixels or readiness',
    () async {
      final f = _Harness();
      try {
        f.media.privacyFails = true;
        await f.start();
        await flush();
        expect(f.session.closed, isTrue);
        expect(f.acks, isEmpty);
        expect(f.media.packets, isEmpty);
      } finally {
        await f.close();
      }
    },
  );
  test(
    'Closing while privacy completes cannot revive the retired decoder',
    () async {
      final f = _Harness();
      try {
        f.media.privacyReply = Completer<void>();
        await f.start();
        await f.close();
        f.media.privacyReply!.complete();
        await flush();
        expect(f.session.closed, isTrue);
        expect(f.session.frameReady, isFalse);
        expect(f.acks, isEmpty);
      } finally {
        await f.close();
      }
    },
  );
}
