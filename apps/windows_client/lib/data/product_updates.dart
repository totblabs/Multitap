import 'broker_transport.dart';
import 'broker_probe.dart';

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'session_windows.dart';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';

import 'client_settings.dart';
import '../l10n/l10n.dart' show acceptLanguage;
export 'client_build_version.dart' show clientProductVersion;

const _maximumFile = 128 * 1024 * 1024;
final _version = RegExp(r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$');
// The project's own Ed25519 release signature (base64 of 64 bytes) over the
// release's product, version, file name, size and SHA-256. The native side
// verifies it against the key pinned in this build before any installer runs.
final _signature = RegExp(r'^[A-Za-z0-9+/]{86}==$');

bool _validUnicode(String value) {
  final units = value.codeUnits;
  for (var i = 0; i < units.length; i++) {
    final unit = units[i];
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++i >= units.length || units[i] < 0xdc00 || units[i] > 0xdfff) {
        return false;
      }
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return false;
    }
  }
  return true;
}

class UpdateFailure implements Exception {
  const UpdateFailure(this.code);
  final String code;
}

int compareProductVersions(String a, String b) {
  if (!_version.hasMatch(a) || !_version.hasMatch(b)) {
    throw const UpdateFailure('invalid_catalog');
  }
  final left = a.split('.').map(int.parse).toList();
  final right = b.split('.').map(int.parse).toList();
  for (var i = 0; i < 3; i++) {
    final comparison = left[i].compareTo(right[i]);
    if (comparison != 0) return comparison;
  }
  return 0;
}

class ProductRelease {
  const ProductRelease({
    required this.product,
    required this.version,
    required this.filename,
    required this.size,
    required this.sha256,
    required this.url,
    this.changelog = '',
    this.releasedAt,
    this.signature,
  });
  final String product, version, filename, sha256, changelog;
  final int size;
  final Uri url;
  final DateTime? releasedAt;

  /// The catalog's release signature, or null when the entry has none (older
  /// releases, or a server that does not pass it on). An installer without a
  /// verified signature is never started ([ProductUpdates.download] refuses
  /// it with `signature_invalid`; the native side verifies it).
  final String? signature;

  // The public constructor is also used by callers outside catalog parsing.
  // Revalidate before choosing a filesystem path or starting any download.
  void _validateFor(Uri origin) {
    if (!['client', 'host'].contains(product) ||
        !brokerOriginAllowed(origin) ||
        url.scheme != origin.scheme ||
        !url.hasAuthority ||
        url.origin != origin.origin ||
        url.userInfo.isNotEmpty ||
        url.hasQuery ||
        url.hasFragment) {
      throw const UpdateFailure('invalid_catalog');
    }
    final checked = parse(
      {
        'product': product,
        'format': 'installer',
        'version': version,
        'filename': filename,
        'size': size,
        'sha256': sha256,
        'url': url.path,
        'changelog': changelog,
        if (releasedAt != null)
          'releasedAt': releasedAt!.toUtc().toIso8601String(),
        'signature': ?signature,
      },
      origin,
      product,
    );
    if (checked == null ||
        checked.url != url ||
        checked.signature != signature) {
      throw const UpdateFailure('invalid_catalog');
    }
  }

