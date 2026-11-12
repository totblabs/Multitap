import 'broker_transport.dart';
import 'broker_probe.dart';

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:crypto/crypto.dart';

import 'broker_api.dart';
import 'console_rfb.dart';
import '../l10n/l10n.dart';

abstract interface class ConsoleTransport {
  Stream<Object?> get messages;
  void send(Object value);
  Future<void> close();
}

class _WebConsoleTransport implements ConsoleTransport {
  _WebConsoleTransport(this.socket);
  final WebSocket socket;
  @override
  Stream<Object?> get messages => socket;
  @override
  void send(Object value) => socket.add(value);
  @override
  Future<void> close() async {
    await socket.close();
  }
}

typedef ConsoleTransportFactory = Future<ConsoleTransport> Function(
  Uri relay,
  String token,
);

Future<ConsoleTransport> openConsoleTransport(
  Uri relay,
  String token, {
  HttpClient? client,
}) async {
  if (!brokerRelayAllowed(relay) ||
      relay.host.isEmpty ||
      relay.userInfo.isNotEmpty ||
      relay.hasQuery ||
      relay.hasFragment ||
      !RegExp(
        r'^/relay/console/[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$',
      ).hasMatch(relay.path)) {
    client?.close(force: true);
    throw const WebSocketException('console_auth_failed');
  }
  // Explicit same-origin HTTPS upgrade. Unlike HttpClient's default behavior,
  // a redirect cannot forward this Bearer header to a different endpoint.
  final http =
      client ?? (HttpClient()..connectionTimeout = const Duration(seconds: 10));
  HttpClientRequest? request;
  Socket? socket;
  try {
    final origin = brokerOriginForRelay(relay);
    final admission = BrokerTransport.token(origin);
    if (origin.scheme == 'http') {
      await ensureBrokerTransport(origin, client: http, force: true);
    }
    BrokerTransport.requireCurrent(origin, admission);
    final nonce = base64Encode(
      List.generate(16, (_) => Random.secure().nextInt(256)),
    );
    request = await http
        .getUrl(relay.replace(scheme: origin.scheme))
        .timeout(const Duration(seconds: 10));
    BrokerTransport.requireCurrent(origin, admission);
    request.followRedirects = false;
    request.headers
      ..set(HttpHeaders.authorizationHeader, 'Bearer $token')
      ..set(HttpHeaders.connectionHeader, 'Upgrade')
      ..set(HttpHeaders.upgradeHeader, 'websocket')
      ..set('Sec-WebSocket-Key', nonce)
      ..set('Sec-WebSocket-Version', '13');
    final response = await request.close().timeout(const Duration(seconds: 10));
    if (response.statusCode == HttpStatus.unauthorized ||
        response.statusCode == HttpStatus.forbidden) {
      throw BrokerException('console_auth_failed', response.statusCode);
    }
    final expected = base64Encode(
      sha1
          .convert(
            ascii.encode(
              '$nonce'
              '258EAFA5-E914-47DA-95CA-C5AB0DC85B11',
            ),
          )
          .bytes,
    );
    final connection = response.headers
        .value(HttpHeaders.connectionHeader)
        ?.toLowerCase()
        .split(',')
        .map((v) => v.trim());
    if (response.statusCode != 101 ||
        connection?.contains('upgrade') != true ||
        response.headers.value(HttpHeaders.upgradeHeader)?.toLowerCase() !=
            'websocket' ||
        response.headers.value('Sec-WebSocket-Accept') != expected ||
        response.headers.value('Sec-WebSocket-Extensions') != null ||
        response.headers.value('Sec-WebSocket-Protocol') != null) {
      throw const WebSocketException('console_auth_failed');
    }
    // detachSocket removes the connection from HttpClient ownership before
    // its stream hand-off completes. Closing the client cannot retire a late
    // detached socket, so retain that exact future after our observer expires.
    var abandoned = false;
    final detached = response.detachSocket();
    unawaited(
      detached.then<void>((late) {
        if (abandoned) {
          late.destroy();
        }
      }, onError: (Object _) {}),
    );
    try {
      socket = await detached.timeout(
        const Duration(seconds: 10),
        onTimeout: () {
          abandoned = true;
          throw TimeoutException('console socket hand-off');
        },
      );
    } catch (_) {
      abandoned = true;
      rethrow;
    }
    final webSocket = WebSocket.fromUpgradedSocket(
      socket,
      serverSide: false,
      compression: CompressionOptions.compressionOff,
      maxPayloadLength: ConsoleRfb.maxChunk,
    );
    socket = null;
    return _WebConsoleTransport(webSocket);
  } catch (_) {
    request?.abort();
    socket?.destroy();
    rethrow;
  } finally {
    http.close(force: true);
  }
}

