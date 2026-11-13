import '../data/broker_transport.dart';
import '../data/broker_probe.dart';

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';

import '../data/product_updates.dart'
    show
        ReleaseHistoryPage,
        ReleaseNote,
        UpdateFailure,
        UpdateResponse,
        UpdateTransport;
import '../l10n/l10n.dart';

/// Android versions as the server's catalog accepts them
/// (server/src/downloads.mjs): x.y.z with an optional -preview or -preview.N.
final androidVersionPattern = RegExp(
  r'^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})(-preview(?:\.([1-9][0-9]{0,5}))?)?$',
);
const androidUpdateAbis = {'arm64-v8a', 'armeabi-v7a', 'x86_64'};
const _maximumApk = 128 * 1024 * 1024;
const _maximumCatalog = 512 * 1024;

/// The server's compareVersion: numeric x.y.z, then the preview number
/// (-preview counts as preview 0); a preview is older than the same x.y.z
/// without one.
int compareAndroidVersions(String a, String b) {
  final left = androidVersionPattern.firstMatch(a);
  final right = androidVersionPattern.firstMatch(b);
  if (left == null || right == null) {
    throw const UpdateFailure('invalid_catalog');
  }
  for (var group = 1; group <= 3; group++) {
    final result = int.parse(left[group]!).compareTo(int.parse(right[group]!));
    if (result != 0) return result;
  }
  final leftPreview = left[4] != null, rightPreview = right[4] != null;
  if (!leftPreview || !rightPreview) {
    return leftPreview == rightPreview ? 0 : (leftPreview ? -1 : 1);
  }
  return int.parse(left[5] ?? '0').compareTo(int.parse(right[5] ?? '0'));
}

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

/// One android-client APK of the configured server's public catalog.
class AndroidRelease {
  const AndroidRelease({
    required this.version,
    required this.abi,
    required this.size,
    required this.sha256,
    required this.url,
    this.changelog = '',
    this.releasedAt,
  });
  final String version, abi, sha256, changelog;
  final int size;
  final Uri url;
  final DateTime? releasedAt;

  String get filename => 'Multitap-$version-android-$abi.apk';

  /// Null for anything that is not an android-client APK of [abi]; an entry
  /// of this product and ABI that breaks the server's own rules makes the
  /// whole catalog untrusted (invalid_catalog), as on Windows.
  static AndroidRelease? parse(dynamic entry, Uri origin, String abi) {
    if (entry is! Map ||
        entry['product'] != 'android-client' ||
        entry['abi'] != abi) {
      return null;
    }
    final version = entry['version'];
    if (entry['format'] != 'apk' ||
        version is! String ||
        androidVersionPattern.firstMatch(version) == null) {
      throw const UpdateFailure('invalid_catalog');
    }
    final filename = 'Multitap-$version-android-$abi.apk';
    final size = entry['size'], digest = entry['sha256'];
    final notes = entry['changelog'] ?? '';
    if (entry['filename'] != filename ||
        entry['url'] != '/downloads/$filename' ||
        size is! int ||
        size < 1 ||
        size > _maximumApk ||
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
    return AndroidRelease(
      version: version,
      abi: abi,
      size: size,
      sha256: digest,
      url: origin.resolve('/downloads/$filename'),
      changelog: notes,
      releasedAt: releasedAt,
    );
  }

  /// Only the authorized scheme and origin's /downloads/ path, with nothing
  /// else in the URL, and only the server's own release shape.
  void validateFor(Uri origin) {
    if (!brokerOriginAllowed(origin) ||
        url.scheme != origin.scheme ||
        !url.hasAuthority ||
        url.origin != origin.origin ||
        url.userInfo.isNotEmpty ||
        url.hasQuery ||
        url.hasFragment ||
        url.path != '/downloads/$filename' ||
        !androidUpdateAbis.contains(abi) ||
        androidVersionPattern.firstMatch(version) == null ||
        size < 1 ||
        size > _maximumApk ||
        !RegExp(r'^[a-f0-9]{64}$').hasMatch(sha256)) {
      throw const UpdateFailure('invalid_catalog');
    }
  }
}

/// Authorized broker GET without redirects, for the catalog and the APK.
class AndroidUpdateTransport implements UpdateTransport {
  final _client = HttpClient()..connectionTimeout = const Duration(seconds: 12);
  @override
  Future<UpdateResponse> get(Uri uri) async {
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
      'application/json, application/vnd.android.package-archive',
    );
    final response = await request.close().timeout(const Duration(seconds: 15));
    return UpdateResponse(
      response.statusCode,
      response.contentLength,
      response,
    );
  }

  @override
  void close() => _client.close(force: true);
}

class _DigestSink implements Sink<Digest> {
  Digest? value;
  @override
  void add(Digest data) => value = data;
  @override
  void close() {}
}

