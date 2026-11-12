import 'dart:async';

import 'package:flutter/services.dart';

/// Network changes a remote view reacts to (ToTBDesk TUNING-2.4.0/2.4.5):
/// `lost` when the network a connection was made on is gone (not merely no
/// longer the default), `available` when a validated default network is
/// there to reconnect on.
abstract interface class AndroidNetworkEvents {
  Stream<String> get changes;

  /// The current default network carries the connection just made.
  Future<void> mark();
}

/// The Android engine's watcher (AndroidNetwork.kt); no addresses or names.
class AndroidNetworkWatch implements AndroidNetworkEvents {
  static const _events = EventChannel('multitap/network');
  static const _mark = MethodChannel('multitap/network/mark');

  @override
  Stream<String> get changes => _events
      .receiveBroadcastStream()
      .where((event) => event == 'lost' || event == 'available')
      .cast<String>();

  @override
  Future<void> mark() async {
    try {
      await _mark.invokeMethod<bool>('mark');
    } catch (_) {
      // Without it the session's own timeouts still notice a drop.
    }
  }
}
