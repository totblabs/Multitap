import 'broker_transport.dart';

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import '../models/vm_resources.dart';
import 'notices.dart' show noticeLiveFeature;
import 'relay_socket.dart';

abstract interface class DesktopUpdates implements Listenable {
  bool get connected;
  Stream<void> get changes;
  void start();
  Future<void> close();
}

/// One VM's pushed Proxmox sample (null [resources]: the VM left this view).
class LiveResourceSample {
  const LiveResourceSample(this.id, this.resources);
  final String id;
  final VmResources? resources;
}

/// The broker's optional live pushes besides list invalidations (asked for
/// with the x-totb-live header; a released broker sends none): changed
/// CPU/RAM/uptime values, and a change of the public download catalog.
abstract interface class LiveDesktopFeeds {
  Stream<List<LiveResourceSample>> get resources;
  Stream<void> get catalog;
}

/// notice-v1 (notices.dart): the broker says this account has notices to
/// read. Contentless; a released broker never sends it.
abstract interface class NoticeFeed {
  Stream<void> get notices;
}

/// Optional fixed terminal authentication notice; legacy feeds need no change.
abstract interface class TerminalDesktopAuthFeed {
  Stream<String> get terminalAuthentication;
}

typedef DesktopWatchFactory = DesktopUpdates Function(Uri origin, String token);
typedef DesktopWatchDialer = Future<WebSocket> Function(
  Uri url,
  String token,
  Uri origin,
);

Future<WebSocket> _dial(Uri url, String token, Uri origin) =>
    connectRelaySocket(
      url,
      token,
      origin: origin,
      maxPayloadLength: _maxResources,
      headers: const {'x-totb-live': 'resources,catalog,$noticeLiveFeature'},
    );

const _maxControl = 2048, _maxResources = 65536;