/// Reads the configured server's public catalog and downloads one release.
/// Checking never downloads; installing stays the user's choice.
class AndroidUpdates {
  AndroidUpdates(this.origin, {UpdateTransport? transport})
    : _transport = transport ?? AndroidUpdateTransport() {
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

  /// The newest catalog APK for [abi] that is newer than [current], or null.
  Future<AndroidRelease?> check(String current, String abi) async {
    if (androidVersionPattern.firstMatch(current) == null) {
      throw const UpdateFailure('version_unknown');
    }
    final latest = await this.latest(abi);
    return latest != null && compareAndroidVersions(latest.version, current) > 0
        ? latest
        : null;
  }

  /// The newest catalog APK for [abi] whatever the running version is (the
  /// panel shows it as 최신 even when nothing newer exists), or null.
  Future<AndroidRelease?> latest(String abi) async {
    if (!androidUpdateAbis.contains(abi)) {
      throw const UpdateFailure('abi_unavailable');
    }
    _check();
    if (!brokerOriginAllowed(origin)) {
      throw const UpdateFailure('invalid_catalog');
    }
    final response = await _transport.get(origin.resolve('/api/v1/downloads'));
    if (response.status != 200 || response.contentLength > _maximumCatalog) {
      throw const UpdateFailure('catalog_unavailable');
    }
    final bytes = <int>[];
    await for (final chunk in response.body.timeout(
      const Duration(seconds: 15),
    )) {
      _check();
      if (bytes.length + chunk.length > _maximumCatalog) {
        throw const UpdateFailure('invalid_catalog');
      }
      bytes.addAll(chunk);
    }
    final Object? catalog;
    try {
      catalog = jsonDecode(utf8.decode(bytes));
    } on FormatException {
      throw const UpdateFailure('invalid_catalog');
    }
    if (catalog is! Map ||
        catalog['releases'] is! List ||
        (catalog['releases'] as List).length > 32) {
      throw const UpdateFailure('invalid_catalog');
    }
    AndroidRelease? latest;
    final versions = <String>{};
    for (final item in catalog['releases'] as List) {
      final release = AndroidRelease.parse(item, origin, abi);
      if (release == null) continue;
      // The same version twice is ambiguous; never pick an arbitrary one.
      if (!versions.add(release.version)) {
        throw const UpdateFailure('invalid_catalog');
      }
      if (latest == null ||
          compareAndroidVersions(release.version, latest.version) > 0) {
        latest = release;
      }
    }
    return latest;
  }

  static const _maximumHistoryPage = 2 * 1024 * 1024;

  /// One page of the Android release notes (newest first, older than
  /// [before]) from the server's public notes history, which keeps every
  /// version ever published; a server without that endpoint gives the
  /// download catalog's notes as one page. Nothing is downloaded.
  Future<ReleaseHistoryPage> historyPage(
    String abi, {
    String? before,
    int limit = 40,
  }) async {
    _check();
    if (!androidUpdateAbis.contains(abi) ||
        (before != null && androidVersionPattern.firstMatch(before) == null) ||
        limit < 1 ||
        limit > 100 ||
        !brokerOriginAllowed(origin)) {
      throw const UpdateFailure('invalid_catalog');
    }
    _check();
    final response = await _transport.get(
      origin
          .resolve('/api/v1/releases/history')
          .replace(
            queryParameters: {
              'product': 'android-client',
              'limit': '$limit',
              'before': ?before,
            },
          ),
    );
    if (before == null && const [400, 404, 405].contains(response.status)) {
      try {
        await response.body.drain<void>().timeout(const Duration(seconds: 5));
      } catch (_) {
        /* The fallback is a new request. */
      }
      return _catalogNotes(abi);
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
    final Object? page;
    try {
      page = jsonDecode(utf8.decode(bytes));
    } on FormatException {
      throw const UpdateFailure('invalid_catalog');
    }
    if (page is! Map ||
        page['releases'] is! List ||
        (page['releases'] as List).length > 100) {
      throw const UpdateFailure('invalid_catalog');
    }
    final notes = <ReleaseNote>[];
    final seen = <String>{};
    for (final item in page['releases'] as List) {
      final note = _note(item);
      if (note == null) continue;
      // Strictly older than [before] and descending; anything else is
      // dropped so a confused server cannot loop the list.
      if (before != null && compareAndroidVersions(note.version, before) >= 0) {
        continue;
      }
      if (notes.isNotEmpty &&
          compareAndroidVersions(note.version, notes.last.version) >= 0) {
        continue;
      }
      if (seen.add(note.version)) notes.add(note);
    }
    final next = page['next'], total = page['total'];
    final more =
        next is String &&
        androidVersionPattern.firstMatch(next) != null &&
        notes.isNotEmpty &&
        compareAndroidVersions(next, notes.last.version) <= 0 &&
        (before == null || compareAndroidVersions(next, before) < 0);
    return ReleaseHistoryPage(
      notes,
      next: more ? next : null,
      total: total is int && total >= 0 ? total : null,
    );
  }

  /// A note entry with the catalog's own text rules, or null.
  static ReleaseNote? _note(dynamic entry) {
    if (entry is! Map) return null;
    final version = entry['version'], notes = entry['changelog'] ?? '';
    if (version is! String ||
        androidVersionPattern.firstMatch(version) == null ||
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

  Future<ReleaseHistoryPage> _catalogNotes(String abi) async {
    final response = await _transport.get(origin.resolve('/api/v1/downloads'));
    if (response.status != 200 || response.contentLength > _maximumCatalog) {
      throw const UpdateFailure('catalog_unavailable');
    }
    final bytes = <int>[];
    await for (final chunk in response.body.timeout(
      const Duration(seconds: 15),
    )) {
      _check();
      if (bytes.length + chunk.length > _maximumCatalog) {
        throw const UpdateFailure('invalid_catalog');
      }
      bytes.addAll(chunk);
    }
    final Object? catalog;
    try {
      catalog = jsonDecode(utf8.decode(bytes));
    } on FormatException {
      throw const UpdateFailure('invalid_catalog');
    }
    if (catalog is! Map || catalog['releases'] is! List) {
      throw const UpdateFailure('invalid_catalog');
    }
    final byVersion = <String, ReleaseNote>{};
    for (final item in catalog['releases'] as List) {
      if (item is! Map ||
          item['product'] != 'android-client' ||
          item['abi'] != abi) {
        continue;
      }
      final note = _note(item);
      if (note != null) byVersion[note.version] = note;
    }
    final notes = byVersion.values.toList()
      ..sort((a, b) => compareAndroidVersions(b.version, a.version));
    return ReleaseHistoryPage(notes, total: notes.length);
  }

  /// Downloads [release] into [directory] (app-private) and returns the
  /// file only when its exact size and SHA-256 match the catalog. Any
  /// failure removes the partial file.
  Future<File> download(
    AndroidRelease release,
    Directory directory,
    void Function(int received, int total) progress, {
    void Function()? verifying,
  }) async {
    _check();
    release.validateFor(origin);
    final partial = File('${directory.path}/${release.filename}.part');
    final target = File('${directory.path}/${release.filename}');
    IOSink? sink;
    try {
      final response = await _transport.get(release.url);
      _check();
      if (response.status != 200 ||
          (response.contentLength >= 0 &&
              response.contentLength != release.size)) {
        throw const UpdateFailure('download_failed');
      }
      sink = partial.openWrite();
      final digest = _DigestSink();
      final hashing = sha256.startChunkedConversion(digest);
      var count = 0;
      final clock = Stopwatch()..start();
      await for (final chunk in response.body.timeout(
        const Duration(seconds: 20),
      )) {
        _check();
        count += chunk.length;
        if (count > release.size ||
            clock.elapsed > const Duration(minutes: 10)) {
          throw const UpdateFailure('download_failed');
        }
        hashing.add(chunk);
        sink.add(chunk);
        await sink.flush();
        progress(count, release.size);
      }
      hashing.close();
      await sink.close();
      sink = null;
      _check();
      verifying?.call();
      if (count != release.size ||
          digest.value?.toString() != release.sha256 ||
          await partial.length() != release.size) {
        throw const UpdateFailure('hash_mismatch');
      }
      return await partial.rename(target.path);
    } catch (error) {
      try {
        await sink?.close();
      } catch (_) {
        /* Keep the original failure. */
      }
      for (final file in [partial, target]) {
        try {
          if (await file.exists()) await file.delete();
        } on FileSystemException {
          /* Nothing else to remove. */
        }
      }
      if (error is UpdateFailure) rethrow;
      if (error is FileSystemException) {
        throw const UpdateFailure('storage_failed');
      }
      throw const UpdateFailure('download_failed');
    }
  }
}

/// The Android side of an update. Tests substitute their own.
abstract class AndroidUpdatePlatform {
  /// The installed split's ABI, or null (debug build / unsupported).
  Future<String?> abi();

  /// App-private download directory, emptied of earlier downloads.
  Future<Directory> directory();

  /// Android's "install unknown apps" permission for this app.
  Future<bool> canInstall();
  Future<void> openInstallSettings();

  /// Hands [file] to the Android installer, which asks the user to confirm.
  /// Resolves once handed off; the native side deletes the file either way.
  Future<void> install(File file, AndroidRelease release);
  Future<void> discard(File file);

  /// Installer results after the hand-off (fixed codes), and one step that
  /// is not a result: [androidUpdateConfirmOpened].
  Stream<String> get statuses;
}

/// Android's own confirmation screen was opened and waits for the person:
/// a step on [AndroidUpdatePlatform.statuses], never a result.
const androidUpdateConfirmOpened = 'confirm_opened';

/// A platform that says how much of the file it has copied into Android's
/// install session. The hand-off copies and hashes the whole APK once more,
/// and those bytes of the known size are the one measure it has; checking
/// the package before and committing after have none.
abstract interface class AndroidUpdateInstallProgress {
  Stream<({int copied, int total})> get installProgress;
}

const androidUpdateCodes = {
  'catalog_unavailable',
  'invalid_catalog',
  'download_failed',
  'hash_mismatch',
  'storage_failed',
  'version_unknown',
  'abi_unavailable',
  'install_permission_required',
  'invalid_update',
  'package_mismatch',
  'not_newer',
  'update_busy',
  'installer_start_failed',
  'installer_state_unverified',
  'cancelled',
};

class NativeAndroidUpdatePlatform
    implements AndroidUpdatePlatform, AndroidUpdateInstallProgress {
  NativeAndroidUpdatePlatform() {
    channel.setMethodCallHandler((call) async {
      final arguments = call.arguments;
      if (call.method == 'installStatus' &&
          arguments is Map &&
          arguments['status'] is String) {
        _statuses.add(arguments['status'] as String);
      }
      if (call.method == 'installProgress' && arguments is Map) {
        final copied = arguments['copied'], total = arguments['size'];
        if (copied is int && total is int && total > 0 && copied >= 0) {
          _installProgress.add((
            copied: copied > total ? total : copied,
            total: total,
          ));
        }
      }
      return null;
    });
  }
  static const channel = MethodChannel('multitap/android/update');
  final _statuses = StreamController<String>.broadcast();
  final _installProgress =
      StreamController<({int copied, int total})>.broadcast();
  @override
  Stream<({int copied, int total})> get installProgress =>
      _installProgress.stream;

  Future<T?> _call<T>(String method, [Object? arguments]) async {
    try {
      return await channel.invokeMethod<T>(method, arguments);
    } on PlatformException catch (error) {
      throw UpdateFailure(
        androidUpdateCodes.contains(error.code)
            ? error.code
            : 'installer_start_failed',
      );
    } on MissingPluginException {
      throw const UpdateFailure('installer_start_failed');
    }
  }

  @override
  Future<String?> abi() => _call<String>('abi');
  @override
  Future<Directory> directory() async {
    final path = await _call<String>('directory');
    if (path == null) throw const UpdateFailure('storage_failed');
    return Directory(path);
  }

  @override
  Future<bool> canInstall() async => await _call<bool>('canInstall') == true;
  @override
  Future<void> openInstallSettings() => _call<bool>('openInstallSettings');
  @override
  Future<void> install(File file, AndroidRelease release) =>
      _call<String>('install', {
        'path': file.path,
        'version': release.version,
        'size': release.size,
        'sha256': release.sha256,
      });
  @override
  Future<void> discard(File file) =>
      _call<void>('discard', {'path': file.path});
  @override
  Stream<String> get statuses => _statuses.stream;
}

/// 해요체 text for a fixed failure code.
String androidUpdateMessage(String code) => switch (code) {
  'catalog_unavailable' => l10n.couldntGetUpdateInformationCheck,
  'invalid_catalog' => l10n.serversUpdateInformationIsntValid,
  'download_failed' => l10n.couldntDownloadUpdateFileTry,
  'hash_mismatch' => l10n.downloadedFilesSizeSha256,
  'storage_failed' => l10n.couldntSaveUpdateFileCheck,
  'version_unknown' => l10n.cantCheckUpdatesBecauseApps,
  'abi_unavailable' => l10n.cantCheckUpdatesFitDevice,
  'install_permission_required' =>
    l10n.allowInstallUnknownAppsMultitap,
  'package_mismatch' => l10n.downloadedFileIsntMultitapUpdate,
  'not_newer' => l10n.downloadedFileIsntNewerThan,
  'update_busy' => l10n.anotherUpdateBeingInstalledTry,
  'installer_state_unverified' =>
    l10n.couldntCheckInstallStatusRestart,
  'cancelled' => l10n.updateCanceled,
  // Installer results after the hand-off.
  'aborted' => l10n.updateInstallCanceledYouKeep,
  'blocked' => l10n.androidBlockedUpdateInstallCheck,
  'conflict' => l10n.couldntUpdateBecauseDoesntMatch,
  'incompatible' => l10n.updateIsntCompatibleDevice,
  'storage' => l10n.notEnoughStorageInstallUpdate,
  'invalid' => l10n.androidFoundUpdateFileInvalid,
  'timeout' => l10n.updateInstallTimedOutTry,
  _ => l10n.couldntOpenAndroidInstallerTry,
};
