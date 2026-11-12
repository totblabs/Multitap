import 'broker_transport.dart';

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';

import 'channel_redial.dart';
import 'relay_socket.dart';
import 'user_data_path.dart';
import '../l10n/l10n.dart';

/// Streaming printer redirection (print-v1; docs/remote-session-protocol.md
/// "Printing"). While a granted stream session runs, the VM has a printer
/// "Multitap 프린터 (PC)"; each document printed there arrives here as a PDF
/// over the session's own print socket. It is kept in a per-session folder
/// under %LOCALAPPDATA%\Multitap\Client\print until the user prints it on a
/// local printer, saves it, or cancels; then it is deleted. Nothing prints
/// without the user's choice.
const printProtocol = 'print-v1';

typedef PrintDialer = Future<WebSocket> Function(Uri url, String token);

/// What the user did with a document (sent to the Host for its counters).
enum PrintOutcome { printed, saved, cancelled, failed }

/// One complete, verified document waiting for the user's choice.
class PrintDocument {
  PrintDocument({
    required this.jobId,
    required this.title,
    required this.size,
    required this.file,
  });
  final String jobId;

  /// The VM's document name (may be empty); shown, never logged.
  final String title;
  final int size;
  final File file;

  /// A file name for "PDF로 저장": the title without characters Windows
  /// refuses, or a fixed name.
  String get suggestedName {
    var name = title
        .replaceAll(RegExp(r'[<>:"/\\|?*\x00-\x1f]'), ' ')
        .replaceAll(RegExp(r'\s+'), ' ')
        .trim();
    if (name.toLowerCase().endsWith('.pdf')) {
      name = name.substring(0, name.length - 4).trim();
    }
    while (name.endsWith('.')) {
      name = name.substring(0, name.length - 1).trim();
    }
    if (name.length > 100) name = name.substring(0, 100).trim();
    return '${name.isEmpty ? l10n.multitapPrint : name}.pdf';
  }
}

class PrintSessionException implements Exception {
  const PrintSessionException(this.code);
  final String code;
  @override
  String toString() => code;
}

class _DigestSink implements Sink<Digest> {
  Digest? value;
  @override
  void add(Digest data) => value = data;
  @override
  void close() {}
}

class _Receiving {
  _Receiving(this.jobId, this.title, this.size, this.file, this.output) {
    hash = sha256.startChunkedConversion(digest);
  }
  final String jobId, title;
  final int size;
  final File file;
  final RandomAccessFile output;
  final digest = _DigestSink();
  late final ByteConversionSink hash;
  int received = 0;
  DateTime at = DateTime.now();
}

/// The client side of one session's print socket.
class PrintSession extends ChangeNotifier {
  PrintSession({
    required this.url,
    required this._token,
    required this.sessionId,
    PrintDialer? dialer,
    Directory? storage,
    String? folderName,
    this.readyTimeout = const Duration(seconds: 15),
    this.idleTimeout = const Duration(seconds: 90),
  }) : _dialer = dialer ?? _dial,
       _folderName = folderName ?? sessionId,
       _storageRoot = storage;

  static Future<WebSocket> _dial(Uri url, String token) =>
      connectRelaySocket(url, token, maxPayloadLength: 256 * 1024);
  static const maximumChunkBytes = 131072;
  static const maximumJobBytes = 256 * 1024 * 1024;
  static const maximumPending = 16;
  static const maximumTitle = 200;
  static final _random = Random.secure();

  final Uri url;
  final String sessionId;
  final String _token;
  final PrintDialer _dialer;
  final Directory? _storageRoot;

  /// This channel's own folder under the print storage. A channel dialled
  /// again in the same session (channel-resume-v1) gets another one, so a
  /// document the earlier channel still shows in a dialog and the new
  /// channel's documents never share a folder.
  final String _folderName;
  final Duration readyTimeout, idleTimeout;

  /// The broker refused the dial (not a lost network): never dialled again.
  bool dialRefused = false;

  /// channel-resume-v1: this ended channel is being brought back by itself
  /// (or waits for the stream to continue). The views say nothing about a
  /// lost printer meanwhile.
  bool get recovering => _recovering;
  bool _recovering = false;
  set recovering(bool value) {
    if (_recovering == value) return;
    _recovering = value;
    notifyListeners();
  }