String consoleFailureMessage(String? code) => switch (code) {
  'console_busy' => l10n.desktopHasAnotherConnectionDisconnect,
  'console_permission_denied' ||
  'authorization_revoked' ||
  'desktop_not_found' => l10n.youDontHaveConsolePermission,
  'session_expired' => l10n.signHasExpiredSignAgain2,
  'console_vm_unavailable' => l10n.desktopIsntSoConsoleCant,
  'console_invalid_upstream' =>
    l10n.couldntGetConsoleConnectionDetails,
  'console_identity_changed' => l10n.desktopDetailsHaveChangedClose,
  'console_unsupported' => l10n.serverDoesntSupportConsole,
  'console_certificate_error' => l10n.serverCouldntVerifyProxmoxCertificate,
  'console_identity_unavailable' =>
    l10n.serverCouldntVerifyDesktopsProxmox,
  'console_limit_reached' ||
  'console_limits_exceeded' ||
  'console_backpressure' => l10n.consoleIsntAvailableRightNow,
  'console_invalid_frame' ||
  'console_auth_failed' => l10n.couldntVerifyConsoleConnectionLet,
  'console_timeout' => l10n.didntReceiveConsoleScreenReconnect,
  'console_frame_timeout' => l10n.didntReceiveConsoleScreenReconnect2,
  _ => l10n.consoleDisconnectedReconnect,
};

/// One explicit connection attempt. No automatic reacquisition/retry or takeover.
class EmergencyConsoleSession extends ChangeNotifier {
  EmergencyConsoleSession({
    required this.acquire,
    required this.release,
    required this.token,
    required this.origin,
    this.transportFactory = openConsoleTransport,
    this.deadline = const Duration(seconds: 20),
  }) {
    rfb = ConsoleRfb(send: _sendBytes)..addListener(_changed);
  }
  final Future<BrokerConsoleGrant> Function() acquire;
  final Future<void> Function(String id) release;
  final String token;
  final Uri origin;
  final ConsoleTransportFactory transportFactory;
  final Duration deadline;
  late final ConsoleRfb rfb;
  ConsoleTransport? _transport;
  BrokerConsoleGrant? _grant;
  StreamSubscription<Object?>? _subscription;
  Timer? _timer;
  bool _begun = false, _closed = false, _ready = false, _disposed = false;
  bool _retiring = false;
  bool _nativeKeyboardRequested = false;
  Future<void>? _retirement;
  String? failureCode;
  final _released = <String>{};
  bool get closed => _closed;
  bool get connected => _ready && rfb.frameValid && !_closed;
  bool get waiting => !_closed && !rfb.frameValid;

  /// Windows view opt-in only. Shared/mobile callers retain the legacy wire.
  void requestNativeKeyboardProtocol() {
    if (!_begun && !_closed) _nativeKeyboardRequested = true;
  }

  bool get canRetry =>
      _closed &&
      !{
        'authorization_revoked',
        'console_permission_denied',
        'desktop_not_found',
        'session_expired',
        'console_unsupported',
        'console_auth_failed',
        'console_identity_changed',
      }.contains(failureCode);

  void _changed() {
    if (rfb.frameValid) {
      _timer?.cancel();
      _timer = null;
    } else if (!_closed && !_retiring && _ready && rfb.initialized) {
      // DesktopSize invalidates the picture after the initial deadline ended.
      // ACKs/LEDs cannot extend this wait, and a static valid frame needs no timer.
      _timer ??= Timer(deadline, () => _fail('console_frame_timeout'));
    }
    if (!_disposed) notifyListeners();
  }

