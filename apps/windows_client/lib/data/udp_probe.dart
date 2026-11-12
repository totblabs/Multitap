import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';

/// udp-media-v1, docs/remote-session-protocol.md ("UDP media"). The socket lives in the
/// native leg (windows/runner/udp_media_leg.h) because the relay pins the
/// address of whichever socket sent BIND. Dart stays in charge of control and
/// everything here catches everything: nothing UDP does reaches the session
/// except media the leg delivered and the fixed numbers of [UdpMedia.report].
const udpMediaProtocol = 'udp-media-v1';
const udpDirectProtocol = 'udp-direct-v1';
const udpTracksProtocol = 'udp-tracks-v1';
bool udpTracksAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['udpTracksProtocol'] == udpTracksProtocol;

/// One immutable output grant. A replacement with identical epochs still has a
/// different generation, so delayed keys cannot engage its new receive socket.
class UdpTrackScope {
  const UdpTrackScope(
    this.track,
    this.topology,
    this.epoch,
    this.generation,
    this.primary,
  );
  final String track;
  final int topology, epoch, generation;
  final bool primary;
  Map<String, Object> get fields => {
    'track': track,
    'topologyEpoch': topology,
    'epoch': epoch,
    'generation': generation,
  };
  bool matches(Map<String, dynamic> event) =>
      fields.entries.every((entry) => event[entry.key] == entry.value);
}

/// Shared by every leg and regrant of one remote session.
class DirectInputSequence {
  int _value = 0;
  int? next() => _value < 0xffffffff ? ++_value : null;
}

bool udpDirectAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['udpDirectProtocol'] == udpDirectProtocol;

/// Released brokers reject this connect field and strip this capability, so a
/// client that does not see it here must not send the field.
bool udpMediaAdvertised(Object? capabilities) =>
    capabilities is Map && capabilities['udpMediaProtocol'] == udpMediaProtocol;

/// `udpProbe` report values; 0 (no grant) is the field's absence.
const udpProbeReachable = 1,
    udpProbeSocketError = 2,
    udpProbeLookupFailed = 3,
    udpProbeBindTimeout = 4,
    udpProbeNoPong = 5;

// 600 ms per relay: eight of them fit the 5 s budget once around.
const _maxRelays = 8, _idBytes = 8, _ticketBytes = 32, _publicKeyBytes = 32;
const _readingInterval = Duration(milliseconds: 100);
final _relayHost = RegExp(r'^[A-Za-z0-9.-]{1,253}$');

/// One leg's `udp.grant`. Holds the ticket, a secret: never log it.
class UdpGrant {
  UdpGrant._(
    this.relays,
    this.id,
    this.ticket,
    this.keys,
    this.direct,
    this.stun,
  );
  final List<(String, int)> relays;
  final List<(String, int)> stun;
  final Uint8List id, ticket;

  /// This Broker forwards `udp.key`. Released ones end the session over it.
  final bool keys;
  final bool direct;

  /// Null for anything but the exact contract. [event]'s `expiresAt` is only
  /// checked for shape: the relay decides by its own clock, not this PC's.
  static UdpGrant? parse(Map<String, dynamic> event) {
    final relays = event['relays'], expiresAt = event['expiresAt'];
    final id = _decoded(event['id'], _idBytes);
    final ticket = _decoded(event['ticket'], _ticketBytes);
    if (event['protocol'] != udpMediaProtocol ||
        id == null ||
        ticket == null ||
        expiresAt is! int ||
        expiresAt <= 0 ||
        relays is! List ||
        relays.isEmpty ||
        relays.length > _maxRelays) {
      return null;
    }
    final parsed = <(String, int)>[];
    for (final relay in relays) {
      if (relay is! Map) return null;
      final host = relay['host'], port = relay['port'];
      if (host is! String ||
          !_relayHost.hasMatch(host) ||
          port is! int ||
          port < 1 ||
          port > 65535) {
        return null;
      }
      parsed.add((host, port));
    }
    final direct =
        event['keys'] == true && event['directProtocol'] == udpDirectProtocol;
    final servers = event['stun'] ?? const [];
    if (direct && (servers is! List || servers.length > 2)) return null;
    final stun = <(String, int)>[];
    if (direct) {
      for (final value in servers as List) {
        if (value is! Map) return null;
        final host = value['host'], port = value['port'];
        if (host is! String ||
            !_relayHost.hasMatch(host) ||
            port is! int ||
            port < 1 ||
            port > 65535) {
          return null;
        }
        stun.add((host, port));
      }
    }
    return UdpGrant._(
      List.unmodifiable(parsed),
      id,
      ticket,
      event['keys'] == true,
      direct,
      List.unmodifiable(stun),
    );
  }

