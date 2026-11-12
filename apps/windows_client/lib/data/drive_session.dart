import 'broker_transport.dart';

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';

import 'channel_redial.dart';
import 'client_diagnostic_codes.dart';
import 'drive_files.dart';
import 'drive_memory.dart';
import 'drive_rules.dart';
import 'product_log.dart';
import 'relay_socket.dart';

typedef DriveDialer = Future<WebSocket> Function(Uri url, String token);

/// Where one shared folder stands. [opening]: this PC is checking it;
/// [unavailable]: it cannot be shared now (gone, no access) and is not
/// offered to the VM; [pending]: offered, the VM is mapping it; [mapped]: a
/// drive letter in the VM; [failed]: the VM could not map it.
enum DriveShareState { opening, unavailable, pending, mapped, failed }

class DriveShare {
  DriveShare({
    required this.id,
    required this.path,
    required this.label,
    this.readOnly = false,
  });
  final String id, label;

  /// Where the folder is now on this PC (a rename or move there is followed).
  String path;

  /// The name shown for it; follows the folder, while [label] is what the VM
  /// was offered.
  late String shownName = label;
  bool readOnly;
  DriveShareState state = DriveShareState.opening;

  /// The VM drive letter while [state] is [DriveShareState.mapped].
  String? letter;

  /// A fixed code: why it is [DriveShareState.unavailable] or
  /// [DriveShareState.failed].
  String? error;

  /// The folder's device is gone (a pulled USB stick, a dropped network
  /// share, or a request that never came back): the VM's requests for it get
  /// drive_not_found at once, and 다시 시도 opens the folder again.
  bool offline = false;

  /// Requests of this folder in a row that ran out of time (a slow disk or a
  /// OneDrive download is not offline; two in a row is).
  int stalls = 0;
  bool get offered =>
      state == DriveShareState.pending ||
      state == DriveShareState.mapped ||
      state == DriveShareState.failed;
}

class _Snapshot {
  _Snapshot(this.entries, this.at);
  final List<Map<String, Object>> entries;
  final DateTime at;
}

class _ReplyOwner {
  bool replied = false;
  bool actualDone = false;
  bool observerDone = false;
}

/// drive-v1 for one streaming session (docs/drive-redirection.md): the
/// relay pair, the folders shared with the VM and the file requests the VM's
/// Host sends. It runs on its own socket beside video and input and never
/// waits for them; file data moves in requests of at most 256 KiB.
class DriveSession extends ChangeNotifier {
  DriveSession({
    required this.url,
    required this._token,
    required this.sessionId,
    required this.files,
    this.memory,
    this.server,
    this.desktopId,
    this.fs = false,
    DriveDialer? dialer,
    this.readyTimeout = const Duration(seconds: 20),
    this.snapshotLifetime = const Duration(seconds: 30),
    this.folderCheckInterval = const Duration(seconds: 2),
    this.requestDeadline = const Duration(seconds: 40),
    ProductLog? log,
  }) : _dialer = dialer ?? _dial,
       _log = log ?? ProductLog.instance;

  static Future<WebSocket> _dial(Uri url, String token) => connectRelaySocket(
    url,
    token,
    maxPayloadLength: 288 * 1024,
    // This client understands drive.notice (an older one would close the pair).
    headers: const {
      'X-Multitap-Drive-Notices': '1',
      'X-Multitap-Drive-Open': 'drive.open-v1',
    },
  );
  static final _random = Random.secure();

  final Uri url;
  final String sessionId;
  final DriveFileSystem files;
  final DriveMemory? memory;

  /// The "fs" revision of drive-v1 was negotiated for this session: the Host
  /// may also send `setsize` and `flush`. Without it they are unknown ops.
  final bool fs;

  /// The broker origin and VM the remembered folders belong to.
  final String? server, desktopId;
  final Duration readyTimeout, snapshotLifetime, folderCheckInterval;

  /// How long one file request may take on this PC. The relay ends the whole
  /// pair when the Host's request is unanswered for 60 s, which would unmap
  /// every shared drive because one folder (a dead network share) hangs.
  final Duration requestDeadline;
  final String _token;
  final DriveDialer _dialer;
  final ProductLog _log;

  final _shares = <DriveShare>[];
  final _snapshots = <String, _Snapshot>{};
  WebSocket? _socket;
  StreamSubscription<dynamic>? _subscription;
  Timer? _readyTimer, _folderTimer;
  bool _checking = false;
  Future<void>? _starting;
  bool _adding = false;
  int requests = 0, failedRequests = 0, active = 0;

  /// One terminal reply per admitted action, retained until both the actual
  /// action and its deadline observer finish. Admission bounds this map too.
  final _replyOwners = <int, _ReplyOwner>{};

  bool ready = false, closed = false;
  bool openSupported = false;
  String? openError;
  int _nextOpenId = 0;
  _DriveOpen? _opening;
  String? get openingShare => _opening?.share;
  bool canOpen(String id) =>
      openSupported &&
      ready &&
      !closed &&
      _opening == null &&
      _shares.any(
        (s) => s.id == id && s.state == DriveShareState.mapped && !s.offline,
      );