  WebSocket? _socket;
  StreamSubscription<dynamic>? _subscription;
  Future<void>? _starting, _closing;
  Directory? _folder;
  Timer? _deadline;
  _Receiving? _receiving;
  final _ready = Completer<void>();
  int _chunkBytes = maximumChunkBytes, _maxJobBytes = maximumJobBytes;

  /// The relay paired this client with the Host.
  bool ready = false;

  /// The VM's printer exists (print.queue ready).
  bool queueReady = false;

  /// Why the VM's printer could not be prepared (a fixed print_* code).
  String? queueReason;

  /// The last document the Host could not offer (a fixed print_* code).
  String? skipped;

  /// The channel ended with this fixed code (null while open or after a
  /// normal session end).
  String? error;
  bool closed = false;

  /// Complete documents waiting for the user, oldest first.
  final pending = <PrintDocument>[];

  /// Documents a dialog or Android's print screen is showing right now. They
  /// survive the end of the session (see [hold]) until [finish].
  final _held = <PrintDocument>[];

  /// Documents that were waiting or still arriving when the channel ended and
  /// are gone with it (the notice says how many).
  int lostDocuments = 0;

  /// The document arriving now (for a progress line), or null.
  String? get receivingTitle => _receiving?.title;
  int get receivedBytes => _receiving?.received ?? 0;
  int get receivingSize => _receiving?.size ?? 0;
  bool get receiving => _receiving != null;

  Future<void> start() => _starting ??= _start();
  Future<void> _start() async {
    try {
      if (!brokerRelayAllowed(url) ||
          url.host.isEmpty ||
          url.userInfo.isNotEmpty ||
          url.hasQuery ||
          url.hasFragment ||
          !RegExp(r'^[A-Za-z0-9-]{1,80}$').hasMatch(sessionId) ||
          url.path != '/api/v1/sessions/$sessionId/print') {
        throw const PrintSessionException('print_invalid_url');
      }
      final socket = await _dialer(url, _token);
      if (closed) {
        await socket.close();
        return;
      }
      _socket = socket;
      socket.pingInterval = const Duration(seconds: 10);
      _subscription = socket.listen(
        (message) {
          // One message at a time, files included, in arrival order.
          _subscription?.pause();
          unawaited(
            _receive(message).whenComplete(() {
              if (!closed) _subscription?.resume();
            }),
          );
        },
        onError: (_) => unawaited(_end('print_transport_closed')),
        onDone: () => unawaited(_end('print_transport_closed')),
        cancelOnError: true,
      );
      await _ready.future.timeout(readyTimeout);
      if (!closed) {
        _deadline = Timer.periodic(const Duration(seconds: 1), (_) {
          final current = _receiving;
          if (current != null &&
              DateTime.now().difference(current.at) > idleTimeout) {
            unawaited(_end('print_timeout'));
          }
        });
      }
    } catch (failure) {
      final code = failure is PrintSessionException
          ? failure.code
          : failure is TimeoutException
          ? 'print_pair_timeout'
          : 'print_transport_closed';
      dialRefused = channelDialRefused(failure);
      await _end(code);
    }
  }

  void _require(bool value, [String code = 'print_invalid_message']) {
    if (!value) throw PrintSessionException(code);
  }

  void _fields(Map<String, dynamic> value, List<String> fields) => _require(
    value.length == fields.length && fields.every(value.containsKey),
  );

  void _send(Map<String, Object?> value) {
    final socket = _socket;
    if (closed || socket == null) return;
    try {
      socket.add(jsonEncode(value));
    } catch (_) {
      /* The transport's own close ends the session. */
    }
  }

  Future<void> _receive(dynamic message) async {
    if (closed) return;
    try {
      _require(message is String && message.length <= 256 * 1024);
      final value = jsonDecode(message as String);
      _require(value is Map<String, dynamic> && value['type'] is String);
      await _handle(value as Map<String, dynamic>);
    } catch (failure) {
      await _end(
        failure is PrintSessionException
            ? failure.code
            : 'print_invalid_message',
      );
    }
  }