  static Uint8List? _decoded(Object? value, int length) {
    if (value is! String) return null;
    try {
      final bytes = base64.decode(value);
      return bytes.length == length && base64.encode(bytes) == value
          ? bytes
          : null;
    } on FormatException {
      return null;
    }
  }
}

/// One reading of the leg. [arrivals] is what it delivered to the decoder since
/// the last one: (pts, arrival on the session clock, bytes).
class UdpLegReading {
  const UdpLegReading({
    this.report = const {},
    this.running = true,
    this.publicKey,
    this.arrivals = const [],
    this.audioBytes = 0,
    this.keyedStream = 0,
    this.local,
    this.reflexive,
  });
  final Map<String, int> report;
  final bool running;
  final Uint8List? publicKey;
  final List<(int, int, int)> arrivals;
  final int audioBytes;
  final (String, int)? local;
  final (String, int)? reflexive;

  /// The stream whose first key frame came over UDP; see [UdpMedia.stream].
  final int keyedStream;
}

/// The native leg: [NativeUdpLeg] outside tests.
abstract interface class UdpLeg {
  Future<void> start(UdpGrant grant, String sessionId, int clockUs);
  Future<void> stream(int epoch, int generation);

  /// Null when another session's leg replaced this one.
  Future<UdpLegReading?> poll();
  Future<void> hostKey(Uint8List key, {required bool keyed});
  Future<void> peer(List<(String, int)> candidates);
  Future<void> input(int sequence, int epoch, Uint8List bytes);
  Future<void> stop();
}

class NativeUdpLeg implements UdpLeg {
  NativeUdpLeg({this.scope});
  final UdpTrackScope? scope;
  static const _channel = MethodChannel('multitap/udp');
  static int _next = 0;
  final _leg = ++_next;

  @override
  Future<void> start(UdpGrant grant, String sessionId, int clockUs) =>
      _channel.invokeMethod<void>('start', {
        'leg': _leg,
        'relays': [
          for (final (host, port) in grant.relays) {'host': host, 'port': port},
        ],
        'id': grant.id,
        'ticket': grant.ticket,
        'sessionId': sessionId,
        'clockUs': clockUs,
        if (scope != null) ...scope!.fields,
        if (scope != null) 'primary': scope!.primary,
        'direct': grant.direct,
        'stun': [
          for (final (host, port) in grant.stun) {'host': host, 'port': port},
        ],
      });

  @override
  Future<void> stream(int epoch, int generation) => _channel.invokeMethod<void>(
    'stream',
    {'leg': _leg, 'epoch': epoch, 'generation': generation},
  );

  @override
  Future<UdpLegReading?> poll() async {
    final value = await _channel.invokeMapMethod<String, Object?>('poll', {
      'leg': _leg,
    });
    if (value == null) return null;
    final flat = value['arrivals'] as List<Object?>;
    return UdpLegReading(
      report: (value['report'] as Map).cast<String, int>(),
      running: value['running'] as bool,
      publicKey: value['publicKey'] as Uint8List?,
      arrivals: [
        for (var i = 0; i + 2 < flat.length; i += 3)
          (flat[i] as int, flat[i + 1] as int, flat[i + 2] as int),
      ],
      audioBytes: value['audioBytes'] as int,
      keyedStream: value['keyedStream'] as int,
      local: value['local'] is Map
          ? (
              (value['local'] as Map)['address'] as String,
              (value['local'] as Map)['port'] as int,
            )
          : null,
      reflexive: value['reflexive'] is Map
          ? (
              (value['reflexive'] as Map)['address'] as String,
              (value['reflexive'] as Map)['port'] as int,
            )
          : null,
    );
  }