  /// Only the current share id crosses the relay; never a path or shell command.
  /// The deadline ends observation, not ownership of the underlying shell request.
  Future<String?> openShare(String id) {
    if (!canOpen(id) || _nextOpenId == 0xffffffff) {
      return Future.value(
        _opening != null ? 'drive_busy' : 'drive_open_unavailable',
      );
    }
    final pending = _DriveOpen(++_nextOpenId, id);
    _opening = pending;
    openError = null;
    lastGone = null;
    pending.timer = Timer(const Duration(seconds: 5), () {
      if (identical(_opening, pending) && !pending.result.isCompleted) {
        openError = 'drive_open_unknown';
        pending.result.complete(openError);
        notifyListeners();
      }
    });
    _send({'type': 'drive.open', 'id': pending.id, 'share': id});
    notifyListeners();
    return pending.result.future;
  }

  void _opened(Map<String, dynamic> value) {
    final pending = _opening;
    final ok = value['ok'];
    const errors = {
      'drive_share_unknown',
      'drive_open_unavailable',
      'drive_open_failed',
      'drive_open_unknown',
      'drive_busy',
      'no_interactive_session',
    };
    if (!openSupported ||
        pending == null ||
        value['id'] != pending.id ||
        value['share'] != pending.share ||
        !(ok == true
            ? _exact(value, ['type', 'id', 'share', 'ok'])
            : ok == false &&
                  _exact(value, ['type', 'id', 'share', 'ok', 'error']) &&
                  errors.contains(value['error']))) {
      throw const FormatException();
    }
    pending.timer?.cancel();
    final current = _shares.any(
      (s) =>
          s.id == pending.share &&
          s.state == DriveShareState.mapped &&
          !s.offline,
    );
    final failure = current
        ? (ok == true ? null : value['error'] as String)
        : 'drive_share_unknown';
    openError = failure;
    if (!pending.result.isCompleted) pending.result.complete(failure);
    _opening =
        null; // only an actual terminal ACK (or channel end) retires this owner
    notifyListeners();
  }

  /// The fixed code the sharing ended with (null while it runs).
  String? error;

  /// The broker refused the dial (not a lost network): never dialled again.
  bool dialRefused = false;

  /// channel-resume-v1: this ended channel is being brought back by itself
  /// (or waits for the stream to continue); the views do not say it is lost.
  bool get recovering => _recovering;
  bool _recovering = false;
  set recovering(bool value) {
    if (_recovering == value) return;
    _recovering = value;
    notifyListeners();
  }

  /// The folders that were shared when this channel ended, for the channel
  /// that replaces it in the same session ([inherit]).
  List<DriveShare> sharesAtEnd = const [];
  List<DriveShare> _inherited = const [];

  /// channel-resume-v1: this channel replaces [previous] in the same session
  /// (the stream resumed). The folders the person shared there are opened
  /// again here and offered under the same ids and names, so the desktop maps
  /// them as before; one that cannot be opened any more is listed as
  /// unavailable, never offered. Call before [start].
  void inherit(DriveSession previous) {
    _inherited = List.of(previous.sharesAtEnd);
  }

  /// Folders whose sharing ended because the folder was deleted on this PC
  /// (the newest name in [lastGone]; [goneCount] counts them for toasts).
  String? lastGone;
  int goneCount = 0;

  /// The person's next action in the panel (add, end, retry, open): what the
  /// panel still said about an earlier one (a folder that was deleted, a
  /// folder Explorer could not open) is over. [goneCount] keeps counting.
  void _acted() {
    lastGone = null;
    if (_opening == null) openError = null;
  }

  /// A listing was cut at [driveMaxListEntries] entries (the VM sees only the
  /// first ones of that folder); the panel says so. Names are never kept.
  bool listingTruncated = false;

  /// Hints the Host sent once (docs/drive-redirection.md, drive.notice): the VM's
  /// WebClient still has its old 50 MB file cap, or a file of 4 GB or more was
  /// seen (only the WebDAV drive backend ever says so). Never any names.
  bool oldFileLimit = false, fileTooLarge = false;

  List<DriveShare> get shares => List.unmodifiable(_shares);
  bool get adding => _adding;
  bool get canAdd =>
      ready && !closed && !_adding && _shares.length < driveMaxShares;

  /// The end came from outside this client: worth telling the user.
  bool get endedUnexpectedly =>
      closed &&
      error != null &&
      !const {
        'drive_session_closed',
        'session_ended',
        'authorization_revoked',
        'server_stopping',
      }.contains(error);

  Future<void> start() => _starting ??= _start();