  Future<void> start() async {
    if (_begun || _closed) return;
    _begun = true;
    _timer = Timer(deadline, () => _fail('console_timeout'));
    try {
      final grant = await acquire();
      if (_closed) {
        unawaited(_release(grant.id));
        return;
      }
      _grant = grant;
      if (!grant.validFor(origin)) {
        _fail('console_auth_failed');
        return;
      }
      final transport = await transportFactory(grant.relay, token);
      if (_closed) {
        unawaited(transport.close().catchError((Object _) {}));
        return;
      }
      _transport = transport;
      _subscription = transport.messages.listen(
        _message,
        onError: (Object _) => _fail('console_transport_closed'),
        onDone: () => _fail('console_transport_closed'),
      );
      transport.send(
        jsonEncode({'type': 'console-auth', 'ticket': grant.ticket}),
      );
    } on BrokerException catch (error) {
      _fail(
        error.status == HttpStatus.unauthorized
            ? 'session_expired'
            : error.status == HttpStatus.forbidden
            ? 'console_permission_denied'
            : error.code,
      );
    } on TimeoutException {
      _fail('console_timeout');
    } catch (_) {
      _fail('console_transport_closed');
    }
  }

  void _message(Object? value) {
    if (_closed) return;
    try {
      if (value is String) {
        if (value.length > 2048) throw const FormatException();
        final json = jsonDecode(value);
        if (json is! Map) throw const FormatException();
        if (!_ready &&
            json['type'] == 'console-ready' &&
            json['protocol'] == 'rfb-none-v1') {
          if (_nativeKeyboardRequested &&
              json['keyboardProtocol'] == 'qemu-keyboard-v1') {
            rfb.enableQemuKeyboard();
          }
          _ready = true;
          _changed();
          return;
        }
        if (json['type'] == 'console-ended') {
          final code = json['code'];
          const allowed = {
            'console_busy',
            'console_permission_denied',
            'console_identity_changed',
            'console_vm_unavailable',
            'console_auth_failed',
            'console_timeout',
            'console_transport_closed',
            'console_backpressure',
            'authorization_revoked',
            'session_expired',
            'console_identity_unavailable',
            'console_certificate_error',
            'console_invalid_upstream',
            'console_crypto_unavailable',
            'console_rfb_unsupported',
            'console_protocol_error',
            'console_clipboard_denied',
            'console_limits_exceeded',
            'console_limit_reached',
            'console_cancelled',
            'console_unavailable',
          };
          _fail(
            code is String && allowed.contains(code)
                ? code
                : 'console_transport_closed',
          );
          return;
        }
        throw const FormatException();
      }
      if (!_ready || value is! List<int>) throw const FormatException();
      rfb.add(value);
    } catch (_) {
      _fail('console_invalid_frame');
    }
  }

  void _sendBytes(Uint8List message) {
    if (!_closed && _ready) {
      try {
        _transport?.send(message);
      } catch (_) {
        _fail('console_transport_closed');
      }
    }
  }

  void _fail(String code) {
    if (_closed || _retiring) return;
    failureCode = code;
    unawaited(close());
  }

  Future<void> _release(String id) async {
    if (!_released.add(id)) return;
    try {
      await release(id).timeout(const Duration(seconds: 3));
    } catch (_) {
      // Exact WS closure/expired one-use grant also retires the broker lease.
      // An unobserved DELETE acknowledgement is never represented as success.
    }
  }

  Future<void> close() => _retirement ??= _close();
  Future<void> _close() async {
    if (_closed) return;
    _retiring = true;
    rfb.releaseInput(); // send ups before close, only to this transport.
    _closed = true;
    _timer?.cancel();
    _timer = null;
    rfb.close();
    final transport = _transport, grant = _grant;
    _transport = null;
    unawaited(_subscription?.cancel());
    _subscription = null;
    if (!_disposed) notifyListeners();
    await Future.wait([
      if (transport != null)
        transport
            .close()
            .timeout(const Duration(seconds: 3))
            .catchError((Object _) {}),
      if (grant != null) _release(grant.id),
    ]);
  }

  @override
  void dispose() {
    _disposed = true;
    unawaited(close());
    rfb.removeListener(_changed);
    rfb.dispose();
    super.dispose();
  }
}
