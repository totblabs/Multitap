import 'dart:io';

import 'package:flutter/services.dart';

import 'broker_transport.dart';
import 'product_log.dart';

/// The step of saving a tested server address that failed (`stage` of a
/// serverSave record).
enum ServerSaveStage {
  /// The owner's save: the settings file or store, and the app taking the
  /// new address.
  save(1),

  /// After that save returned: the form keeping the address's HTTP approval.
  keep(2);

  const ServerSaveStage(this.number);
  final int number;
}

/// The kind of failure as a fixed number (`error` of a serverSave record).
/// The log keeps numbers only: never the address or the exception's text.
///
/// 1 the settings file could not be written (`osError` is the system's code)
/// 2 the address has no HTTP approval in this run
/// 3 the HTTP address has no passed connection test
/// 4 the address or the stored settings were refused for another reason
/// 5 a sign-in is running or signed in: the server cannot change now
/// 6 no connection to the new address could be prepared
/// 7 the platform's settings store failed
/// 9 anything else
int serverSaveFailureNumber(Object error) => switch (error) {
  FileSystemException() => 1,
  BrokerTransportRefusal(code: 'http_approval_missing') => 2,
  BrokerTransportRefusal(code: 'http_unverified') => 3,
  FormatException() => 4,
  StateError() => 5,
  ArgumentError() => 6,
  PlatformException() || MissingPluginException() => 7,
  _ => 9,
};

/// Leaves the reason a tested server address was not saved in the product
/// log, for the form that only says "could not save".
void recordServerSaveFailure(
  Object error,
  ServerSaveStage stage, {
  ProductLog? log,
}) {
  final osError = error is FileSystemException
      ? error.osError?.errorCode
      : null;
  (log ?? ProductLog.instance).record(LogEvent.serverSave, {
    'ok': false,
    'stage': stage.number,
    'error': serverSaveFailureNumber(error),
    'osError': ?osError,
  });
}