  Future<void> _start() async {
    final port = brokerRelayPort(url);
    if (!brokerRelayAllowed(url) ||
        url.host.isEmpty ||
        port <= 0 ||
        url.userInfo.isNotEmpty ||
        url.hasQuery ||
        url.hasFragment ||
        url.path != '/api/v1/sessions/$sessionId/drive') {
      _close('drive_invalid_url');
      return;
    }
    final WebSocket socket;
    try {
      socket = await _dialer(url, _token);
    } catch (failure) {
      dialRefused = channelDialRefused(failure);
      _close('drive_transport_closed');
      return;
    }
    if (closed) {
      unawaited(socket.close().catchError((Object _) {}));
      return;
    }
    _socket = socket;
    _subscription = socket.listen(
      _receive,
      onError: (Object _) => _close('drive_transport_closed'),
      onDone: () => _close('drive_transport_closed'),
      cancelOnError: true,
    );
    _readyTimer = Timer(readyTimeout, () {
      if (!ready) _close('drive_pair_timeout');
    });
  }

  /// Stages of the drive records (docs/drive-redirection.md): 1 pair ready,
  /// 2 share list sent to the Host, 3 Host mapping result, 4 session ended,
  /// 5 folder add started, 6 picker cancelled, 7 confirmation declined,
  /// 8 native open failed (error: Win32 code), 9 folder offered, 10 remembered
  /// folders restored (remembered / restored counts), 11 add ended with an
  /// unexpected exception, 12 picker failed, 13 folder deleted on this PC
  /// (share ended and forgotten), 14 Host notice (old file cap / file over 4 GB).
  void _record(
    int stage, {
    bool ok = true,
    String? code,
    Map<String, Object> extra = const {},
  }) {
    final values = <String, Object>{
      ...extra,
      'stage': stage,
      'ok': ok,
      'shares': _shares.where((share) => share.offered).length,
      'mapped': _shares
          .where((share) => share.state == DriveShareState.mapped)
          .length,
      'requests': requests,
      'failedRequests': failedRequests,
    };
    if (code == null) {
      _log.record(LogEvent.drive, values);
    } else {
      _log.recordFailure(LogEvent.drive, code, values);
    }
  }

  // ---- incoming -----------------------------------------------------------

  void _receive(dynamic data) {
    if (closed) return;
    try {
      if (data is String) {
        if (data.length > 288 * 1024) throw const FormatException();
        final value = jsonDecode(data);
        if (value is! Map<String, dynamic>) throw const FormatException();
        _message(value);
      } else if (data is List<int>) {
        _binary(data is Uint8List ? data : Uint8List.fromList(data));
      } else {
        throw const FormatException();
      }
    } on FormatException {
      _close('drive_invalid_message');
    }
  }

  static bool _exact(Map<String, dynamic> value, List<String> keys) =>
      value.length == keys.length && keys.every(value.containsKey);
  static bool _integer(Object? value, int min, int max) =>
      value is int && value >= min && value <= max;

  void _message(Map<String, dynamic> value) {
    switch (value['type']) {
      case 'drive.ready':
        final limits = value['limits'];
        if (ready ||
            !(_exact(value, ['type', 'sessionId', 'protocol', 'limits']) ||
                (_exact(value, [
                      'type',
                      'sessionId',
                      'protocol',
                      'limits',
                      'openProtocol',
                    ]) &&
                    value['openProtocol'] == 'drive.open-v1')) ||
            value['sessionId'] != sessionId ||
            value['protocol'] != driveProtocol ||
            limits is! Map<String, dynamic> ||
            !_exact(limits, [
              'maxShares',
              'chunkBytes',
              'maxInFlight',
              'listPage',
              'maxPath',
              'maxName',
              'maxDepth',
            ]) ||
            limits['maxShares'] != driveMaxShares ||
            limits['chunkBytes'] != driveChunkBytes ||
            limits['maxInFlight'] != driveMaxInFlight ||
            limits['listPage'] != driveListPage ||
            limits['maxPath'] != driveMaxPath ||
            limits['maxName'] != driveMaxName ||
            limits['maxDepth'] != driveMaxDepth) {
          throw const FormatException();
        }
        openSupported = value['openProtocol'] == 'drive.open-v1';
        ready = true;
        _readyTimer?.cancel();
        _folderTimer = Timer.periodic(
          folderCheckInterval,
          (_) => unawaited(checkFolders()),
        );
        _record(1);
        notifyListeners();
        unawaited(_restore());
      case 'drive.error':
        final reason = value['reason'];
        _close(
          reason is String && RegExp(r'^[a-z_]{1,64}$').hasMatch(reason)
              ? reason
              : 'drive_transport_closed',
        );
      case 'drive.mounted':
        if (!ready) throw const FormatException();
        _mounted(value);
      case 'drive.opened':
        if (!ready) throw const FormatException();
        _opened(value);
      case 'drive.notice':
        if (!ready ||
            !_exact(value, ['type', 'code']) ||
            value['code'] is! String) {
          throw const FormatException();
        }
        _notice(value['code'] as String);
      case 'drive.request':
        if (!ready) throw const FormatException();
        _request(value);
      default:
        throw const FormatException();
    }
  }

  /// An unknown code (a newer Host) is ignored, never a reason to end sharing.
  void _notice(String code) {
    switch (code) {
      case 'drive_webclient_old_limit' when !oldFileLimit:
        oldFileLimit = true;
      case 'drive_file_too_large' when !fileTooLarge:
        fileTooLarge = true;
      default:
        return;
    }
    _record(14, ok: false, code: code);
    notifyListeners();
  }

