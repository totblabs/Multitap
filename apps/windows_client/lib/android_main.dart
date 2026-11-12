import 'dart:io';

import 'package:flutter/material.dart';

import 'android/android_app.dart';
import 'data/product_log.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await ProductLog.instance.initialize(
    Directory('${Directory.systemTemp.path}/multitap-logs'),
  );
  ProductLog.instance.record(LogEvent.appStarted);
  runApp(const AndroidVdiApp());
}