  Future<void> _handle(Map<String, dynamic> value) async {
    switch (value['type']) {
      case 'print.ready':
        _fields(value, ['type', 'sessionId', 'protocol', 'limits']);
        final limits = value['limits'];
        _require(
          !ready &&
              value['sessionId'] == sessionId &&
              value['protocol'] == printProtocol &&
              limits is Map<String, dynamic>,
        );
        final chunk = (limits as Map<String, dynamic>)['chunkBytes'];
        final window = limits['windowChunks'];
        final maximum = limits['maxJobBytes'];
        _require(
          chunk is int &&
              chunk >= 1024 &&
              chunk <= maximumChunkBytes &&
              window is int &&
              window >= 1 &&
              window <= 64 &&
              maximum is int &&
              maximum >= 1,
        );
        _chunkBytes = chunk as int;
        _maxJobBytes = min(maximum as int, maximumJobBytes);
        ready = true;
        if (!_ready.isCompleted) _ready.complete();
        notifyListeners();
      case 'print.queue':
        _require(ready);
        if (value['state'] == 'ready') {
          _fields(value, ['type', 'state']);
          queueReady = true;
          queueReason = null;
        } else {
          _fields(value, ['type', 'state', 'reason']);
          _require(
            value['state'] == 'unavailable' &&
                value['reason'] is String &&
                RegExp(r'^print_[a-z_]{1,40}$').hasMatch(value['reason']),
          );
          queueReady = false;
          queueReason = value['reason'] as String;
        }
        notifyListeners();
      case 'print.skipped':
        _fields(value, ['type', 'reason']);
        _require(
          ready &&
              value['reason'] is String &&
              RegExp(r'^print_[a-z_]{1,40}$').hasMatch(value['reason']),
        );
        skipped = value['reason'] as String;
        notifyListeners();
      case 'print.job':
        await _begin(value);
      case 'print.chunk':
        await _chunk(value);
      case 'print.end':
        await _finishReceive(value);
      case 'print.cancel':
        _fields(value, ['type', 'jobId']);
        if (_receiving?.jobId == value['jobId']) {
          await _abortReceive();
          // The Host could not read its copy any more: the document will not
          // arrive, and the user is told instead of waiting (edge-case audit).
          skipped = 'print_job_failed';
          notifyListeners();
        }
      case 'print.error':
        _require(value['reason'] is String);
        await _end(value['reason'] as String);
      default:
        throw const PrintSessionException('print_invalid_message');
    }
  }

  Future<Directory> _storage() async {
    final existing = _folder;
    if (existing != null) {
      requirePlainUserDataPath(existing.path);
      await existing.create(recursive: true);
      requirePlainUserDataPath(existing.path);
      return existing;
    }
    var root = _storageRoot;
    if (root == null && Platform.isAndroid) {
      // The app's own cache directory (Directory.systemTemp on Android).
      root = Directory('${Directory.systemTemp.path}/print');
    } else if (root == null) {
      final appData = Platform.environment['LOCALAPPDATA'];
      if (appData == null || appData.isEmpty) {
        throw const PrintSessionException('print_storage_unavailable');
      }
      root = Directory('$appData\\Multitap\\Client\\print');
    }
    final folder = Directory(
      '${root.path}${Platform.pathSeparator}$_folderName',
    );
    // The elevated client writes only through plain folders (no links).
    requirePlainUserDataPath(folder.path);
    await folder.create(recursive: true);
    requirePlainUserDataPath(folder.path);
    await _sweep(root, folder);
    return _folder = folder;
  }

  static final _jobName = RegExp(r'^job-[0-9a-f]{32}\.pdf$');
  static String _leaf(String path) =>
      path.split(RegExp(r'[\\/]')).where((part) => part.isNotEmpty).last;

  /// Removes this product's plain document files from [folder], then the
  /// folder if nothing else is left. A link is never followed or removed by
  /// this elevated client; anything it did not create stays.
  static Future<void> _removeFolder(Directory folder) async {
    try {
      requirePlainUserDataPath(folder.path);
      if (FileSystemEntity.typeSync(folder.path, followLinks: false) !=
          FileSystemEntityType.directory) {
        return;
      }
      await for (final entry in folder.list(followLinks: false)) {
        if (entry is File &&
            _jobName.hasMatch(_leaf(entry.path)) &&
            FileSystemEntity.typeSync(entry.path, followLinks: false) ==
                FileSystemEntityType.file) {
          await entry.delete();
        }
      }
      await folder.delete();
    } catch (_) {
      /* Not empty, busy or gone: tried again by a later session. */
    }
  }

