import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/ui/product_log_dialog.dart';

void main(){
  test('preparation attribution stays memory-only and cannot alter forwarded wire records', () async {
    final forwarded = <ProductLogEntry>[], local = <ProductLogEntry>[];
    final log = ProductLog(recordSink: forwarded.add);
    final subscription = log.localRecords.listen(local.add);
    addTearDown(() async {
      await subscription.cancel();
      log.dispose();
    });
    log.recordFailure(LogEvent.streamEnd, 'capture_failed', {'ok': false});
    log.recordPreparationFailure(LogEvent.streamEnd, 'display_control_unavailable', {
      'ok': false,
      'stage': 3,
      'token': 'PRIVATE_SENTINEL',
    });
    await log.flush();
    expect(local, hasLength(2));
    expect(forwarded, hasLength(2));
    expect(local.first.inheritSession, isTrue);
    expect(local.last.inheritSession, isFalse);
    expect(forwarded.last.inheritSession, isFalse);
    final wire = forwarded.last.toJson();
    expect(wire.keys.toSet(), {'time', 'event', 'values', 'code'});
    expect(wire['values'], {'ok': false, 'stage': 3});
    expect(jsonEncode(wire), isNot(contains('PRIVATE_SENTINEL')));
    final parsed = ProductLogEntry.parse({...wire, 'inheritSession': false});
    expect(parsed, isNotNull);
    expect(parsed!.inheritSession, isTrue, reason: 'An untrusted wire field cannot set internal attribution.');
    expect(log.acceptForwarded(wire), isTrue);
    await log.flush();
    expect(local, hasLength(2), reason: 'Received records never re-enter diagnostics.');
    expect(forwarded, hasLength(2), reason: 'A forwarding logger cannot echo received records.');
  });

  test('journal drops secret text, unknown fields, nonfinite values and malformed events',(){
    final log=ProductLog();
    log.record(LogEvent.displayObserved,{'reboot':true,'monitors':2,'password':'PRIVATE_SENTINEL','token':'PRIVATE_SENTINEL','error':'PRIVATE_SENTINEL','width':double.infinity});
    final encoded=jsonEncode(log.entries.single.toJson());
    expect(encoded, isNot(contains('PRIVATE_SENTINEL')));
    expect(log.entries.single.values,{'reboot':true,'monitors':2});
    expect(ProductLogEntry.parse({'time':'2026-10-23','event':'PRIVATE_SENTINEL','values':{}}),isNull);
    for(var i=0;i<5000;i++){log.record(LogEvent.viewportResult,{'ok':false,'width':i});}
    expect(log.entries.length,ProductLog.maximumEntries);
    log.dispose();
  });
  test('disk history survives a new logger and remains bounded',()async{
    final parent=Directory('${Directory.current.path}${Platform.pathSeparator}..${Platform.pathSeparator}..${Platform.pathSeparator}.artifacts${Platform.pathSeparator}product-log-qa');
    await parent.create(recursive:true);
    final directory=await parent.createTemp('owned-');
    final log=ProductLog();await log.initialize(directory);
    for(var i=0;i<900;i++){log.record(LogEvent.displayObserved,{'ok':false,'reboot':i.isOdd,'monitors':2});}
    await log.flush();expect(log.storageAvailable,isTrue);
    final files=await directory.list().where((e)=>e is File).toList();
    expect(files.length,lessThanOrEqualTo(ProductLog.maximumFiles));
    for(final file in files.cast<File>()){expect(await file.length(),lessThanOrEqualTo(ProductLog.maximumBytes));}
    final restored=ProductLog();await restored.initialize(directory);
    expect(restored.entries.length,ProductLog.maximumEntries);
    expect(restored.entries.last.values['reboot'],isTrue);
    log.dispose();restored.dispose();
    // Preserve the bounded, synthetic QA history as an artifact.
  });
  testWidgets('log viewer displays host restart evidence and filters attention', (tester)async{
    await tester.pumpWidget(MaterialApp(home:Builder(builder:(context)=>TextButton(
      onPressed:()=>showProductLog(context,loadHost:()async=>[
        ProductLogEntry(DateTime.utc(2026,10,23),LogEvent.displayObserved,{'reboot':true,'monitors':0}),
      ]),child:const Text('열기')))));
    await tester.tap(find.text('열기'));await tester.pumpAndSettle();
    expect(find.text('로그 보기'),findsOneWidget);
    expect(find.textContaining('다시 시작 필요'),findsOneWidget);
    expect(find.textContaining(': 예'),findsNothing);
    await tester.tap(find.text('확인 필요 1'));await tester.pumpAndSettle();
    expect(tester.takeException(),isNull);
  });

  testWidgets('Clipboard records read as one sentence and a broken channel is marked', (tester)async{
    final at=DateTime.utc(2026,10,26,4);
    await tester.pumpWidget(MaterialApp(home:Builder(builder:(context)=>TextButton(
      onPressed:()=>showProductLog(context,loadHost:()async=>[
        ProductLogEntry(at,LogEvent.clipboard,const {'action':1,'ok':true,'auto':true,'available':true}),
        ProductLogEntry(at.add(const Duration(seconds:1)),LogEvent.clipboard,const {'action':2,'ok':true,'remote':false,'auto':true}),
        ProductLogEntry(at.add(const Duration(seconds:2)),LogEvent.clipboard,const {'action':3,'ok':false},code:'clipboard_transport_closed'),
        // An ok record's code is information, not an error (tuning #7).
        ProductLogEntry(at.add(const Duration(seconds:3)),LogEvent.clipboard,const {'action':3,'ok':true},code:'session_ended'),
      ]),child:const Text('열기')))));
    await tester.tap(find.text('열기'));await tester.pumpAndSettle();
    expect(find.textContaining('클립보드 연결됨 · 복사·붙여넣기 공유'),findsOneWidget);
    expect(find.textContaining('클립보드 데스크톱 → PC 전송'),findsOneWidget);
    expect(find.textContaining('클립보드 연결 끊김'),findsOneWidget);
    expect(find.textContaining('오류 코드 clipboard_transport_closed'),findsOneWidget);
    expect(find.textContaining('오류 코드 session_ended'),findsNothing);
    expect(find.textContaining('코드 session_ended'),findsOneWidget);
    expect(find.text('확인 필요 1'),findsOneWidget);
    expect(ProductLogEntry.safeValues(const {'auto':true,'action':2}),{'action':2,'auto':true});
  });

  testWidgets('Only failures and warnings are marked, and update records read as one sentence with versions', (tester)async{
    final at=DateTime.utc(2026,10,26,3);
    await tester.pumpWidget(MaterialApp(home:Builder(builder:(context)=>TextButton(
      onPressed:()=>showProductLog(context,loadHost:()async=>[
        // A pre-0.4.72 check row is folded into its result.
        ProductLogEntry(at,LogEvent.updateCheck,const {}),
        ProductLogEntry(at.add(const Duration(seconds:1)),LogEvent.updateResult,{'ok':true,'available':false,'current':logVersion('0.4.71')!,'latest':logVersion('0.4.71')!}),
        ProductLogEntry(at.add(const Duration(seconds:2)),LogEvent.updateResult,{'ok':true,'available':true,'current':logVersion('0.4.70')!,'latest':logVersion('0.4.72')!}),
        ProductLogEntry(at.add(const Duration(seconds:3)),LogEvent.updateDownload,{'ok':true,'current':logVersion('0.4.70')!,'latest':logVersion('0.4.72')!}),
        ProductLogEntry(at.add(const Duration(seconds:4)),LogEvent.updateInstall,{'ok':true,'manual':false,'current':logVersion('0.4.70')!,'latest':logVersion('0.4.72')!}),
        ProductLogEntry(at.add(const Duration(seconds:5)),LogEvent.viewportResult,{'ok':false,'width':1920,'height':1080}),
        ProductLogEntry(at.add(const Duration(seconds:6)),LogEvent.displayObserved,{'recovery':true,'monitors':2}),
        ProductLogEntry(at.add(const Duration(seconds:7)),LogEvent.inventory,{'ok':true}),
      ]),child:const Text('열기')))));
    await tester.tap(find.text('열기'));await tester.pumpAndSettle();
    expect(find.textContaining('업데이트 조회'),findsNothing);
    expect(find.textContaining('업데이트 확인 · 최신 버전 (0.4.71)'),findsOneWidget);
    expect(find.textContaining('업데이트 확인 · 새 버전 0.4.72 발견'),findsOneWidget);
    expect(find.textContaining('0.4.72 다운로드 완료'),findsOneWidget);
    expect(find.textContaining('0.4.70 → 0.4.72 업데이트 후 다시 시작'),findsOneWidget);
    expect(find.textContaining('해상도 1920×1080'),findsOneWidget);
    expect(find.textContaining('성공'),findsNothing);
    // Seven rows, one failure and one warning; nothing else carries a mark.
    expect(find.byIcon(Icons.error_outline_rounded),findsOneWidget);
    expect(find.byIcon(Icons.warning_amber_rounded),findsOneWidget);
    expect(find.text('확인 필요 2'),findsOneWidget);
    await tester.tap(find.byKey(const Key('log-attention-filter')));await tester.pumpAndSettle();
    expect(find.textContaining('최신 버전'),findsNothing);
    expect(find.textContaining('해상도 1920×1080'),findsOneWidget);
    expect(tester.takeException(),isNull);
  });
}