  void _mounted(Map<String, dynamic> value) {
    final list = value['shares'];
    if (!_exact(value, ['type', 'shares']) ||
        list is! List ||
        list.length > driveMaxShares) {
      throw const FormatException();
    }
    final failures = <String>[];
    for (final item in list) {
      if (item is! Map<String, dynamic> ||
          item['id'] is! String ||
          !driveShareIdPattern.hasMatch(item['id'] as String)) {
        throw const FormatException();
      }
      final share = _shares
          .where((share) => share.id == item['id'] && share.offered)
          .firstOrNull;
      switch (item['state']) {
        case 'mapped':
          final letter = item['letter'];
          if (!_exact(item, ['id', 'state', 'letter']) ||
              letter is! String ||
              !RegExp(r'^[D-Z]$').hasMatch(letter)) {
            throw const FormatException();
          }
          share
            ?..state = DriveShareState.mapped
            ..letter = letter
            ..error = null;
        case 'failed':
          final code = item['error'];
          if (!_exact(item, ['id', 'state', 'error']) ||
              !driveMountErrorCodes.contains(code)) {
            throw const FormatException();
          }
          failures.add(code as String);
          share
            ?..state = DriveShareState.failed
            ..letter = null
            ..error = code;
        case 'pending':
          if (!_exact(item, ['id', 'state'])) throw const FormatException();
          share
            ?..state = DriveShareState.pending
            ..letter = null
            ..error = null;
        default:
          throw const FormatException();
      }
    }
    _record(3, ok: failures.isEmpty, code: failures.firstOrNull);
    notifyListeners();
  }

  static const _requestFields = {
    'stat': <String>[],
    'list': ['cursor'],
    'read': ['offset', 'length'],
    'create': ['dir', 'truncate'],
    'rename': ['to', 'replace'],
    'delete': <String>[],
    'touch': ['modified'],
    'setsize': ['size'],
    'flush': <String>[],
  };

  /// The share a request may use, or the fixed refusal.
  (DriveShare?, String?) _shareFor(String share, String op) {
    final found = _shares
        .where((item) => item.id == share && item.offered)
        .firstOrNull;
    if (found == null) return (null, 'drive_share_unknown');
    // A pulled USB stick or dropped network share: "not found" at once, never
    // another request waiting on a dead handle.
    if (found.offline) return (null, 'drive_not_found');
    if (driveModifyingOps.contains(op) && found.readOnly) {
      return (null, 'drive_read_only');
    }
    return (found, null);
  }

  /// Checks a request at once (a malformed one ends the pair) and serves it
  /// asynchronously.
  void _request(Map<String, dynamic> value) {
    final id = value['id'], op = value['op'];
    if (!_integer(id, 1, 0xffffffff) ||
        op is! String ||
        !_requestFields.containsKey(op) ||
        (!fs && (op == 'setsize' || op == 'flush'))) {
      throw const FormatException();
    }
    final requestId = id as int;
    if (_replyOwners.containsKey(requestId)) throw const FormatException();
    final share = value['share'], path = value['path'];
    final validShape =
        _exact(value, [
          'type',
          'id',
          'op',
          'share',
          'path',
          ..._requestFields[op]!,
        ]) &&
        share is String &&
        driveShareIdPattern.hasMatch(share) &&
        driveSafePath(path) &&
        switch (op) {
          'list' => _integer(value['cursor'], 0, 0xffffffff),
          'read' =>
            _integer(value['offset'], 0, driveMaxSize) &&
                _integer(value['length'], 1, driveChunkBytes),
          'create' =>
            value['dir'] is bool &&
                value['truncate'] is bool &&
                !(value['dir'] == true && value['truncate'] == true) &&
                path != '',
          'rename' =>
            value['replace'] is bool &&
                path != '' &&
                value['to'] != '' &&
                driveSafePath(value['to']) &&
                value['to'] != path,
          'delete' => path != '',
          'touch' => path != '' && _integer(value['modified'], 0, driveMaxTime),
          'setsize' => path != '' && _integer(value['size'], 0, driveMaxSize),
          _ => true,
        };
    if (!validShape) {
      _fail(requestId, 'drive_invalid_path');
      return;
    }
    final (target, refusal) = _shareFor(share, op);
    if (target == null) {
      _fail(requestId, refusal!);
      return;
    }
    // The relay never has more in flight; more is refused, never queued.
    if (active >= driveMaxInFlight || _replyOwners.length >= driveMaxInFlight) {
      _fail(requestId, 'drive_busy');
      return;
    }
    unawaited(_serve(requestId, op, share, path as String, value));
  }