  static ProductRelease? parse(dynamic entry, Uri origin, String product) {
    if (entry is! Map || entry['product'] != product) return null;
    if (entry['format'] != 'installer') return null;
    final version = entry['version'];
    if (version is! String || !_version.hasMatch(version)) {
      throw const UpdateFailure('invalid_catalog');
    }
    final filename =
        'Multitap${product == 'host' ? '-Host' : ''}-$version-windows-x64.exe';
    final size = entry['size'], digest = entry['sha256'];
    final notes = entry['changelog'] ?? '';
    if (entry['filename'] != filename ||
        entry['url'] != '/downloads/$filename' ||
        size is! int ||
        size < 1 ||
        size > _maximumFile ||
        digest is! String ||
        !RegExp(r'^[a-f0-9]{64}$').hasMatch(digest) ||
        notes is! String ||
        !_validUnicode(notes) ||
        utf8.encode(notes).length > 8000 ||
        RegExp(r'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]').hasMatch(notes)) {
      throw const UpdateFailure('invalid_catalog');
    }
    final date = entry['releasedAt'];
    DateTime? releasedAt;
    if (date != null) {
      if (date is! String ||
          !date.endsWith('Z') ||
          (releasedAt = DateTime.tryParse(date)) == null ||
          releasedAt!.toUtc().toIso8601String() != date) {
        throw const UpdateFailure('invalid_catalog');
      }
    }
    // Only a well-formed signature is kept; anything else counts as unsigned
    // (released servers and older entries carry none). Old clients ignore it.
    final signature = entry['signature'];
    return ProductRelease(
      product: product,
      version: version,
      filename: filename,
      size: size,
      sha256: digest,
      url: origin.resolve('/downloads/$filename'),
      changelog: notes,
      releasedAt: releasedAt,
      signature: signature is String && _signature.hasMatch(signature)
          ? signature
          : null,
    );
  }
}

/// One version's entry in the release history: its notes and date only (no
/// file, size, hash or address; nothing here is downloaded or installed).
class ReleaseNote {
  const ReleaseNote({
    required this.version,
    this.changelog = '',
    this.releasedAt,
  });
  final String version, changelog;
  final DateTime? releasedAt;

  /// The catalog's note rule (ProductRelease.parse): well-formed text of at
  /// most 8000 UTF-8 bytes without control characters, and a canonical UTC
  /// time. Null for anything else, which the history leaves out.
  static ReleaseNote? parse(dynamic entry) {
    if (entry is! Map) return null;
    final version = entry['version'], notes = entry['changelog'] ?? '';
    if (version is! String ||
        !_version.hasMatch(version) ||
        notes is! String ||
        !_validUnicode(notes) ||
        utf8.encode(notes).length > 8000 ||
        RegExp(r'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]').hasMatch(notes)) {
      return null;
    }
    final date = entry['releasedAt'];
    DateTime? releasedAt;
    if (date != null) {
      if (date is! String ||
          !date.endsWith('Z') ||
          (releasedAt = DateTime.tryParse(date)) == null ||
          releasedAt!.toUtc().toIso8601String() != date) {
        return null;
      }
    }
    return ReleaseNote(
      version: version,
      changelog: notes,
      releasedAt: releasedAt,
    );
  }
}

/// A page of the release history, newest first.
class ReleaseHistoryPage {
  const ReleaseHistoryPage(this.releases, {this.next, this.total});
  final List<ReleaseNote> releases;

  /// What to pass as `before` for the following page; null after the oldest
  /// version the server keeps.
  final String? next;

  /// Every version the server keeps for the product, when it says.
  final int? total;
}

class UpdateResponse {
  const UpdateResponse(
    this.status,
    this.contentLength,
    this.body, {
    this.contentRange,
  });
  final int status, contentLength;
  final Stream<List<int>> body;

  /// The `Content-Range` header of a 206 reply, as sent.
  final String? contentRange;
}

abstract class UpdateTransport {
  Future<UpdateResponse> get(Uri uri);
  void close();
}

/// A transport that can continue an interrupted download: the bytes of [uri]
/// from [from] on (HTTP Range), only while the file is still [entityTag]
/// (If-Range). A server without ranges, or with another file, answers 200
/// with the whole file instead.
abstract class RangeUpdateTransport implements UpdateTransport {
  Future<UpdateResponse> getFrom(Uri uri, int from, String entityTag);
}

class HttpsUpdateTransport implements RangeUpdateTransport {
  final _client = HttpClient()..connectionTimeout = const Duration(seconds: 12);
  @override
  Future<UpdateResponse> get(Uri uri) => _get(uri);
  @override
  Future<UpdateResponse> getFrom(Uri uri, int from, String entityTag) =>
      _get(uri, from: from, entityTag: entityTag);