/// A read-only invalidation channel. Detailed VM state is still fetched through
/// the same authenticated HTTP API; events contain no credentials or VM data.
class BrokerDesktopWatch extends ChangeNotifier
    implements
        DesktopUpdates,
        LiveDesktopFeeds,
        NoticeFeed,
        TerminalDesktopAuthFeed {
  BrokerDesktopWatch({
    required this.origin,
    required this._token,
    DesktopWatchDialer? dialer,
  }) : _dialer = dialer ?? _dial {
    if (!brokerOriginAllowed(origin) ||
        origin.host.isEmpty ||
        origin.userInfo.isNotEmpty ||
        origin.hasQuery ||
        origin.hasFragment ||
        (origin.path.isNotEmpty && origin.path != '/')) {
      throw ArgumentError(
        'A desktop watch requires the configured HTTPS broker.',
      );
    }
  }
  final Uri origin;
  final String _token;
  final DesktopWatchDialer _dialer;
  final _changes = StreamController<void>.broadcast();
  final _resources = StreamController<List<LiveResourceSample>>.broadcast();
  final _catalog = StreamController<void>.broadcast();
  final _notices = StreamController<void>.broadcast();
  final _terminalAuthentication = StreamController<String>.broadcast();
  bool _authRetired = false;
  WebSocket? _socket;
  StreamSubscription<dynamic>? _subscription;
  Timer? _retry, _readyDeadline;
  bool _started = false, _closed = false, _connected = false;
  int _generation = 0, _attempts = 0, _revision = 0;
  int _resourcesRevision = 0, _catalogRevision = 0, _noticesRevision = 0;
  Future<void>? _closing;
  final _socketClosures = Expando<Future<void>>();
  @override
  bool get connected => _connected;
  @override
  Stream<void> get changes => _changes.stream;
  @override
  Stream<List<LiveResourceSample>> get resources => _resources.stream;
  @override
  Stream<void> get catalog => _catalog.stream;
  @override
  Stream<void> get notices => _notices.stream;
  @override
  Stream<String> get terminalAuthentication => _terminalAuthentication.stream;
  @override
  void start() {
    if (_started || _closed) return;
    _started = true;
    unawaited(_connect());
  }

  Future<void> _connect() async {
    if (_closed || _authRetired) return;
    final generation = ++_generation;
    _revision = _resourcesRevision = _catalogRevision = _noticesRevision = 0;
    final url = origin.replace(
      scheme: brokerRelayScheme(origin),
      path: '/api/v1/events',
    );
    try {
      final pending = _dialer(url, _token, origin);
      // A late completed handshake after timeout/logout must release its socket.
      unawaited(
        pending.then((socket) async {
          if (_closed || generation != _generation) await _closeSocket(socket);
        }, onError: (Object _) {}),
      );
      final socket = await pending.timeout(const Duration(seconds: 12));
      if (_closed || generation != _generation) {
        await _closeSocket(socket);
        return;
      }
      _socket = socket;
      socket.pingInterval = const Duration(seconds: 10);
      _readyDeadline = Timer(
        const Duration(seconds: 10),
        () => _lost(generation),
      );
      _subscription = socket.listen(
        (message) {
          if (_closed || generation != _generation) return;
          try {
            if (message is! String || message.length > _maxResources) {
              throw const FormatException();
            }
            final value = jsonDecode(message);
            if (value is! Map<String, dynamic>) throw const FormatException();
            if (message.length > _maxControl && value['type'] != 'resources') {
              throw const FormatException();
            }
            switch (value['type']) {
              case 'desktops.ready':
                if (_connected) throw const FormatException();
                _connected = true;
                _attempts = 0;
                _readyDeadline?.cancel();
                notifyListeners();
                _changes.add(null);
              case 'desktops.changed':
                final revision = value['revision'];
                if (!_connected ||
                    revision is! int ||
                    revision < 1 ||
                    revision > 9007199254740991) {
                  throw const FormatException();
                }
                if (revision <= _revision) return;
                _revision = revision;
                _changes.add(null);
              case 'resources':
                _resourcesMessage(value);
              case 'catalog':
                final revision = value['revision'];
                if (!_connected || revision is! int || revision < 1) {
                  throw const FormatException();
                }
                if (revision <= _catalogRevision) return;
                _catalogRevision = revision;
                _catalog.add(null);
              case 'notices':
                final revision = value['revision'];
                if (!_connected || revision is! int || revision < 1) {
                  throw const FormatException();
                }
                if (revision <= _noticesRevision) return;
                _noticesRevision = revision;
                _notices.add(null);
              default:
                throw const FormatException();
            }
          } catch (_) {
            _lost(generation);
          }
        },
        onError: (Object _) => _lost(generation),
        onDone: () => _lost(generation),
      );
    } catch (_) {
      _lost(generation);
    }
  }

  void _resourcesMessage(Map<String, dynamic> value) {
    final revision = value['revision'], list = value['desktops'];
    if (!_connected ||
        revision is! int ||
        revision < 1 ||
        list is! List ||
        list.length > 100) {
      throw const FormatException();
    }
    final received = DateTime.now().toUtc();
    final samples = <LiveResourceSample>[];
    for (final item in list) {
      if (item is! Map) throw const FormatException();
      final id = item['id'], resources = item['resources'];
      if (id is! String || id.isEmpty || id.length > 128) {
        throw const FormatException();
      }
      if (resources != null && resources is! Map) throw const FormatException();
      samples.add(
        LiveResourceSample(
          id,
          resources == null
              ? null
              : VmResources.fromJson(resources, receivedAt: received),
        ),
      );
    }
    if (revision <= _resourcesRevision) return;
    _resourcesRevision = revision;
    if (samples.isNotEmpty) _resources.add(samples);
  }

  void _lost(int generation) {
    if (_closed || generation != _generation) return;
    // This is the account-owned /events connection. Its server closes with
    // these fixed reasons only after the bearer/user identity is invalid.
    // Per-VM stream authorization_revoked is deliberately handled elsewhere.
    final reason = _socket?.closeReason;
    final terminal = switch (reason) {
      'pc_login_replaced' || 'session_idle_timeout' => reason,
      'session_expired' ||
      'authentication_required' ||
      'authorization_revoked' => 'session_reauthentication_required',
      _ => null,
    };
    if (!_authRetired && _socket?.closeCode == 1008 && terminal != null) {
      _authRetired = true;
      _terminalAuthentication.add(terminal);
    }
    _generation++;
    _readyDeadline?.cancel();
    final subscription = _subscription, socket = _socket;
    _subscription = null;
    _socket = null;
    if (subscription != null) unawaited(_cancel(subscription));
    if (socket != null) unawaited(_closeSocket(socket));
    if (_connected) {
      _connected = false;
      notifyListeners();
    }
    _retry?.cancel();
    if (_authRetired) return;
    final seconds = [1, 2, 4, 8, 15][_attempts.clamp(0, 4)];
    _attempts++;
    _retry = Timer(Duration(seconds: seconds), () => unawaited(_connect()));
  }

  Future<void> _cancel(StreamSubscription<dynamic> subscription) async {
    try {
      await subscription.cancel().timeout(const Duration(seconds: 2));
    } catch (_) {}
  }

  Future<void> _closeSocket(WebSocket socket) =>
      _socketClosures[socket] ??= () async {
        try {
          await socket.close().timeout(const Duration(seconds: 2));
        } catch (_) {}
      }();

  @override
  Future<void> close() => _closing ??= _close();
  Future<void> _close() async {
    _closed = true;
    _generation++;
    _connected = false;
    _readyDeadline?.cancel();
    _retry?.cancel();
    final subscription = _subscription;
    _subscription = null;
    final socket = _socket;
    _socket = null;
    await Future.wait([
      if (subscription != null) _cancel(subscription),
      if (socket != null) _closeSocket(socket),
    ]);
    try {
      await Future.wait([
        _changes.close(),
        _resources.close(),
        _catalog.close(),
        _notices.close(),
        _terminalAuthentication.close(),
      ]).timeout(const Duration(seconds: 2));
    } catch (_) {}
    super.dispose();
  }
}