  /// Folders of sessions that ended abnormally more than a day ago.
  static Future<void> _sweep(Directory root, Directory keep) async {
    try {
      final now = DateTime.now();
      await for (final entry in root.list(followLinks: false)) {
        if (entry is! Directory || entry.path == keep.path) continue;
        if (!RegExp(r'^[A-Za-z0-9-]{1,80}$').hasMatch(_leaf(entry.path))) {
          continue;
        }
        final stat = await entry.stat();
        if (now.difference(stat.modified) > const Duration(days: 1)) {
          await _removeFolder(entry);
        }
      }
    } catch (_) {
      /* Best effort; a busy folder is tried again by a later session. */
    }
  }

  Future<void> _begin(Map<String, dynamic> value) async {
    _fields(value, ['type', 'jobId', 'format', 'size', 'title']);
    final jobId = value['jobId'], size = value['size'], title = value['title'];
    _require(
      ready &&
          _receiving == null &&
          jobId is String &&
          RegExp(r'^[A-Za-z0-9_-]{1,64}$').hasMatch(jobId) &&
          value['format'] == 'pdf' &&
          size is int &&
          size >= 1 &&
          title is String &&
          title.length <= maximumTitle,
    );
    if ((size as int) > _maxJobBytes || pending.length >= maximumPending) {
      // More than the user can still act on: refused, the VM is not blocked.
      _send({'type': 'print.cancel', 'jobId': jobId});
      skipped = size > _maxJobBytes
          ? 'print_job_too_large'
          : 'print_too_many_waiting';
      notifyListeners();
      return;
    }
    final File file;
    final RandomAccessFile output;
    try {
      final folder = await _storage();
      final name =
          'job-${List.generate(16, (_) => _random.nextInt(256).toRadixString(16).padLeft(2, '0')).join()}.pdf';
      file = File('${folder.path}${Platform.pathSeparator}$name');
      requirePlainUserDataPath(file.path);
      output = await file.open(mode: FileMode.writeOnly);
    } catch (_) {
      // No room or no access here: this document is declined, the printer and
      // the other documents are not (an end of the whole channel would hide
      // the cause behind a protocol error).
      _send({'type': 'print.cancel', 'jobId': jobId});
      skipped = 'print_local_storage_failed';
      notifyListeners();
      return;
    }
    _receiving = _Receiving(
      jobId as String,
      title as String,
      size,
      file,
      output,
    );
    notifyListeners();
  }

  Future<void> _chunk(Map<String, dynamic> value) async {
    _fields(value, ['type', 'jobId', 'offset', 'data']);
    final current = _receiving;
    // Chunks of a job this client just cancelled may still be on their way.
    if (current == null || current.jobId != value['jobId']) return;
    _require(
      value['offset'] == current.received && value['data'] is String,
      'print_out_of_order',
    );
    final List<int> bytes;
    try {
      bytes = base64.decode(value['data'] as String);
    } on FormatException {
      throw const PrintSessionException('print_invalid_message');
    }
    _require(
      bytes.isNotEmpty &&
          bytes.length <= _chunkBytes &&
          bytes.length <= current.size - current.received,
      'print_limits_exceeded',
    );
    try {
      await current.output.writeFrom(bytes);
    } on FileSystemException {
      // The disk filled up (or the folder went away) while the document
      // arrived: this document is dropped, the channel stays.
      await _abortReceive();
      _send({'type': 'print.cancel', 'jobId': current.jobId});
      skipped = 'print_local_storage_failed';
      notifyListeners();
      return;
    }
    current.hash.add(bytes);
    current.received += bytes.length;
    current.at = DateTime.now();
    _send({
      'type': 'print.ack',
      'jobId': current.jobId,
      'offset': current.received,
    });
    notifyListeners();
  }

