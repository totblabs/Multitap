import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import '../pc_login_replacement_lifetime_test.dart' show AuthWatch;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm, mainButton;
import 'android_updates_test.dart' show FakePlatform, FakeTransport;
import 'dart:async';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'android_takeover_context_test.dart' show androidOffer;













void originalRegressionCases() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final reason in [
    'pc_login_replaced',
    'session_idle_timeout',
    'session_reauthentication_required',
  ]) {
    for (final openedRemote in [false, true]) {
      testWidgets(
        '$reason ${openedRemote ? 'real tile-open' : 'no-open control'} allows Connect after ACK and new manual login',
        (tester) async {
          tester.view.physicalSize = const Size(1080, 2400);
          tester.view.devicePixelRatio = 3;
          addTearDown(tester.view.reset);
          final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
          const pen = MethodChannel('multitap/pen-input');
          messenger.setMockMethodCallHandler(pen, (_) async => null);
          messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel,
            (call) async => call.method == 'start' ? true : null);
          addTearDown(() {
            for (final channel in [pen, AndroidSessionKeepalive.channel, AndroidStore.channel]) {
              messenger.setMockMethodCallHandler(channel, null);
            }
          });
          final broker = FakeBroker([vm(ownedOutput: true)]);
          final watches = <AuthWatch>[];
          MemoryStore(origin: 'https://vdi.example', username: 'tester',
            prefs: {'launcher.view': 'cards'}).install();
          await tester.pumpWidget(AndroidVdiApp(
            apiFactory: (_) => broker,
            desktopWatchFactory: (_, _) { final watch = AuthWatch(); watches.add(watch); return watch; },
            updatesFactory: (origin) => AndroidUpdates(origin, transport: FakeTransport({})),
            updatePlatform: FakePlatform(),
          ));
          await tester.pumpAndSettle();
          Future<void> manualLogin() async {
            await tester.enterText(find.byKey(const Key('sign-in-password')), 'synthetic-only');
            await tester.ensureVisible(find.byKey(const Key('login-submit')));
            await tester.tap(find.byKey(const Key('login-submit')));
            await tester.pumpAndSettle();
          }
          await manualLogin();
          expect(watches, hasLength(1));
          expect(tester.widget<FilledButton>(mainButton('pve-149')).onPressed, isNotNull);
          if (openedRemote) {
            await tester.ensureVisible(mainButton('pve-149'));
            await tester.tap(mainButton('pve-149'));
            await acceptStreamConnect(tester, 'pve-149');
            await tester.pumpAndSettle();
            expect(find.byType(AndroidRemoteView), findsOneWidget,
              reason: 'Production AndroidHome._open owns the route, not a synthetic Navigator push.');
            expect(broker.calls.where((v) => v == 'connect'), hasLength(1));
          }
          // The real desktop-watch event retires the production authentication owner.
          watches.first.terminal.add(reason);
          await tester.pumpAndSettle();
          expect(find.byType(AndroidRemoteView), findsNothing);
          final ack = find.byKey(const ValueKey('session-end-ack'));
          expect(ack, findsOneWidget);
          expect(tester.widget<FilledButton>(ack).onPressed, isNotNull,
            reason: 'This fixture isolates the Android ownership bug with verified route teardown.');
          await tester.tap(ack);
          await tester.pumpAndSettle();
          await manualLogin();
          expect(watches, hasLength(2));
          expect(broker.calls.where((v) => v == 'login:tester'), hasLength(2));
          final primary = tester.widget<FilledButton>(mainButton('pve-149'));
          // Retain this desired assertion unchanged across the original RED and future fix.
          expect(primary.onPressed, isNotNull,
            reason: 'A fresh manually authenticated owner with the same ready inventory must regain Connect.');
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pumpAndSettle();
        },
      );
    }
  }
}


















class OccupiedBroker extends FakeBroker implements ConfirmedTakeoverBrokerApi {
  OccupiedBroker(super.inventory);
  int confirmations = 0;
  final held = <Completer<BrokerConnection>>[];
  bool holdConnections = false;
  @override
  Future<BrokerConnection> connect(String token, String desktopId,
      {String mode = 'stream', StreamOptions? options}) {
    calls.add('connect');
    if (holdConnections) {
      final pending = Completer<BrokerConnection>(); held.add(pending); return pending.future;
    }
    return Future.error(BrokerException('desktop_busy', 409, null, true, androidOffer()));
  }
  @override
  Future<bool> confirmTakeover(String token, String desktopId, StreamTakeoverOffer offer) async {
    confirmations++; return true;
  }
}

BrokerDesktop occupied({bool cleanup = false}) => BrokerDesktop(
  id: 'pve-149', name: 'TESTER', kind: 'vm', description: '', os: 'Windows11',
  status: 'online', canConnect: true, canStream: !cleanup, hostConnected: true,
  use: DesktopUse.mine, monitors: const [RemoteMonitor(id:'0',width:1920,height:1080)],
  display: VirtualDisplayState(supported:true,activeCount:1,stale:false,checkedAt:DateTime.now().toUtc()),
  power: const BrokerPowerSnapshot(state:'running',stale:false),
  sessionCleanup: cleanup ? SessionCleanupState(pending:true,phase:'pending',checkedAt:DateTime.now().toUtc()) : const SessionCleanupState(),
);