  Future<void> _serve(
    int requestId,
    String op,
    String share,
    String relative,
    Map<String, dynamic> value,
  ) => _run(requestId, share: share, () async {
    switch (op) {
      case 'stat':
        final entry = await files.stat(share, relative);
        _send({
          'type': 'drive.response',
          'id': requestId,
          'ok': true,
          'entry': _entryJson(
            entry,
            root: relative.isEmpty,
            // Never a name the relay would refuse: the asked one instead.
            name: driveSafeName(entry.name)
                ? entry.name
                : relative.split('/').last,
          ),
        });
      case 'list':
        _send(await _page(requestId, share, relative, value['cursor'] as int));
      case 'read':
        final data = await files.read(
          share,
          relative,
          value['offset'] as int,
          value['length'] as int,
        );
        if (data.length > (value['length'] as int)) {
          throw const DriveFileException('drive_io_failed');
        }
        final frame = Uint8List(5 + data.length);
        frame[0] = 1;
        ByteData.sublistView(frame).setUint32(1, requestId, Endian.little);
        frame.setRange(5, frame.length, data);
        _sendBytes(frame);
      case 'create':
        await files.create(
          share,
          relative,
          dir: value['dir'] as bool,
          truncate: value['truncate'] as bool,
        );
        _ok(requestId);
      case 'rename':
        await files.rename(
          share,
          relative,
          value['to'] as String,
          replace: value['replace'] as bool,
        );
        _snapshots.clear();
        _ok(requestId);
      case 'delete':
        await files.delete(share, relative);
        _snapshots.clear();
        _ok(requestId);
      case 'touch':
        await files.touch(share, relative, value['modified'] as int);
        _ok(requestId);
      case 'setsize':
        await files.setSize(share, relative, value['size'] as int);
        _snapshots.clear();
        _ok(requestId);
      case 'flush':
        await files.flush(share, relative);
        _ok(requestId);
    }
  });

  /// A kind-2 write: [2][u32 id][u16 n][n bytes JSON header][data].
  void _binary(Uint8List data) {
    if (!ready || data.length < 7 || data[0] != 2) {
      throw const FormatException();
    }
    final view = ByteData.sublistView(data);
    final id = view.getUint32(1, Endian.little);
    final size = view.getUint16(5, Endian.little);
    if (id < 1 || size < 2 || 7 + size > data.length) {
      throw const FormatException();
    }
    if (_replyOwners.containsKey(id)) throw const FormatException();
    final Object? header;
    try {
      header = jsonDecode(
        utf8.decode(Uint8List.sublistView(data, 7, 7 + size)),
      );
    } catch (_) {
      throw const FormatException();
    }
    final payload = Uint8List.sublistView(data, 7 + size);
    if (header is! Map<String, dynamic> ||
        !_exact(header, ['op', 'share', 'path', 'offset', 'truncate']) ||
        header['op'] != 'write') {
      throw const FormatException();
    }
    final share = header['share'], path = header['path'];
    final truncate = header['truncate'];
    if (share is! String ||
        !driveShareIdPattern.hasMatch(share) ||
        !driveSafePath(path) ||
        path == '' ||
        !_integer(header['offset'], 0, driveMaxSize) ||
        truncate is! bool ||
        (truncate && header['offset'] != 0) ||
        payload.isEmpty ||
        payload.length > driveChunkBytes) {
      _fail(id, 'drive_invalid_path');
      return;
    }
    final (target, refusal) = _shareFor(share, 'write');
    if (target == null) {
      _fail(id, refusal!);
      return;
    }
    if (active >= driveMaxInFlight || _replyOwners.length >= driveMaxInFlight) {
      _fail(id, 'drive_busy');
      return;
    }
    final offset = header['offset'] as int, relative = path as String;
    unawaited(
      _run(id, share: share, () async {
        await files.write(share, relative, offset, payload, truncate: truncate);
        _ok(id);
      }),
    );
  }

  Future<void> _run(
    int id,
    Future<void> Function() action, {
    required String share,
  }) async {
    requests++;
    active++;
    final owner = _ReplyOwner();
    _replyOwners[id] = owner;
    // A deadline ends the wire waiter, while native I/O can still be running.
    // Keep its admission slot until that original action actually settles,
    // including a late failure or a session that has already been closed.
    final operation = Future<void>.sync(action).whenComplete(() {
      active--;
      owner.actualDone = true;
      _forgetReplyOwner(id, owner);
    });
    try {
      await operation.timeout(requestDeadline);
      for (final item in _shares) {
        if (item.id == share) item.stalls = 0;
      }
    } on TimeoutException {
      // The folder hangs (a network share that stopped answering). Answered
      // now so the relay never reaches its own 60 s limit and ends every
      // drive; after two in a row the folder counts as offline until it
      // answers again.
      _fail(id, 'drive_io_failed', counted: true);
      final stalled = _shares.where((item) => item.id == share).firstOrNull;
      if (stalled != null && ++stalled.stalls >= 2 && !stalled.offline) {
        stalled.offline = true;
        notifyListeners();
      }
    } on DriveFileException catch (failure) {
      _fail(id, failure.code, counted: true);
      if (failure.code == 'drive_not_found') unawaited(checkFolders());
    } catch (_) {
      _fail(id, 'drive_io_failed', counted: true);
    } finally {
      owner.observerDone = true;
      _forgetReplyOwner(id, owner);
    }
  }

  void _forgetReplyOwner(int id, _ReplyOwner owner) {
    if (owner.actualDone &&
        owner.observerDone &&
        identical(_replyOwners[id], owner)) {
      _replyOwners.remove(id);
    }
  }