  Future<UpdateResponse> _get(
    Uri uri, {
    int from = 0,
    String? entityTag,
  }) async {
    final origin = normalizeBrokerOrigin(uri.origin);
    final admission = BrokerTransport.token(origin);
    if (origin.scheme == 'http') {
      await ensureBrokerTransport(origin, client: _client, force: true);
    }
    BrokerTransport.requireCurrent(origin, admission);
    if (uri.userInfo.isNotEmpty || uri.hasFragment) {
      throw const UpdateFailure('invalid_catalog');
    }
    final request = await _client
        .getUrl(uri)
        .timeout(const Duration(seconds: 15));
    BrokerTransport.requireCurrent(origin, admission);
    request.followRedirects = false;
    request.headers.set(brokerWireHeader, brokerWireVersion);
    // The server's few human sentences come in the app's language.
    request.headers.set(HttpHeaders.acceptLanguageHeader, acceptLanguage);
    request.headers.set(
      HttpHeaders.acceptHeader,
      'application/json, application/vnd.microsoft.portable-executable',
    );
    if (from > 0 && entityTag != null) {
      request.headers.set(HttpHeaders.rangeHeader, 'bytes=$from-');
      request.headers.set(HttpHeaders.ifRangeHeader, entityTag);
    }
    final response = await request.close().timeout(const Duration(seconds: 15));
    return UpdateResponse(
      response.statusCode,
      response.contentLength,
      response,
      contentRange: response.headers.value(HttpHeaders.contentRangeHeader),
    );
  }

  @override
  void close() => _client.close(force: true);
}

class DownloadedUpdate {
  DownloadedUpdate(this.release, this.file, this.directory);
  final ProductRelease release;
  final File file;
  final Directory directory;
  Future<void> discard() async {
    try {
      if (await file.exists()) await file.delete();
    } on FileSystemException {
      /* Retain a file still held by Windows. */
    }
    try {
      if (await directory.exists()) await directory.delete();
    } on FileSystemException {
      /* Never recursively remove an altered directory. */
    }
  }
}

/// The newer [product] version in [server]'s public catalog, or null when
/// [currentVersion] is the latest or unknown. A catalog that cannot be read
/// (no network, a server error, an unreadable answer) throws instead: that
/// says nothing about the version, so the caller keeps what it knew.
/// Only the catalog is read; nothing is downloaded — it drives the header's
/// update badge, while installing stays the user's choice in the panel.
Future<String?> newerProductVersion(
  String server,
  String product,
  String currentVersion, {
  UpdateTransport? transport,
}) async {
  // An unknown running version has nothing to compare: no badge.
  if (!_version.hasMatch(currentVersion)) return null;
  ProductUpdates? updates;
  try {
    updates = ProductUpdates(server, transport: transport);
    final release = await updates.check(product, currentVersion);
    return release != null &&
            compareProductVersions(release.version, currentVersion) > 0
        ? release.version
        : null;
  } finally {
    updates?.close();
  }
}

class ProductUpdates {
  ProductUpdates(
    String server, {
    UpdateTransport? transport,
    this.temporaryRoot,
    this.retryDelay = const Duration(seconds: 2),
  }) : origin = parseServerOrigin(server),
       _transport = transport ?? HttpsUpdateTransport() {
    try {
      _transportToken = BrokerTransport.token(origin);
    } catch (_) {
      _transport.close();
      rethrow;
    }
  }
  final Uri origin;
  late final Object? _transportToken;
  final UpdateTransport _transport;

  /// Where each download's own `Multitap-update-*` folder is created: null
  /// is the system temporary folder; tests supply a root they remove.
  final Directory? temporaryRoot;

  /// The pause before the n-th retry of an interrupted download is n times
  /// this; tests pass zero.
  final Duration retryDelay;

  /// Requests per download: the first and up to three that continue an
  /// interrupted transfer (network, timeout or a server busy restarting).
  static const downloadAttempts = 4;

  /// The whole download, retries included. A stall ends an attempt sooner
  /// (15 s without bytes).
  static const downloadDeadline = Duration(minutes: 30);
  bool _closed = false;
  void close() {
    if (_closed) return;
    _closed = true;
    _transport.close();
  }

  void _check() {
    if (_closed) throw const UpdateFailure('cancelled');
    if (!BrokerTransport.current(origin, _transportToken)) {
      throw const UpdateFailure('cancelled');
    }
  }

