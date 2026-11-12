import 'dart:io';

/// Asks Windows to restart this PC the ordinary way: `shutdown /r /t 0`
/// without /f, so programs may still ask to save or refuse. True when Windows
/// accepted the request, which is not the restart itself.
Future<bool> requestPcRestart() async {
  // Never from a test run: the PC running the tests may be a live desktop.
  if (Platform.environment.containsKey('FLUTTER_TEST')) {
    throw StateError('requestPcRestart is not available in tests');
  }
  if (!Platform.isWindows) return false;
  final root = Platform.environment['SystemRoot'] ?? r'C:\Windows';
  try {
    final result = await Process.run('$root\\System32\\shutdown.exe', [
      '/r',
      '/t',
      '0',
    ]);
    return result.exitCode == 0;
  } catch (_) {
    return false;
  }
}
