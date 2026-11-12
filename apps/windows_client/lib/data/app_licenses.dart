import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import '../l10n/l10n.dart';

const nativeLicenseManifestAsset = 'assets/licenses/native_notices.json';

/// The APK carries none of the Windows client's native parts (UsbDk, the
/// Visual C++ runtime, usb.ids, libopus, WebView2), so Android lists its own.
const androidLicenseManifestAsset =
    'assets/licenses/android_native_notices.json';

/// The client inventory for the running app: the Android app reads its own
/// list; any other manifest (the Host's) is kept as given. This follows the
/// real platform (not [defaultTargetPlatform], which tests set to Android).
String licenseManifestFor(String manifestAsset, {bool? android}) =>
    manifestAsset == nativeLicenseManifestAsset &&
        (android ?? Platform.isAndroid)
    ? androidLicenseManifestAsset
    : manifestAsset;

class AppLicenseNotice {
  const AppLicenseNotice({
    required this.text,
    required this.license,
    this.version,
    this.source,
  });

  final String text, license;
  final String? version, source;
}

class AppLicenseComponent {
  AppLicenseComponent(this.name, Iterable<AppLicenseNotice> notices)
    : notices = List.unmodifiable(notices);

  final String name;
  final List<AppLicenseNotice> notices;

  bool matches(String query) {
    if (query.isEmpty || name.toLowerCase().contains(query)) return true;
    return notices.any(
      (notice) => [
        notice.license,
        notice.version ?? '',
        notice.source ?? '',
        notice.text,
      ].any((text) => text.toLowerCase().contains(query)),
    );
  }
}

enum AppLicenseIssue {
  registeredUnavailable,
  nativeManifestUnavailable,
  nativeNoticeUnavailable,
}

class AppLicenseCatalog {
  AppLicenseCatalog({
    required Iterable<AppLicenseComponent> components,
    Iterable<AppLicenseIssue> issues = const [],
  }) : components = List.unmodifiable(components),
       issues = List.unmodifiable(issues);

  final List<AppLicenseComponent> components;
  final List<AppLicenseIssue> issues;

  /// Collects the complete registered notices and bundled native inventory.
  /// Reads are local assets only. Failures preserve the notices already read;
  /// they never turn a missing inventory into a compliance/success claim.
  static Future<AppLicenseCatalog> load({
    AssetBundle? assets,
    Stream<LicenseEntry> Function()? registered,
    String manifestAsset = nativeLicenseManifestAsset,
  }) async {
    final bundle = assets ?? rootBundle;
    final manifestPath = licenseManifestFor(manifestAsset);
    final notices = <String, List<AppLicenseNotice>>{};
    final names = <String, String>{};
    final issues = <AppLicenseIssue>[];
    void add(String name, AppLicenseNotice notice) {
      final key = name.toLowerCase();
      names.putIfAbsent(key, () => name);
      notices.putIfAbsent(key, () => []).add(notice);
    }

    try {
      final registry = registered?.call() ?? LicenseRegistry.licenses;
      await for (final entry in registry.timeout(const Duration(seconds: 15))) {
        final text = entry is LicenseEntryWithLineBreaks
            ? entry.text
            : entry.paragraphs.map((paragraph) => paragraph.text).join('\n\n');
        final packages = entry.packages.toSet();
        if (packages.isEmpty) packages.add(l10n.unnamedComponent);
        for (final package in packages) {
          add(package, AppLicenseNotice(text: text, license: l10n.packageLicense));
        }
      }
    } catch (_) {
      issues.add(AppLicenseIssue.registeredUnavailable);
    }

    List? components;
    try {
      if (!RegExp(r'^assets/licenses/[A-Za-z0-9][A-Za-z0-9._-]*\.json$')
          .hasMatch(manifestPath)) {
        throw const FormatException('invalid_native_license_manifest_asset');
      }
      final manifest = jsonDecode(
        await bundle
            .loadString(manifestPath, cache: false)
            .timeout(const Duration(seconds: 5)),
      );
      if (manifest is! Map ||
          manifest['schema'] is! int ||
          manifest['schema'] != 1 ||
          manifest['components'] is! List ||
          (manifest['components'] as List).length > 256) {
        throw const FormatException('invalid_native_license_manifest');
      }
      components = manifest['components'] as List;
    } catch (_) {
      issues.add(AppLicenseIssue.nativeManifestUnavailable);
    }

    // Load at most four notices together. A read timeout does not cancel the
    // underlying read: stop further batches if that happens, keeping at most
    // four such local asset reads outstanding. Their late result cannot alter
    // the immutable returned catalog.
    if (components != null) {
      var timedOut = false;
      for (var offset = 0; offset < components.length; offset += 4) {
        final end = (offset + 4).clamp(0, components.length).toInt();
        final loaded = await Future.wait(
          components.sublist(offset, end).map((raw) async {
            try {
              if (raw is! Map ||
                  raw['name'] is! String ||
                  (raw['name'] as String).trim().isEmpty ||
                  raw['license'] is! String ||
                  (raw['license'] as String).trim().isEmpty ||
                  raw['textAsset'] is! String ||
                  (raw['version'] != null && raw['version'] is! String) ||
                  (raw['source'] != null && raw['source'] is! String)) {
                throw const FormatException('invalid_native_license_entry');
              }
              final path = raw['textAsset'] as String;
              // The declared assets/licenses/ folder is flat. Never resolve an
              // arbitrary file/URL or traversal from a bundled manifest field.
              if (!RegExp(r'^assets/licenses/[A-Za-z0-9][A-Za-z0-9._-]*$')
                      .hasMatch(path) ||
                  path == manifestPath) {
                throw const FormatException('invalid_native_license_asset');
              }
              final text = await bundle
                  .loadString(path, cache: false)
                  .timeout(const Duration(seconds: 5));
              if (text.trim().isEmpty) {
                throw const FormatException('empty_native_license');
              }
              return (
                raw['name'] as String,
                AppLicenseNotice(
                  text: text,
                  license: raw['license'] as String,
                  version: raw['version'] as String?,
                  source: raw['source'] as String?,
                ),
              );
            } on TimeoutException {
              timedOut = true;
              return null;
            } catch (_) {
              return null;
            }
          }),
        );
        for (final result in loaded) {
          if (result == null) {
            issues.add(AppLicenseIssue.nativeNoticeUnavailable);
          } else {
            add(result.$1, result.$2);
          }
        }
        if (timedOut) break;
      }
    }

    final sorted = notices.keys.toList()..sort();
    return AppLicenseCatalog(
      components: [
        for (final key in sorted)
          AppLicenseComponent(names[key]!, notices[key]!),
      ],
      issues: issues,
    );
  }
}