  /// The configured server's public release list, bounded in bytes and count.
  Future<List<dynamic>> _catalog({required int maximumEntries}) async {
    _check();
    final response = await _transport.get(origin.resolve('/api/v1/downloads'));
    if (response.status != 200 || response.contentLength > 512 * 1024) {
      throw const UpdateFailure('catalog_unavailable');
    }
    final bytes = <int>[];
    await for (final chunk in response.body.timeout(
      const Duration(seconds: 15),
    )) {
      _check();
      if (bytes.length + chunk.length > 512 * 1024) {
        throw const UpdateFailure('invalid_catalog');
      }
      bytes.addAll(chunk);
    }
    _check();
    final catalog = jsonDecode(utf8.decode(bytes));
    if (catalog is! Map ||
        catalog['releases'] is! List ||
        (catalog['releases'] as List).length > maximumEntries) {
      throw const UpdateFailure('invalid_catalog');
    }
    return catalog['releases'] as List;
  }

  /// Every published [product] installer with its notes, newest first, for
  /// the release history ("변경 내역"). Each entry passes the same
  /// [ProductRelease.parse] validation as an update; unlike [check], an
  /// invalid entry is left out instead of failing the list, and a version
  /// listed twice is ambiguous, so neither copy is shown. Nothing here is
  /// downloaded or installed.
  Future<List<ProductRelease>> history(String product) async {
    if (!['client', 'host'].contains(product)) {
      throw const UpdateFailure('invalid_product');
    }
    // The server keeps up to 128 immutable entries; its public feed is a
    // bounded subset of them.
    final entries = await _catalog(maximumEntries: 128);
    final releases = <ProductRelease>[];
    for (final item in entries) {
      try {
        final release = ProductRelease.parse(item, origin, product);
        if (release != null) releases.add(release);
      } on UpdateFailure {
        continue;
      }
    }
    String identity(ProductRelease release) =>
        release.version.split('.').map(int.parse).join('.');
    final counts = <String, int>{};
    for (final release in releases) {
      counts.update(identity(release), (n) => n + 1, ifAbsent: () => 1);
    }
    return [
      for (final release in releases)
        if (counts[identity(release)] == 1) release,
    ]..sort((a, b) => compareProductVersions(b.version, a.version));
  }

  /// The largest history page body: 100 versions with 8000-byte notes.
  static const _maximumHistoryPage = 2 * 1024 * 1024;

  /// One page of [product]'s release history from the configured server's
  /// public notes history (`/api/v1/releases/history`, every version it ever
  /// published, also after its installer was pruned), newest first and
  /// older than [before]. A server without that endpoint (404/400/405) gives
  /// its download catalog's notes as a single page instead ([history]).
  /// Invalid or repeated entries are left out. Nothing is downloaded.
  Future<ReleaseHistoryPage> historyPage(
    String product, {
    String? before,
    int limit = 40,
  }) async {
    if (!['client', 'host'].contains(product) ||
        (before != null && !_version.hasMatch(before)) ||
        limit < 1 ||
        limit > 100) {
      throw const UpdateFailure('invalid_product');
    }
    _check();
    final response = await _transport.get(
      origin
          .resolve('/api/v1/releases/history')
          .replace(
            queryParameters: {
              'product': product,
              'limit': '$limit',
              'before': ?before,
            },
          ),
    );
    if (before == null && const [400, 404, 405].contains(response.status)) {
      // Released brokers: drain the refusal, then read the feed's notes.
      try {
        await response.body.drain<void>().timeout(const Duration(seconds: 5));
      } catch (_) {
        /* The fallback is a new request. */
      }
      final releases = await history(product);
      return ReleaseHistoryPage([
        for (final release in releases)
          ReleaseNote(
            version: release.version,
            changelog: release.changelog,
            releasedAt: release.releasedAt,
          ),
      ], total: releases.length);
    }
    if (response.status != 200 ||
        response.contentLength > _maximumHistoryPage) {
      throw const UpdateFailure('catalog_unavailable');
    }
    final bytes = <int>[];
    await for (final chunk in response.body.timeout(
      const Duration(seconds: 15),
    )) {
      _check();
      if (bytes.length + chunk.length > _maximumHistoryPage) {
        throw const UpdateFailure('invalid_catalog');
      }
      bytes.addAll(chunk);
    }
    _check();
    final Object? page;
    try {
      page = jsonDecode(utf8.decode(bytes));
    } on FormatException {
      throw const UpdateFailure('invalid_catalog');
    }
    if (page is! Map || page['releases'] is! List) {
      throw const UpdateFailure('invalid_catalog');
    }
    final items = page['releases'] as List;
    if (items.length > 100) throw const UpdateFailure('invalid_catalog');
    final releases = <ReleaseNote>[];
    final seen = <String>{};
    for (final item in items) {
      final note = ReleaseNote.parse(item);
      if (note == null) continue;
      // Newest first and strictly older than [before]; anything else is
      // dropped so a confused server cannot loop the list.
      if (before != null && compareProductVersions(note.version, before) >= 0) {
        continue;
      }
      if (releases.isNotEmpty &&
          compareProductVersions(note.version, releases.last.version) >= 0) {
        continue;
      }
      if (seen.add(note.version)) releases.add(note);
    }
    final next = page['next'], total = page['total'];
    final more =
        next is String &&
        _version.hasMatch(next) &&
        releases.isNotEmpty &&
        compareProductVersions(next, releases.last.version) <= 0 &&
        (before == null || compareProductVersions(next, before) < 0);
    return ReleaseHistoryPage(
      releases,
      next: more ? next : null,
      total: total is int && total >= 0 ? total : null,
    );
  }