  @override
  Future<void> hostKey(Uint8List key, {required bool keyed}) => _channel
      .invokeMethod<void>('hostKey', {'leg': _leg, 'key': key, 'keyed': keyed});

  @override
  Future<void> stop() => _channel.invokeMethod<void>('stop', {'leg': _leg});
  @override
  Future<void> input(int sequence, int epoch, Uint8List bytes) =>
      _channel.invokeMethod<void>('input', {
        'leg': _leg,
        'sequence': sequence,
        'epoch': epoch,
        'bytes': bytes,
      });
  @override
  Future<void> peer(List<(String, int)> candidates) =>
      _channel.invokeMethod<void>('peer', {
        'leg': _leg,
        'candidates': [
          for (final (address, port) in candidates)
            {'address': address, 'port': port},
        ],
      });
}

/// One grant's leg, as the session sees it.
class UdpMedia {
  UdpMedia(
    this._grant,
    this._leg, {
    required this.sessionId,
    required this.nowUs,
    required this.mediaAllowed,
    required this.sendKey,
    required this.delivered,
    required this.keyed,
    this.sendLocal,
    this.sendReflexive,
    DirectInputSequence? inputSequence,
  }) : _inputSequence = inputSequence ?? DirectInputSequence();
  final UdpGrant _grant;
  final UdpLeg _leg;
  final String sessionId;
  final int Function() nowUs;

  /// False when this session keeps media on WebSocket: no key exchange means
  /// the Host never switches. Track grants pace their own fixed bitrate.
  final bool mediaAllowed;
  final void Function(Uint8List publicKey) sendKey;
  final void Function(UdpLegReading reading) delivered;

  /// This stream's first key frame reached the decoder over UDP.
  final void Function() keyed;
  final void Function(String, int)? sendLocal;
  final void Function(String, int)? sendReflexive;
  bool _reflexiveSent = false;
  final DirectInputSequence _inputSequence;
  Map<String, dynamic>? wrapInput(Map<String, dynamic> input, int epoch) {
    if (_closed ||
        !_grant.direct ||
        !mediaAllowed ||
        !['input', 'input.reset', 'input.fence'].contains(input['type'])) {
      return null;
    }
    final sequence = _inputSequence.next();
    if (sequence == null) return null;
    final wrapped = {
      'type': 'input.direct',
      'protocol': udpDirectProtocol,
      'sequence': sequence,
      'epoch': epoch,
      'input': input,
    };
    return utf8.encode(jsonEncode(wrapped)).length <= 8192 ? wrapped : null;
  }

  void fastInput(Map<String, dynamic> wrapped) {
    final input = wrapped['input'] as Map<String, dynamic>;
    if (input['type'] != 'input.reset' &&
        ![
          'key',
          'move',
          'moveRelative',
          'button',
          'wheel',
        ].contains(input['event'])) {
      return;
    }
    final bytes = Uint8List.fromList(utf8.encode(jsonEncode(input)));
    // One protected datagram (1167 plaintext bytes minus the 9-byte input header).
    if (bytes.length > 1158) return;
    _call(
      () => _leg.input(
        wrapped['sequence'] as int,
        wrapped['epoch'] as int,
        bytes,
      ),
    );
  }

