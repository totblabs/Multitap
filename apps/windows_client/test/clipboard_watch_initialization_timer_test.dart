// PRIVATE candidate tests. Fake time/metadata/sockets only, no OS clipboard.
import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'clipboard_session_test.dart' show ClipboardSocket, ready, sessionId;

const _backoffs = [Duration(milliseconds: 100), Duration(milliseconds: 250), Duration(milliseconds: 500)];
const _channel = MethodChannel('multitap/clipboard');
Future<void> _turns(WidgetTester t) async { for (var n=0;n<14;n++) { await t.pump(); } }
Future<void> _progress(WidgetTester t, Future<void> work) async {
  var done=false; Object? failure;
  work.then<void>((_) {done=true;},onError:(Object error) {failure=error;done=true;});
  for(var n=0;n<320&&!done;n++) {await t.pump(n<16?Duration.zero:const Duration(milliseconds:10));if(n%8==0) await t.runAsync(()=>Future<void>.delayed(Duration.zero));}
  expect(done,isTrue,reason:'bounded exact fixture Future completed');expect(failure,isNull);
}
class _Metadata implements ClipboardObservationBridge, ClipboardWatchOwnership {
  final events=StreamController<ClipboardChange>.broadcast(sync:true);
  int enabled=0, failures=0, sequence=17;
  bool own=true;
  @override Stream<ClipboardChange> get changes=>events.stream;
  @override int? get lastCommittedSequence=>null;
  @override Future<int> watch(String owner,bool active) async {
    if(active){enabled++;if(failures!=0){if(failures>0)failures--;throw PlatformException(code:'clipboard_not_active');}}
    return sequence;
  }
  @override Future<({int sequence,bool own})> watchOwned(String owner) async => (sequence:await watch(owner,true),own:own);
  void changed(int value){sequence=value;events.add(ClipboardChange('timer-owner',value));}
}
class _Unit {
  _Unit({bool recovery=true,this.armFailure=false,Completer<void>? idle}) {
    sync=ClipboardSync(owner:'timer-owner',bridge:bridge,policy:const ClipboardPolicy.all(),
      initializationBackoffs:recovery?_backoffs:const [],
      sendLocal:()async{sent.add(bridge.sequence);sync.capturedLocal(bridge.sequence);sync.confirmedLocal(bridge.sequence);},
      receiveRemote:()async{received++;},watchRemote:remote.add,waitIdle:()async{},
      armRemote:()async{arms++;if(armFailure)throw PlatformException(code:'clipboard_busy');},
      retryRemoteInitialization:()async{retryEntered=true;await idle?.future;arms++;return true;},
      onInitializationRecovered:(_){recovered++;},
      onFailure:(){failures++;unawaited(sync.setActive(true));});
  }
  final bridge=_Metadata();late final ClipboardSync sync;
  bool armFailure,retryEntered=false;
  int arms=0,failures=0,recovered=0,received=0;
  final sent=<int>[],remote=<bool>[];
  Future<void> close(WidgetTester t)async{await _progress(t,sync.close());await _progress(t,bridge.events.close());}
}
class _SessionNative {
  int watches=0,captures=0,armCalls=0,writes=0;
  bool failCapture=false;int failures=1;
  Future<Object?> call(MethodCall call)async{
    final args=Map<String,Object?>.from(call.arguments as Map? ?? {});
    switch(call.method){
      case 'watch':if(args['enabled']==true){watches++;if(failures!=0){if(failures>0)failures--;throw PlatformException(code:'clipboard_not_active');}}return {'sequence':17,'own':false};
      case 'observePendingSequence':armCalls++;return 17;
      case 'capture':captures++;if(failCapture)throw PlatformException(code:'clipboard_write_failed');return {'own':true,'sequence':17};
      case 'cancelPendingOwner':case 'cancelPending':case 'cancel':case 'release':case 'pullShutdown':case 'pullClose':case 'closeOffers':return null;
      default:writes++;throw PlatformException(code:'fixture_unexpected_native_call');
    }
  }
}
Map<String,dynamic> _ready(bool intent)=>{
  ...ready,'clipboardSync':true,
  if(intent)...{'clipboardSettled':clipboardSettledProtocol,'clipboardIntent':clipboardIntentProtocol,
    'limits':{'chunkBytes':46080,'maxFiles':200000,'maxFileBytes':9007199254740991,'maxTotalBytes':9007199254740991,
      'maxTextUnits':64*1024*1024,'maxTextBytes':64*1024*1024,'limitsProfile':2,'formatsProfile':2,
      'maxEntries':200000,'maxDepth':512,'maxPathChars':1024,'maxRichBytes':64*1024*1024,
      'maxImageBytes':256*1024*1024,'maxImagePixels':64000000,'windowBytes':1024*1024,'windowUnits':32,
      'pullProfile':1,'maxOffers':2,'scaleProfile':1,'onPasteProfile':1,'pushTextBytes':1024*1024,'pushImageBytes':8*1024*1024}}
};
class _Session {
  _Session({bool android=false,this.intent=false}){
    messenger.setMockMethodCallHandler(_channel,native.call);
    session=ClipboardSession(url:Uri.parse('wss://fixture.invalid/api/v1/sessions/$sessionId/clipboard'),token:'fixture-only',
      sessionId:sessionId,policy:const ClipboardPolicy.all(),syncSupported:true,
      bridge:NativeClipboardBridge(android:android,allowedText:true,allowedFiles:true),dialer:(_,_)async=>socket);
    session.addListener(()=>unawaited(session.setSharingActive(listenerActive)));
  }
  final bool intent;
  bool listenerActive=true;
  final messenger=TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final native=_SessionNative(),socket=ClipboardSocket();late final ClipboardSession session;
  Future<void> start(WidgetTester t)async{final starting=session.start();await _turns(t);socket.emit(_ready(intent));await _progress(t,starting);await _progress(t,session.setSharingActive(true));await _turns(t);}
  Future<void> close(WidgetTester t)async{await _progress(t,session.close());messenger.setMockMethodCallHandler(_channel,null);}
}
void main(){
 testWidgets('initialization has exactly three backoffs and listener notifications cannot restart the budget',(t)async{
  final f=_Unit()..bridge.failures=-1;
  try{await f.sync.setActive(true);expect(f.bridge.enabled,1);expect(f.failures,1);
   for(final wait in [100,250,500]){final before=f.bridge.enabled;await t.pump(Duration(milliseconds:wait-1));expect(f.bridge.enabled,before);await t.pump(const Duration(milliseconds:1));await _turns(t);expect(f.bridge.enabled,before+1);}
   expect(f.bridge.enabled,4);expect(f.failures,4);await t.pump(const Duration(seconds:5));await f.sync.setActive(true);await f.sync.retryInitialization();await _turns(t);
   expect(f.bridge.enabled,4);expect(f.sent,isEmpty);expect(f.received,0);
  }finally{await f.close(t);}
 });
 testWidgets('retry baseline sends no old contents but a later authorized copy still transfers',(t)async{
  final f=_Unit()..bridge.failures=1..bridge.own=false;
  try{await f.sync.setActive(true);await t.pump(const Duration(milliseconds:100));await _turns(t);
   expect(f.bridge.enabled,2);expect(f.recovered,1);expect(f.sent,isEmpty);expect(f.received,0);
   f.bridge.changed(18);await t.pump(const Duration(milliseconds:80));await _turns(t);expect(f.sent,[18]);
  }finally{await f.close(t);}
 });
 testWidgets('arm-only recovery waits exact existing work and preserves the healthy local watcher',(t)async{
  final idle=Completer<void>();
  final g=_Unit(armFailure:true,idle:idle);
  try{await g.sync.setActive(true);await t.pump(const Duration(milliseconds:100));await _turns(t);
   expect(g.retryEntered,isTrue);expect(g.arms,1);expect(g.bridge.enabled,1);expect(g.remote.where((v)=>v),isEmpty);expect(g.sent,isEmpty);
   idle.complete();await _turns(t);expect(g.arms,2);expect(g.bridge.enabled,1);expect(g.remote.where((v)=>v),[true]);expect(g.sent,isEmpty);
  }finally{if(!idle.isCompleted)idle.complete();await g.close(t);}
 });
 testWidgets('new copy after baseline is not dropped while remote initialization is held',(t)async{
  final gate=Completer<void>();
  final g=_Unit(idle:gate)..bridge.failures=1..bridge.own=false;
  try{await g.sync.setActive(true);await t.pump(const Duration(milliseconds:100));await _turns(t);expect(g.retryEntered,isTrue);expect(g.sent,isEmpty);
   g.bridge.changed(18);await t.pump(const Duration(milliseconds:80));await _turns(t);expect(g.sent,[18]);
   gate.complete();await _turns(t);expect(g.bridge.enabled,2);expect(g.sent,[18]);
  }finally{if(!gate.isCompleted)gate.complete();await g.close(t);}
 });
 testWidgets('deactivation cancels scheduled retry and late held recovery cannot revive a closed owner',(t)async{
  final f=_Unit()..bridge.failures=-1;
  try{await f.sync.setActive(true);await f.sync.setActive(false);await t.pump(const Duration(seconds:2));expect(f.bridge.enabled,1);expect(f.remote.where((v)=>v),isEmpty);
  }finally{await f.close(t);}
  final gate=Completer<void>();
  final h=_Unit(armFailure:true,idle:gate);
  await h.sync.setActive(true);await t.pump(const Duration(milliseconds:100));await _turns(t);expect(h.retryEntered,isTrue);
  final closing=h.sync.close();gate.complete();await _progress(t,closing);expect(h.recovered,0);expect(h.remote.where((v)=>v),isEmpty);expect(h.sent,isEmpty);await _progress(t,h.bridge.events.close());
 });
 testWidgets('default recovery remains off for fake and legacy ClipboardSync callers',(t)async{
  final f=_Unit(recovery:false)..bridge.failures=-1;
  try{await f.sync.setActive(true);await t.pump(const Duration(seconds:3));await f.sync.retryInitialization();expect(f.bridge.enabled,1);expect(f.failures,1);expect(f.sent,isEmpty);
  }finally{await f.close(t);}
 });
 for(final intent in [false,true]){
  testWidgets('real Windows session retries ${intent?'virgin intent':'legacy'} initialization and clears its exact watch error',(t)async{
   final f=_Session(intent:intent);
   try{await f.start(t);expect(f.native.watches,1);expect(f.session.error,'clipboard_watch_unavailable');
    await t.pump(const Duration(milliseconds:99));expect(f.native.watches,1);await t.pump(const Duration(milliseconds:1));await _turns(t);
    expect(f.native.watches,2);expect(f.native.captures,0);expect(f.native.writes,0);expect(f.session.error,isNull);
    expect(f.socket.sent.where((v)=>v['type']=='clipboard.watch'&&v['enabled']==true),hasLength(1));if(intent)expect(f.native.armCalls,1);
   }finally{await f.close(t);}
  },variant:TargetPlatformVariant.only(TargetPlatform.windows),skip:!Platform.isWindows);
 }
 testWidgets('never-armed intent after focus grace binds its current revision and recovers safely',(t)async{
  final f=_Session(intent:true)..native.failures=-1;
  try{await f.start(t);expect(f.native.watches,1);expect(f.native.armCalls,0);
   f.listenerActive=false;await _progress(t,f.session.setSharingActive(false));await t.pump(const Duration(seconds:2));await _turns(t);
   expect(f.native.armCalls,0);f.native.failures=1;f.listenerActive=true;
   await _progress(t,f.session.setSharingActive(true));await _turns(t);expect(f.native.watches,2);expect(f.native.armCalls,0);
   await t.pump(const Duration(milliseconds:100));await _turns(t);expect(f.native.watches,3);expect(f.native.armCalls,1);
   expect(f.native.captures,0);expect(f.native.writes,0);expect(f.session.error,isNull);
  }finally{await f.close(t);}
 },variant:TargetPlatformVariant.only(TargetPlatform.windows),skip:!Platform.isWindows);
 testWidgets('successful watch recovery does not erase a later actual transfer error',(t)async{
  final f=_Session();
  try{await f.start(t);f.native.failCapture=true;
   await expectLater(f.session.sendLocal(),throwsA(isA<ClipboardTransferException>().having((e)=>e.code,'code','clipboard_write_failed')));
   expect(f.session.error,'clipboard_write_failed');await t.pump(const Duration(milliseconds:100));await _turns(t);
   expect(f.native.watches,2);expect(f.session.error,'clipboard_write_failed');expect(f.native.writes,0);
  }finally{await f.close(t);}
 },variant:TargetPlatformVariant.only(TargetPlatform.windows),skip:!Platform.isWindows);
 testWidgets('real Android adapter keeps initialization retries off and preserves its prior behavior',(t)async{
  final f=_Session(android:true);
  try{await f.start(t);await t.pump(const Duration(seconds:2));await f.session.retrySharingInitialization();await _turns(t);
   expect(f.native.watches,1);expect(f.session.error,'clipboard_watch_unavailable');expect(f.native.captures,0);expect(f.native.writes,0);
  }finally{await f.close(t);}
 },variant:TargetPlatformVariant.only(TargetPlatform.android));
 testWidgets('successful normal Windows reactivation clears only its owned watch warning after focus grace',(t)async{
  final f=_Session(intent:true)..native.failures=-1;
  try{await f.start(t);expect(f.session.error,'clipboard_watch_unavailable');expect(f.native.watches,1);
   f.listenerActive=false;await _progress(t,f.session.setSharingActive(false));await t.pump(const Duration(seconds:2));await _turns(t);
   f.native.failures=0;f.listenerActive=true;
   // Current own contents stay a metadata baseline; no body/native source read.
   f.messenger.setMockMethodCallHandler(_channel,(call)async{
    final value=await f.native.call(call);
    return call.method=='watch'&&value is Map?{...value,'own':true}:value;
   });
   await _progress(t,f.session.setSharingActive(true));await _turns(t);
   expect(f.native.watches,2);expect(f.native.armCalls,1);expect(f.session.error,isNull);
   expect(f.native.captures,0);expect(f.native.writes,0);
   expect(f.socket.sent.where((v)=>v['type']=='clipboard.watch'&&v['enabled']==true),hasLength(1));
  }finally{await f.close(t);}
 },variant:TargetPlatformVariant.only(TargetPlatform.windows),skip:!Platform.isWindows);
}