  Future<void> _finishReceive(Map<String, dynamic> value) async {
    _fields(value, ['type', 'jobId', 'sha256']);
    final current = _receiving;
    if (current == null || current.jobId != value['jobId']) return;
    _require(current.received == current.size, 'print_out_of_order');
    _receiving = null;
    current.hash.close();
    try {
      await current.output.close();
    } on FileSystemException {
      // The last buffered bytes could not be written: not a whole document.
      await _delete(current.file);
      _send({
        'type': 'print.result',
        'jobId': current.jobId,
        'result': 'failed',
      });
      skipped = 'print_local_storage_failed';
      notifyListeners();
      return;
    }
    final digest = current.digest.value?.toString();
    if (digest == null || digest != value['sha256']) {
      await _delete(current.file);
      _send({
        'type': 'print.result',
        'jobId': current.jobId,
        'result': 'failed',
      });
      skipped = 'print_job_failed';
      notifyListeners();
      return;
    }
    pending.add(
      PrintDocument(
        jobId: current.jobId,
        title: current.title,
        size: current.size,
        file: current.file,
      ),
    );
    notifyListeners();
  }

  Future<void> _abortReceive() async {
    final current = _receiving;
    _receiving = null;
    if (current == null) return;
    try {
      current.hash.close();
      await current.output.close();
    } catch (_) {}
    await _delete(current.file);
  }

  static Future<void> _delete(File file) async {
    try {
      requirePlainUserDataPath(file.path);
      await file.delete();
    } catch (_) {
      /* Swept with the session folder. */
    }
  }

  /// The user printed, saved or dismissed [document]: the Host is told (fixed
  /// outcome only) and the local copy is deleted.
  Future<void> finish(PrintDocument document, PrintOutcome outcome) async {
    final wasHeld = _held.remove(document);
    if (!pending.remove(document)) {
      // The channel ended while the person was still choosing: the document
      // they had in front of them is theirs to print or save to the end, and
      // is deleted only now (the Host is gone; nothing is sent).
      if (wasHeld) {
        await _delete(document.file);
        final folder = _folder;
        if (folder != null && _held.isEmpty) await _removeFolder(folder);
      }
      return;
    }
    _send({
      'type': 'print.result',
      'jobId': document.jobId,
      'result': outcome.name,
    });
    await _delete(document.file);
    notifyListeners();
  }

  /// The document a dialog (or Android's print screen) is showing: it stays
  /// on disk if the channel ends before [finish]. Held documents are the
  /// caller's to finish.
  void hold(PrintDocument document) {
    if (!_held.contains(document) && (pending.contains(document) || !closed)) {
      _held.add(document);
    }
  }

  /// Dismisses the notice of a skipped document.
  void clearSkipped() {
    if (skipped == null) return;
    skipped = null;
    notifyListeners();
  }

  Future<void> _end(String code) async {
    if (closed) return;
    closed = true;
    error = code == 'session_ended' ? null : code;
    queueReady = false;
    // Nothing is offered any more; the files go below, except the one a
    // dialog is showing (it is finished by whoever holds it).
    final documents = List.of(pending);
    pending.clear();
    lostDocuments =
        documents.where((document) => !_held.contains(document)).length +
        (_receiving == null ? 0 : 1);
    if (!_ready.isCompleted) _ready.complete();
    _deadline?.cancel();
    final subscription = _subscription, socket = _socket;
    _subscription = null;
    _socket = null;
    try {
      await subscription?.cancel();
    } catch (_) {}
    try {
      await socket?.close();
    } catch (_) {}
    await _abortReceive();
    for (final document in documents) {
      if (!_held.contains(document)) await _delete(document.file);
    }
    final folder = _folder;
    if (folder != null && _held.isEmpty) await _removeFolder(folder);
    notifyListeners();
  }

  /// Ends printing for this session (stream closed): unprinted documents are
  /// deleted with the session folder.
  Future<void> close() => _closing ??= _end('session_ended');
}

/// Documents that were arriving or waiting when the printer connection fell
/// and is being brought back by itself: they are gone, and the person prints
/// them again from the desktop (no "connect again": the app does that).
String printLostNotice(int count) =>
    l10n.printAgainFromDesktop(count);
