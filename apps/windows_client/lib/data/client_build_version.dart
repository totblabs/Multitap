import 'package:flutter/services.dart';
import '../l10n/l10n.dart';

/// The runner supplies the same build value used in its Windows EXE resource.
/// An unavailable value stays unknown; it must never impersonate an old build.
class ClientBuildVersion {
  const ClientBuildVersion.unknown() : version = null, build = null;
  const ClientBuildVersion._(this.version, this.build);

  final String? version;
  final String? build;
  String get label => version ?? l10n.versionUnknown;

  static ClientBuildVersion parse(Object? value) {
    if (value is! String) return const ClientBuildVersion.unknown();
    final match = RegExp(
      r'^([0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6})(?:\+([0-9]{1,10}))?$',
    ).firstMatch(value);
    if (match == null || match.end != value.length) {
      return const ClientBuildVersion.unknown();
    }
    return ClientBuildVersion._(match[1], match[2]);
  }

  static Future<ClientBuildVersion> load({
    MethodChannel channel = const MethodChannel('multitap/window'),
  }) async {
    try {
      return parse(await channel.invokeMethod<Object?>('productVersion'));
    } on MissingPluginException {
      return const ClientBuildVersion.unknown();
    } on PlatformException {
      return const ClientBuildVersion.unknown();
    }
  }
}

ClientBuildVersion runningClientBuild = const ClientBuildVersion.unknown();
String get clientProductVersion => runningClientBuild.version ?? '';