  bool _claimReply(int id) {
    final owner = _replyOwners[id];
    if (owner == null) return true; // Immediate, non-admitted refusals.
    if (owner.replied) return false;
    owner.replied = true;
    return true;
  }

  void _ok(int id) => _send({'type': 'drive.response', 'id': id, 'ok': true});

  void _fail(int id, String code, {bool counted = false}) {
    if (!counted) requests++;
    failedRequests++;
    _send({
      'type': 'drive.response',
      'id': id,
      'ok': false,
      // drive_user_unavailable and anything unknown never reach the VM.
      'error': code == 'drive_user_unavailable'
          ? 'drive_denied'
          : driveErrorCodes.contains(code)
          ? code
          : 'drive_io_failed',
    });
  }

  static Map<String, Object> _entryJson(
    DriveEntry entry, {
    required bool root,
    String? name,
  }) {
    final free = entry.free, total = entry.total;
    return {
      'name': root ? '' : name ?? entry.name,
      'dir': root || entry.dir,
      'size': root || entry.dir ? 0 : entry.size.clamp(0, driveMaxSize),
      'modified': entry.modified.clamp(0, driveMaxTime),
      'created': entry.created.clamp(0, driveMaxTime),
      'readOnly': entry.readOnly,
      if (root &&
          free != null &&
          total != null &&
          free >= 0 &&
          total <= driveMaxSize &&
          free <= total)
        'space': {'free': free, 'total': total},
    };
  }

  /// One page of a directory, served from the listing taken at cursor 0
  /// (kept [snapshotLifetime]); <= 256 entries and <= 60,000 bytes of JSON.
  Future<Map<String, Object>> _page(
    int id,
    String share,
    String path,
    int cursor,
  ) async {
    final key = '$share\n$path';
    final now = DateTime.now();
    _snapshots.removeWhere(
      (_, snapshot) => now.difference(snapshot.at) > snapshotLifetime,
    );
    var snapshot = cursor == 0 ? null : _snapshots[key];
    if (snapshot == null) {
      final listed = await files.list(share, path);
      if (listed.length >= driveMaxListEntries && !listingTruncated) {
        listingTruncated = true;
        notifyListeners();
      }
      final entries = <Map<String, Object>>[];
      final names = <String>{};
      for (final entry in listed) {
        // Never a name the relay would refuse, never the same name twice.
        if (!driveSafeName(entry.name) || !names.add(entry.name)) continue;
        entries.add(_entryJson(entry, root: false));
        if (entries.length >= driveMaxListEntries) break;
      }
      snapshot = _Snapshot(entries, now);
      _snapshots[key] = snapshot;
      while (_snapshots.length > 16) {
        _snapshots.remove(_snapshots.keys.first);
      }
    }
    final page = <Map<String, Object>>[];
    var index = cursor, bytes = 96;
    while (index < snapshot.entries.length && page.length < driveListPage) {
      final size = utf8.encode(jsonEncode(snapshot.entries[index])).length + 1;
      if (page.isNotEmpty && bytes + size > driveJsonLimit) break;
      page.add(snapshot.entries[index]);
      bytes += size;
      index++;
    }
    return {
      'type': 'drive.response',
      'id': id,
      'ok': true,
      'entries': page,
      if (index < snapshot.entries.length) 'next': index,
    };
  }

  void _send(Map<String, Object?> value) {
    final socket = _socket;
    if (closed || socket == null) return;
    // Control IDs have a separate namespace from Host file-request replies.
    final id = value['type'] == 'drive.response' ? value['id'] : null;
    if (id is int && _replyOwners[id]?.replied == true) return;
    var text = jsonEncode(value);
    if (utf8.encode(text).length > driveJsonLimit && value['id'] is int) {
      failedRequests++;
      text = jsonEncode({
        'type': 'drive.response',
        'id': value['id'],
        'ok': false,
        'error': 'drive_io_failed',
      });
    }
    try {
      // Serialize and resolve the oversize fallback before claiming the one
      // terminal reply. Id-less control messages do not consume an owner.
      if (id is int && !_claimReply(id)) return;
      socket.add(text);
    } catch (_) {
      _close('drive_transport_closed');
    }
  }

  void _sendBytes(Uint8List data) {
    final socket = _socket;
    if (closed || socket == null) return;
    try {
      if (data.length >= 5 &&
          !_claimReply(
            ByteData.sublistView(data).getUint32(1, Endian.little),
          )) {
        return;
      }
      socket.add(data);
    } catch (_) {
      _close('drive_transport_closed');
    }
  }

  // ---- shares -----------------------------------------------------------

  String _newId() {
    const alphabet = 'abcdefghijklmnopqrstuvwxyz0123456789';
    for (;;) {
      final id =
          'd${List.generate(8, (_) => alphabet[_random.nextInt(alphabet.length)]).join()}';
      if (!_shares.any((share) => share.id == id)) return id;
    }
  }