  Future<ProductRelease?> check(String product, String currentVersion) async {
    if (!['client', 'host'].contains(product) ||
        !_version.hasMatch(currentVersion)) {
      throw const UpdateFailure('invalid_product');
    }
    final catalog = await _catalog(maximumEntries: 32);
    ProductRelease? latest;
    final versions = <String>{};
    for (final item in catalog) {
      final release = ProductRelease.parse(item, origin, product);
      if (release == null) continue;
      if (!versions.add(release.version.split('.').map(int.parse).join('.'))) {
        throw const UpdateFailure('invalid_catalog');
      }
      if (latest == null ||
          compareProductVersions(release.version, latest.version) > 0) {
        latest = release;
      }
    }
    return latest;
  }

  /// [verifying] runs once every byte arrived, before the exact size and
  /// SHA-256 checks, so the caller can show that step separately.
  /// [retrying] runs before each request that continues an interrupted
  /// transfer, with its number (2, 3, ...). Throws [UpdateFailure] only, with
  /// a [downloadFailureCodes] code, `cancelled` or `signature_invalid`.
  Future<DownloadedUpdate> download(
    ProductRelease release,
    void Function(int, int) progress, {
    void Function()? verifying,
    void Function(int attempt)? retrying,
  }) async {
    _check();
    release._validateFor(origin);
    // Every release is signed with the project's key; one without a signature
    // is never downloaded or installed (the native side verifies the value).
    if (release.signature == null) {
      throw const UpdateFailure('signature_invalid');
    }
    final Directory directory;
    try {
      directory = await (temporaryRoot ?? Directory.systemTemp).createTemp(
        'Multitap-update-',
      );
    } catch (error) {
      throw UpdateFailure(_downloadFailure(error));
    }
    final file = File('${directory.path}/${release.filename}');
    RandomAccessFile? output;
    try {
      output = await file.open(mode: FileMode.write);
      final clock = Stopwatch()..start();
      var count = 0;
      for (var attempt = 1; ; attempt++) {
        try {
          await _receive(release, output, count, progress, clock);
          break;
        } catch (error) {
          final code = _closed ? 'cancelled' : _downloadFailure(error);
          if (attempt >= downloadAttempts ||
              !_retriedDownloadFailures.contains(code) ||
              clock.elapsed > downloadDeadline) {
            throw UpdateFailure(code);
          }
        }
        // The bytes already written stay; the next request continues there.
        count = await output.position();
        if (retryDelay > Duration.zero) {
          await Future<void>.delayed(retryDelay * attempt);
        }
        _check();
        retrying?.call(attempt + 1);
      }
      final received = await output.position();
      await output.close();
      output = null;
      _check();
      verifying?.call();
      if (received != release.size ||
          (await sha256.bind(file.openRead()).first).toString() !=
              release.sha256) {
        throw const UpdateFailure('hash_mismatch');
      }
      _check();
      return DownloadedUpdate(release, file, directory);
    } catch (error) {
      try {
        await output?.close();
      } catch (_) {
        /* Preserve original failure. */
      }
      await DownloadedUpdate(release, file, directory).discard();
      if (error is UpdateFailure) rethrow;
      throw UpdateFailure(_closed ? 'cancelled' : _downloadFailure(error));
    }
  }