  bool _localSent = false;
  void peer(Map<String, dynamic> event) {
    if (_closed ||
        !_grant.direct ||
        event['protocol'] != udpDirectProtocol ||
        event['sessionId'] != sessionId) {
      return;
    }
    final rows = event['candidates'];
    if (rows is! List || rows.length > 5) return;
    final parsed = <(String, int)>[];
    for (final row in rows) {
      if (row is! Map) return;
      final address = row['address'], port = row['port'];
      if (address is! String ||
          !_ipv4(address) ||
          port is! int ||
          port < 1 ||
          port > 65535) {
        return;
      }
      parsed.add((address, port));
    }
    _call(() => _leg.peer(parsed));
  }

  /// Fixed numbers only. Empty while the outcome is open, like an absent metric.
  Map<String, int> report = const {};
  Timer? _timer;
  bool _closed = false, _reading = false, _hostKey = false, _keySent = false;
  Uint8List? _publicKey;
  int _generation = 0;

  void start(int epoch) {
    stream(epoch);
    _call(() async {
      // It works once and the leg has it now: nothing keeps it for the session.
      try {
        await _leg.start(_grant, sessionId, nowUs());
      } finally {
        _grant.ticket.fillRange(0, _grant.ticket.length, 0);
      }
    });
    _timer = Timer.periodic(_readingInterval, (_) => unawaited(_read()));
  }

  /// A stream run was announced, or its decoder is about to restart: both paths
  /// start over at this epoch. Call before the decoder starts, which is before
  /// any of its frames can pass.
  void stream(int epoch) {
    if (_closed) return;
    final generation = ++_generation;
    _call(() => _leg.stream(epoch, generation));
  }

  /// The Host's answer to [sendKey], once per grant: the Host never speaks
  /// first. True means both paths now follow the path rule. [hasKeyFrame]:
  /// this decoder already took a key frame on WebSocket.
  bool hostKey(Map<String, dynamic> event, {required bool hasKeyFrame}) {
    if (_closed || !_keySent || _hostKey) return false;
    final key = UdpGrant._decoded(event['publicKey'], _publicKeyBytes);
    if (key == null || event['protocol'] != udpMediaProtocol) return false;
    _hostKey = true;
    _call(() => _leg.hostKey(key, keyed: hasKeyFrame));
    return true;
  }

  /// The client speaks first, once the probe passed, and only on the grant's
  /// marker: without it the session stays on the probe.
  void _sendKey() {
    final key = _publicKey;
    if (_closed || _keySent || !mediaAllowed || !_grant.keys || key == null) {
      return;
    }
    _keySent = true;
    sendKey(key);
  }

  Future<void> _read() async {
    if (_closed || _reading) return;
    _reading = true;
    try {
      final reading = await _leg.poll();
      if (_closed || reading == null) return;
      report = reading.report;
      if (_grant.direct && !_reflexiveSent && reading.reflexive != null) {
        _reflexiveSent = true;
        final (address, port) = reading.reflexive!;
        sendReflexive?.call(address, port);
      }
      if (_grant.direct && !_localSent && reading.local != null) {
        _localSent = true;
        final (address, port) = reading.local!;
        sendLocal?.call(address, port);
      }
      _publicKey ??= reading.publicKey;
      _sendKey();
      if (reading.keyedStream == _generation) keyed();
      delivered(reading);
      if (!reading.running) _timer?.cancel();
    } catch (_) {
      // The native leg runs on, so reading stops only with it. The field is the
      // only trace: a failed reading otherwise looks like a session without a grant.
      report = {...report, 'udpLegFailed': 1};
    } finally {
      _reading = false;
    }
  }

  void _call(Future<void> Function() call) {
    try {
      unawaited(call().catchError((Object _) {}));
    } catch (_) {}
  }

  /// Safe at any stage and repeatable, and it never waits for the leg.
  void close() {
    if (_closed) return;
    _closed = true;
    _timer?.cancel();
    _call(_leg.stop);
  }
}

bool _ipv4(String value) {
  final parts = value.split('.');
  return parts.length == 4 &&
      parts.every(
        (part) =>
            RegExp(r'^(0|[1-9][0-9]{0,2})$').hasMatch(part) &&
            (int.tryParse(part) ?? 256) <= 255,
      );
}
