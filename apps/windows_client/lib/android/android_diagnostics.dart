import 'android_updates.dart' show androidVersionPattern;

/// The diagnostics wire accepts x.y.z+N, while the APK names previews
/// x.y.z-preview.N. Derive the diagnostic build from the running APK version;
/// an unreadable or malformed native version remains unknown.
String? androidDiagnosticVersion(String? version) {
  if (version == null) return null;
  final match = androidVersionPattern.firstMatch(version);
  if (match == null || match.end != version.length) return null;
  final base = '${match[1]}.${match[2]}.${match[3]}';
  return match[4] == null ? base : '$base+${match[5] ?? '0'}';
}