  /// One request for the rest of the file from [from] (all of it when the
  /// server does not continue), written to [output] from where it starts.
  Future<void> _receive(
    ProductRelease release,
    RandomAccessFile output,
    int from,
    void Function(int, int) progress,
    Stopwatch clock,
  ) async {
    _check();
    final transport = _transport;
    final ranged = from > 0 && transport is RangeUpdateTransport;
    final response = ranged
        ? await transport.getFrom(
            release.url,
            from,
            '"sha256-${release.sha256}"',
          )
        : await transport.get(release.url);
    _check();
    var count = 0;
    if (ranged && response.status == 206) {
      if (response.contentRange !=
              'bytes $from-${release.size - 1}/${release.size}' ||
          (response.contentLength >= 0 &&
              response.contentLength != release.size - from)) {
        throw const UpdateFailure('download_size_mismatch');
      }
      count = from;
    } else if (response.status != 200) {
      throw UpdateFailure(switch (response.status) {
        404 || 410 => 'download_not_found',
        408 || 429 || >= 500 => 'download_server_error',
        _ => 'download_http_status',
      });
    } else if (response.contentLength >= 0 &&
        response.contentLength != release.size) {
      throw const UpdateFailure('download_size_mismatch');
    }
    await output.setPosition(count);
    await output.truncate(count);
    await for (final chunk in response.body.timeout(
      const Duration(seconds: 15),
    )) {
      _check();
      if (count + chunk.length > release.size) {
        throw const UpdateFailure('download_size_mismatch');
      }
      if (clock.elapsed > downloadDeadline) {
        throw const UpdateFailure('download_timeout');
      }
      // Each chunk is on disk before the next: memory stays bounded.
      await output.writeFrom(chunk);
      count += chunk.length;
      progress(count, release.size);
    }
    _check();
    // A body that ended early without an error is an interrupted transfer.
    if (count < release.size) throw const UpdateFailure('download_network');
  }
}

/// Fixed download outcomes (client diagnostics): network, server, disk and
/// verification apart. `download_failed` is anything not classified.
const downloadFailureCodes = {
  // Network: connection refused/reset/closed or TLS; no bytes for 15 s or
  // the whole download past its deadline.
  'download_network',
  'download_timeout',
  // Server: busy or restarting (5xx, 408, 429); the release file is gone
  // (404, 410); any other refusal or redirect.
  'download_server_error',
  'download_not_found',
  'download_http_status',
  // Disk: the temporary folder is full; it cannot be created or written
  // (permissions, another program holding the file).
  'download_disk_full',
  'download_file_error',
  // Verification: a length other than the release's (header or bytes).
  // A wrong SHA-256 is hash_mismatch.
  'download_size_mismatch',
  'download_failed',
};

const _retriedDownloadFailures = {
  'download_network',
  'download_timeout',
  'download_server_error',
};

String _downloadFailure(Object error) => switch (error) {
  UpdateFailure(:final code) => code,
  TimeoutException() => 'download_timeout',
  // ERROR_HANDLE_DISK_FULL, ERROR_DISK_FULL
  FileSystemException(:final osError)
      when osError?.errorCode == 39 || osError?.errorCode == 112 =>
    'download_disk_full',
  FileSystemException() => 'download_file_error',
  SocketException() || HttpException() || TlsException() => 'download_network',
  _ => 'download_failed',
};

/// How the verified installer runs. [apply]: its unattended update mode with
/// the one-process handoff (update-handoff-v1): this app closes once the
/// installer accepted the update and the installer reopens it afterwards.
/// [manual]: the ordinary graphical installer with its own Install click.
enum UpdateInstallMode { apply, manual }

