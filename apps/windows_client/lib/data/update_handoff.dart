import 'broker_transport.dart';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'product_updates.dart';

/// What the installer that relaunched this app did (update-handoff-v1). The
/// native side reads and consumes its record once, and only for the version
/// the installer itself opened: the new one after an update, the previous
/// one after a failure. Fixed versions, codes and flags only.
class UpdateHandoff {
  const UpdateHandoff({
    required this.product,
    required this.updated,
    required this.fromVersion,
    required this.toVersion,
    this.code = '',
    this.stage = 0,
    this.attention = false,
    this.restartRequired = false,
  });

  /// 'client' or 'host'.
  final String product;

  /// True: [toVersion] is running. False: the update failed and the
  /// previous [fromVersion] was reopened.
  final bool updated;
  final String fromVersion, toVersion;

  /// A fixed failure code from [updateFailureCodes]; empty after success.
  final String code;

  /// Installer steps completed (0-6) when it stopped.
  final int stage;

  /// Host only: the display needs a look, or Windows asked for a restart.
  final bool attention, restartRequired;

  static final _version = RegExp(r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$');

  static UpdateHandoff? parse(Object? value) {
    if (value is! Map) return null;
    final result = value['result'], product = value['product'];
    final from = value['fromVersion'], to = value['toVersion'];
    final code = value['code'] ?? '', stage = value['stage'] ?? 0;
    final attention = value['attention'] ?? false;
    final restart = value['restartRequired'] ?? false;
    if ((result != 'updated' && result != 'failed') ||
        (product != 'client' && product != 'host') ||
        from is! String ||
        to is! String ||
        !_version.hasMatch(from) ||
        !_version.hasMatch(to) ||
        code is! String ||
        (code.isNotEmpty && !updateFailureCodes.contains(code)) ||
        stage is! int ||
        stage < 0 ||
        stage > 6 ||
        attention is! bool ||
        restart is! bool) {
      return null;
    }
    final updated = result == 'updated';
    return UpdateHandoff(
      product: product as String,
      updated: updated,
      fromVersion: from,
      toVersion: to,
      code: updated ? '' : (code.isEmpty ? 'install_failed' : code),
      stage: stage,
      attention: updated && attention,
      restartRequired: updated && restart,
    );
  }

  static Future<UpdateHandoff?> take({
    MethodChannel channel = const MethodChannel('multitap/window'),
  }) async {
    try {
      return parse(await channel.invokeMethod<Object?>('takeUpdateHandoff'));
    } on MissingPluginException {
      return null;
    } on PlatformException {
      return null;
    }
  }
}

typedef UpdateHistoryOpener = Future<void> Function({
  required String server,
  required String product,
  required String currentVersion,
});
typedef UpdatePanelOpener = Future<void> Function({
  required String server,
  required String product,
  required String currentVersion,
  required bool manual,
});

/// The one non-blocking notice after an update relaunch, and the configured
/// origin and running version its actions may use. The origin always comes
/// from this app's own settings, never from the handoff record.
class UpdateNotices extends ChangeNotifier {
  static final instance = UpdateNotices();

  UpdateHandoff? _current;
  UpdateHandoff? get current => _current;
  String? _server, _version;

  /// The configured HTTPS origin and the running version of [product].
  String? get server => _server;
  String? get currentVersion => _version;

  /// Set by the shell that opens dialogs (the toast lives above routes).
  UpdatePanelOpener? openPanel;
  UpdateHistoryOpener? openHistory;

  void configure({String? server, String? currentVersion}) {
    String? origin;
    try {
      if (server != null) {
        final parsed = normalizeBrokerOrigin(server);
        if (brokerOriginAllowed(parsed)) origin = parsed.origin;
      }
    } on FormatException {
      /* Invalid notices cannot authorize a server. */
    }
    final version =
        currentVersion != null &&
            RegExp(r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$')
                .hasMatch(currentVersion)
        ? currentVersion
        : null;
    if (origin == _server && version == _version) return;
    _server = origin;
    _version = version;
    notifyListeners();
  }

  void show(UpdateHandoff notice) {
    _current = notice;
    notifyListeners();
  }

  void dismiss() {
    if (_current == null) return;
    _current = null;
    notifyListeners();
  }
}