  /// The complete list of shares offered to the VM.
  void _sendShares() {
    if (!ready || closed) return;
    final offered = _shares
        .where(
          (share) =>
              share.state != DriveShareState.opening &&
              share.state != DriveShareState.unavailable,
        )
        .toList();
    for (final share in offered) {
      if (share.state != DriveShareState.mapped &&
          share.state != DriveShareState.failed) {
        share.state = DriveShareState.pending;
      }
    }
    _send({
      'type': 'drive.shares',
      'shares': [
        for (final share in offered)
          {'id': share.id, 'name': share.label, 'readOnly': share.readOnly},
      ],
    });
    _snapshots.clear();
    _record(2);
  }

  /// Opens [share] natively; null when it can be offered, else the code.
  Future<String?> _open(DriveShare share, {bool record = true}) async {
    try {
      await files.open(share.id, share.path, readOnly: share.readOnly);
      if (closed) return 'drive_session_closed';
      if (share.state == DriveShareState.opening ||
          share.state == DriveShareState.unavailable) {
        share.state = DriveShareState.pending;
      }
      share.error = null;
      share.offline = false;
      return null;
    } catch (failure) {
      // Nothing of an earlier open of this share stays usable natively.
      unawaited(files.close(share.id).catchError((Object _) {}));
      final code = failure is DriveFileException
          ? failure.code
          : 'drive_io_failed';
      if (record && !closed) {
        _record(
          8,
          ok: false,
          code: code,
          extra: {'error': failure is DriveFileException ? failure.win32 : 0},
        );
      }
      share
        ..state = DriveShareState.unavailable
        ..letter = null
        ..error = code;
      return code;
    }
  }

  Future<void>? _forgetting;

  Future<void> _forgetRemembered() => _forgetting ??= () async {
    final store = memory, origin = server, desktop = desktopId;
    if (store == null || origin == null || desktop == null) return;
    try {
      // Migration only: old choices must never authorize a new session.
      await store
          .save(origin, desktop, const [])
          .timeout(const Duration(seconds: 2));
    } catch (_) {
      // Even when legacy preferences cannot be removed, we never read or
      // restore them. Sharing still requires this session's explicit picker.
    }
  }();

  Future<void> _restore() async {
    unawaited(_forgetRemembered());
    final inherited = _inherited;
    _inherited = const [];
    for (final share in inherited) {
      if (closed) return;
      if (_shares.length >= driveMaxShares ||
          _shares.any((item) => item.id == share.id)) {
        continue;
      }
      share
        ..state = DriveShareState.opening
        ..letter = null
        ..error = null
        ..offline = false
        ..stalls = 0;
      _shares.add(share);
      await _open(share);
    }
    if (closed) return;
    if (inherited.isNotEmpty) {
      _record(
        10,
        extra: {
          'remembered': inherited.length,
          'restored': _shares.where((share) => share.offered).length,
        },
      );
      notifyListeners();
    }
    _sendShares();
  }