/// Fixed codes an update step may end with. Anything else a platform reports
/// is shown as [unknownUpdateCode]; no exception text reaches the screen.
const updateFailureCodes = {
  'catalog_unavailable',
  'invalid_catalog',
  // download_failed and the other download codes: downloadFailureCodes.
  'hash_mismatch',
  'package_version_mismatch',
  'installer_start_failed',
  // The Windows administrator approval for the installer was declined.
  'elevation_cancelled',
  // The release is older than the running app (never installed as an update).
  'update_not_newer',
  'installer_exited',
  'handoff_unavailable',
  'session_windows_open',
  'version_unknown',
  'update_busy',
  'install_failed',
  'rollback_failed',
  'installer_missing',
  'progress_channel_failed',
  'existing_install_required',
  'certificate_trust_required',
  'driver_install_required',
  // No release signature, or one that does not verify against the project
  // key pinned in this build.
  'signature_invalid',
  // The release did not come from the server kept for this user.
  'update_origin_unverified',
  ...downloadFailureCodes,
};
const unknownUpdateCode = 'installer_start_failed';

/// A Host update that needs a person: publisher trust, a display driver
/// change or a first installation. The graphical installer continues it.
bool updateNeedsInstaller(String code) =>
    code == 'existing_install_required' ||
    code == 'certificate_trust_required' ||
    code == 'driver_install_required';

/// The native side of an update. Tests substitute their own.
abstract class UpdatePlatform {
  const UpdatePlatform();

  /// The downloaded installer EXE's own version resource names exactly the
  /// release version. Throws [UpdateFailure].
  Future<void> verifyInstaller(DownloadedUpdate update);

  /// Remote session windows of this client are open.
  Future<bool> hasRemoteSessions();
  Future<void> closeRemoteSessions();

  /// [UpdateInstallMode.apply] resolves once the installer accepted the
  /// update; this app then closes by itself. Throws [UpdateFailure] with the
  /// installer's fixed code while this app stays open.
  Future<void> install(
    DownloadedUpdate update,
    UpdateInstallMode mode, {
    required String currentVersion,
  });
}

class NativeUpdatePlatform extends UpdatePlatform {
  const NativeUpdatePlatform({
    this.channel = const MethodChannel('multitap/window'),
  });
  final MethodChannel channel;

  Map<String, Object> _arguments(DownloadedUpdate update) {
    final release = update.release;
    return {
      'path': update.file.path,
      'filename': release.filename,
      'product': release.product,
      'version': release.version,
      'size': release.size,
      'sha256': release.sha256,
      'signature': release.signature ?? '',
      // The server the installer came from, for the record only: the runner
      // does not check it. What guards the installer is the pinned release
      // signature above (ProductUpdateSignatureValid in flutter_window.cpp),
      // checked before the restart is offered and again before it starts.
      'origin': release.url.origin,
    };
  }

  Future<T?> _call<T>(String method, Object arguments) async {
    try {
      return await channel.invokeMethod<T>(method, arguments);
    } on PlatformException catch (error) {
      throw UpdateFailure(
        updateFailureCodes.contains(error.code)
            ? error.code
            : unknownUpdateCode,
      );
    } on MissingPluginException {
      throw const UpdateFailure(unknownUpdateCode);
    }
  }

  @override
  Future<void> verifyInstaller(DownloadedUpdate update) =>
      _call<bool>('verifyUpdate', _arguments(update));

  @override
  Future<bool> hasRemoteSessions() async {
    try {
      return await channel.invokeMethod<bool>('hasSessionWindows') ?? false;
    } on MissingPluginException {
      return false;
    }
  }

  @override
  Future<void> closeRemoteSessions() => SessionWindows.closeAll('update');

  @override
  Future<void> install(
    DownloadedUpdate update,
    UpdateInstallMode mode, {
    required String currentVersion,
  }) => _call<Object>('installUpdate', {
    ..._arguments(update),
    'mode': mode.name,
    'currentVersion': currentVersion,
  });
}