class AppHarness {
  AppHarness(this.broker);
  final OccupiedBroker broker;
  final watches = <AuthWatch>[];
  Future<void> mount(WidgetTester t) async {
    t.view.physicalSize=const Size(1080,2400);t.view.devicePixelRatio=3;addTearDown(t.view.reset);
    final messenger=TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const pen=MethodChannel('multitap/pen-input');
    messenger.setMockMethodCallHandler(pen, (_) async=>null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel,(call)async=>call.method=='start'?true:null);
    addTearDown((){for(final channel in [pen,AndroidSessionKeepalive.channel,AndroidStore.channel]){messenger.setMockMethodCallHandler(channel,null);}});
    MemoryStore(origin:'https://vdi.example',username:'tester',prefs:{'launcher.view':'cards'}).install();
    await t.pumpWidget(AndroidVdiApp(apiFactory:(_)=>broker,
      desktopWatchFactory:(_,_) {final watch=AuthWatch();watches.add(watch);return watch;},
      updatesFactory:(origin)=>AndroidUpdates(origin,transport:FakeTransport({})),updatePlatform:FakePlatform()));
    await t.pumpAndSettle();await login(t);
  }
  Future<void> login(WidgetTester t) async {
    await t.enterText(find.byKey(const Key('sign-in-password')),'synthetic-only');
    await t.ensureVisible(find.byKey(const Key('login-submit')));await t.tap(find.byKey(const Key('login-submit')));await t.pumpAndSettle();
  }
  Future<void> tapConnect(WidgetTester t) async {
    expect(t.widget<FilledButton>(mainButton('pve-149')).onPressed,isNotNull);
    await t.ensureVisible(mainButton('pve-149'));await t.tap(mainButton('pve-149'));
    await acceptStreamConnect(t, 'pve-149');
  }
  Future<void> close(WidgetTester t) async {await t.pumpWidget(const SizedBox.shrink());await t.pumpAndSettle();}
}

void addonCases(){
  TestWidgetsFlutterBinding.ensureInitialized();
  for(final cleanup in [false,true]){
    testWidgets('fresh Android login existing PC controller cleanup=$cleanup remains actionable and asks exact takeover', (t)async{
      final broker=OccupiedBroker([occupied(cleanup:cleanup)]);
      final harness=AppHarness(broker);await harness.mount(t);await harness.tapConnect(t);
      if(cleanup){
        await t.pump();await t.pump(const Duration(milliseconds:200));
        expect(broker.calls.where((v)=>v=='connect'),isEmpty,reason:'Real Host cleanup is never bypassed.');
        broker.inventory=[occupied()];harness.watches.last.changesController.add(null);
      }
      await t.pumpAndSettle();
      expect(find.byType(AndroidRemoteView),findsOneWidget);
      expect(broker.calls.where((v)=>v=='connect'),hasLength(1));
      expect(find.byKey(const Key('android-takeover')),findsOneWidget);
      // The card is the one question: it names the connection its button ends and sends nothing by itself.
      expect(find.textContaining('연결 중인 계정 alice'),findsOneWidget);expect(find.byType(Dialog),findsNothing);
      await t.pump(const Duration(seconds:2));expect(broker.confirmations,0);
      // Only its press asks the broker, once, with no dialog between.
      await t.tap(find.byKey(const Key('android-takeover')));await t.pump();
      expect(find.byType(Dialog),findsNothing);expect(broker.confirmations,1);
      await t.pumpAndSettle();expect(broker.confirmations,1);await harness.close(t);
    });
  }
  testWidgets('late retired connect cannot replace the new owner or clear its opening route',(t)async{
    final broker=OccupiedBroker([vm(ownedOutput:true)])..holdConnections=true;
    final app=AppHarness(broker);await app.mount(t);await app.tapConnect(t);await t.pump();await t.pump(const Duration(milliseconds:200));
    expect(broker.held,hasLength(1));
    app.watches.first.terminal.add('pc_login_replaced');await t.pumpAndSettle();
    await t.tap(find.byKey(const ValueKey('session-end-ack')));await t.pumpAndSettle();await app.login(t);
    // The stream flag must also retire: a new owner's live inventory event is observed.
    final reads=broker.calls.where((v)=>v=='desktops').length;
    app.watches.last.changesController.add(null);await t.pumpAndSettle();
    expect(broker.calls.where((v)=>v=='desktops').length,greaterThan(reads));
    await app.tapConnect(t);await t.pump();await t.pump(const Duration(milliseconds:200));expect(broker.held,hasLength(2));
    final newView=t.widget<AndroidRemoteView>(find.byType(AndroidRemoteView));
    broker.held.first.completeError(const BrokerException('network_error'));await t.pump();await t.pump(const Duration(milliseconds:200));
    expect(t.widget<AndroidRemoteView>(find.byType(AndroidRemoteView)),same(newView));
    // Launcher remains owned by the currently opening route, even after the old RPC finishes.
    expect(t.widget<FilledButton>(find.byKey(const Key('open-pve-149'),skipOffstage:false)).onPressed,isNull);
    broker.held.last.completeError(const BrokerException('network_error'));await t.pumpAndSettle();
    final route=ModalRoute.of(t.element(find.byType(AndroidRemoteView)))!;
    Navigator.of(t.element(find.byType(AndroidRemoteView))).removeRoute(route);await t.pumpAndSettle();
    expect(t.widget<FilledButton>(mainButton('pve-149')).onPressed,isNotNull);
    await app.close(t);
  });
}

void main() { originalRegressionCases(); addonCases(); }