  static bool _samePath(String a, String b) {
    String normal(String value) => value
        .replaceAll('/', r'\')
        .replaceAll(RegExp(r'\\+$'), '')
        .toLowerCase();
    return normal(a) == normal(b);
  }

  /// Picks a folder and shares it (writable; see [setReadOnly]). Null when it
  /// is shared or the picker was cancelled, else a fixed code: drive_exists
  /// (already shared), drive_limit (8 folders), or why it cannot be shared.
  Future<String?> addFolder({
    Future<bool> Function(DrivePick pick)? confirm,
  }) async {
    if (!ready || closed) return 'drive_session_closed';
    if (_shares.length >= driveMaxShares) return 'drive_limit';
    if (_adding) return 'drive_busy';
    _adding = true;
    _acted();
    _record(5);
    notifyListeners();
    try {
      final DrivePick? pick;
      final picking = Stopwatch()..start();
      int pickedMs() => picking.elapsedMilliseconds;
      try {
        pick = await files.pickFolder();
      } on DriveFileException catch (failure) {
        _record(
          12,
          ok: false,
          code: failure.code,
          extra: {'error': failure.win32, 'elapsedMs': pickedMs()},
        );
        return failure.code;
      }
      if (pick == null || closed) {
        // elapsedMs: a "cancel" within a fraction of a second means the
        // picker never really opened.
        if (!closed) _record(6, extra: {'elapsedMs': pickedMs()});
        return null;
      }
      if (_shares.any((share) => _samePath(share.path, pick!.path))) {
        return 'drive_exists';
      }
      if (_shares.length >= driveMaxShares) return 'drive_limit';
      // Nothing is opened or offered before the person agrees.
      if (confirm != null && !await confirm(pick)) {
        if (!closed) _record(7);
        return null;
      }
      if (closed) return null;
      final share = DriveShare(
        id: _newId(),
        path: pick.path,
        label: driveLabelFor(pick.path, pick.name),
      );
      _shares.add(share);
      notifyListeners();
      final failure = await _open(share);
      if (failure != null) {
        // A folder that cannot be shared is not remembered.
        _shares.remove(share);
        return failure;
      }
      _sendShares();
      _record(9);
      return null;
    } catch (_) {
      // The caller's dialog or the picker threw: a fixed code, never the text.
      if (!closed) _record(11, ok: false, code: 'drive_io_failed');
      return 'drive_io_failed';
    } finally {
      _adding = false;
      if (!closed) notifyListeners();
    }
  }

  Future<void> setReadOnly(String id, bool readOnly) async {
    final share = _shares.where((share) => share.id == id).firstOrNull;
    if (share == null || share.readOnly == readOnly || closed) return;
    share.readOnly = readOnly;
    notifyListeners();
    if (share.state != DriveShareState.unavailable) {
      // The PC enforces the new mode before the relay hears of it; a folder
      // that cannot be opened again is withdrawn from the VM instead.
      await _open(share);
      _sendShares();
    }
    if (!closed) notifyListeners();
  }

  Future<void> remove(String id) async {
    final share = _shares.where((share) => share.id == id).firstOrNull;
    if (share == null) return;
    _shares.remove(share);
    _acted();
    // The relay refuses the share at once; then this PC forgets it.
    _sendShares();
    notifyListeners();
    try {
      await files.close(id);
    } catch (_) {
      /* Already gone. */
    }
  }

  /// Asks each shared folder where it is now. A renamed or moved folder keeps
  /// working and its name and path are updated; a deleted one (or one moved
  /// to the Recycle Bin) ends its share: the VM drive is unmapped and the
  /// folder is forgotten. A folder recreated under the same name is never
  /// shared again by itself.
  Future<void> checkFolders() async {
    if (_checking || closed) return;
    _checking = true;
    try {
      for (final share in List.of(_shares)) {
        if (!share.offered) continue;
        final DriveRootStatus status;
        try {
          status = await files.status(share.id);
        } catch (_) {
          continue;
        }
        if (closed) return;
        if (!_shares.contains(share)) continue;
        if (status.deleted) {
          await _rootGone(share);
        } else if (status.offline != share.offline) {
          // The device went, or a folder that hung answers again. A pulled USB
          // stick stays offline until 다시 시도 (its old handle is dead).
          share.offline = status.offline;
          notifyListeners();
        } else if (status.path.isNotEmpty && status.path != share.path) {
          final named = share.shownName == driveLabelFor(share.path);
          share.path = status.path;
          if (named) share.shownName = driveLabelFor(status.path, status.name);
          notifyListeners();
        }
      }
    } finally {
      _checking = false;
    }
  }

  Future<void> _rootGone(DriveShare share) async {
    _shares.remove(share);
    lastGone = share.shownName;
    goneCount++;
    _record(13);
    // The Host unmaps it at once; then this PC lets go of the folder.
    _sendShares();
    notifyListeners();
    try {
      await files.close(share.id);
    } catch (_) {
      /* Already gone. */
    }
  }

  /// Retries a remembered folder that could not be shared.
  Future<String?> retry(String id) async {
    final share = _shares.where((share) => share.id == id).firstOrNull;
    if (share == null ||
        (share.state != DriveShareState.unavailable && !share.offline) ||
        closed) {
      return null;
    }
    final prior = share.state, wasOffline = share.offline;
    _acted();
    final failure = await _open(share);
    if (failure == null) {
      _sendShares();
    } else if (wasOffline && prior != DriveShareState.unavailable) {
      // Still gone: the drive stays in the VM (answering "not found") and the
      // panel keeps offering 다시 시도, instead of withdrawing it.
      share
        ..state = prior
        ..offline = true
        ..error = null;
    }
    notifyListeners();
    return failure;
  }

  // ---- end --------------------------------------------------------------

  void _close(String reason) {
    if (closed) return;
    closed = true;
    openSupported = false;
    final opening = _opening;
    _opening = null;
    opening?.timer?.cancel();
    if (opening != null && !opening.result.isCompleted) {
      opening.result.complete('drive_open_unknown');
    }
    ready = false;
    error = reason;
    _readyTimer?.cancel();
    _folderTimer?.cancel();
    _snapshots.clear();
    unawaited(_subscription?.cancel());
    final socket = _socket;
    _socket = null;
    if (socket != null) unawaited(socket.close().catchError((Object _) {}));
    // What the person had shared (a folder still being opened was never
    // offered): the channel that replaces this one offers it again.
    sharesAtEnd = [
      for (final share in _shares)
        if (share.state != DriveShareState.opening) share,
    ];
    for (final share in _shares) {
      unawaited(files.close(share.id).catchError((Object _) {}));
      share
        ..letter = null
        ..state = share.state == DriveShareState.unavailable
            ? DriveShareState.unavailable
            : DriveShareState.pending;
    }
    _shares.clear();
    unawaited(_forgetRemembered());
    _record(
      4,
      ok:
          reason == 'drive_session_closed' ||
          normalChannelEnds.contains(reason),
      code: reason,
    );
    notifyListeners();
  }

  /// Ends sharing and forgets every choice; a new session starts empty.
  Future<void> close({String reason = 'drive_session_closed'}) async {
    _close(reason);
    await _forgetRemembered();
  }
}

// One retained request; a local observation timeout does not admit another open.
class _DriveOpen {
  _DriveOpen(this.id, this.share);
  final int id;
  final String share;
  final result = Completer<String?>();
  Timer? timer;
}
