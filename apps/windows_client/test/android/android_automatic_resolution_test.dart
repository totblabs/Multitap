import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter/material.dart' show MaterialApp;
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/data/stream_size.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import '../remote_session_test.dart' show SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;
import 'android_large_desktop_test.dart' show openLargeDesktopStream, openLargeDesktopScreenPanel, closeLargeDesktopStreamView;

const limits = DecoderLimits(maxWidth:3840,maxHeight:3840,maxPixels:8294400,maxPixelRate:0);
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    final m=TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    m.setMockMethodCallHandler(AndroidSessionKeepalive.channel,(call)async=>call.method=='start'?'started':true);
    m.setMockMethodCallHandler(const MethodChannel('multitap/pen-input'),(_)async=>null);
  });
  tearDown(() {
    final m=TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    m.setMockMethodCallHandler(AndroidSessionKeepalive.channel,null);
    m.setMockMethodCallHandler(const MethodChannel('multitap/pen-input'),null);
  });
  metricsCases();
  // Independent golden sizes from the existing readability ratio, not device-class caps.
  final specs = [
    (AndroidDeviceClass.phone,const Size(2340,1080),3.0,null,const Size(1248,576)),
    (AndroidDeviceClass.phone,const Size(2400,1080),2.5,null,const Size(1536,690)),
    (AndroidDeviceClass.fold,const Size(2448,1848),2.625,const AndroidScreenDensity(xdpi:400,ydpi:400,width:2448,height:1848),const Size(1148,866)),
    (AndroidDeviceClass.tablet,const Size(2560,1600),2.0,const AndroidScreenDensity(xdpi:274.3,ydpi:274.3,width:2560,height:1600),const Size(1462,914)),
  ];
  for(final s in specs) {
    for(final portrait in [false,true]) {
      final physical=portrait?Size(s.$2.height,s.$2.width):s.$2;
      final wanted=portrait?Size(s.$5.height,s.$5.width):s.$5;
      test('${s.$1} ${s.$2} portrait=$portrait adaptive ratio requests its own moderate workspace',() {
        final policy=AndroidScreenPolicy(deviceClass:s.$1,density:s.$4,window:Size(physical.width/s.$3,physical.height/s.$3));
        final plan=AndroidViewportPlan.of(physical,s.$3,policy,hostScale:true,portraitLimits:limits);
        expect(plan.size,wanted);expect(plan.pixelScale,0);
        final manualDpi=AndroidViewportPlan.of(physical,s.$3,AndroidScreenPolicy(deviceClass:s.$1,density:s.$4,window:policy.window,choice:const AndroidScreenChoice(scale:175)),hostScale:true,portraitLimits:limits);
        expect(manualDpi.size,wanted);expect(manualDpi.displayScale,175);
      });
      testWidgets('${s.$1} ${s.$2} portrait=$portrait actual Remote auto requests adaptive pixels with initial100',(t)async {
        final h=await openLargeDesktopStream(t,physical:physical,dpr:s.$3,density:s.$4,resolution:AndroidResolution.auto,limits:limits,streamSizeV2:true);
        try {
          expect(h.starts,hasLength(1));
          expect((h.starts.single.maxWidth,h.starts.single.maxHeight),(wanted.width.toInt(),wanted.height.toInt()));
          expect(h.scales.single,100);
        } finally {await closeLargeDesktopStreamView(t);}
      });
    }
    testWidgets('${s.$1} ${s.$2} actual resolution menu uses same metrics as request',(t)async {
      final h=await openLargeDesktopStream(t,physical:s.$2,dpr:s.$3,density:s.$4,resolution:AndroidResolution.auto,limits:limits,streamSizeV2:true);
      try {
        await openLargeDesktopScreenPanel(t);
        final options=t.widget<AndroidScreenOptions>(find.byType(AndroidScreenOptions).last);
        expect(options.resolutions.first.resolution,AndroidResolution.auto);
        expect(options.resolutions.first.size,s.$5);
        expect(h.scales.first,100);
      } finally {await closeLargeDesktopStreamView(t);}
    });
  }
  test('legacy no-DPI phone uses exact integer under adaptive source',(){
    final p=AndroidViewportPlan.of(const Size(2400,1080),2.25,const AndroidScreenPolicy(),hostScale:false);
    expect(p.size,const Size(1200,540));expect(p.pixelScale,2);expect(p.displayScale,isNull);
  });
  test('legacy no-DPI measured tablet uses exact integer under adaptive source',(){
    const p=AndroidScreenPolicy(deviceClass:AndroidDeviceClass.tablet,density:AndroidScreenDensity(xdpi:224,ydpi:224,width:1920,height:1200),window:Size(960,600));
    final plan=AndroidViewportPlan.of(const Size(1920,1200),2,p,hostScale:false);
    expect(plan.size,const Size(960,600));expect(plan.pixelScale,2);expect(plan.displayScale,isNull);
  });
  test('small highDPR source respects protocol minimum; legacy picks nearest valid integer',(){
    const p=AndroidScreenPolicy();
    expect(AndroidViewportPlan.of(const Size(720,1600),4,p,hostScale:true,portraitLimits:limits).size,const Size(320,710));
    final legacy=AndroidViewportPlan.of(const Size(720,1600),4,p,hostScale:false,portraitLimits:limits);
    expect(legacy.size,const Size(360,800));expect(legacy.pixelScale,2);
  });
  test('explicit sizes saved scale and native labels are independent of adaptive auto',(){
    for(final kind in [AndroidDeviceClass.phone,AndroidDeviceClass.fold,AndroidDeviceClass.tablet]){
      for(final edge in AndroidResolution.longEdges){
        final choice=AndroidScreenChoice(scale:175,resolution:AndroidResolution.longEdge(edge));
        expect(AndroidScreenChoice.parse(choice.encode()).forDevice(kind),choice);
        final plan=AndroidViewportPlan.of(const Size(3840,2160),3,AndroidScreenPolicy(deviceClass:kind,choice:choice),hostScale:true,portraitLimits:limits);
        expect(plan.width,edge);expect(plan.height,edge*9~/16~/2*2);expect(plan.displayScale,175);
      }
    }
    final native=androidResolutionOptions(const Size(2400,1080)).last;
    expect(native.resolution,const AndroidResolution.longEdge(2560));expect(native.deviceSize,true);expect(native.label,'기기에 맞춤');
  });
  test('lowDPR auto never upsamples; codec bound remains authoritative',(){
    const p=AndroidScreenPolicy();
    expect(AndroidViewportPlan.of(const Size(640,360),1,p,hostScale:true).size,const Size(640,360));
    final limited=AndroidViewportPlan.of(const Size(3840,2160),3,p,hostScale:true,portraitLimits:const DecoderLimits(maxWidth:1280,maxHeight:720,maxPixels:921600,maxPixelRate:0));
    expect(limited.size,const Size(1280,720));
  });
  testWidgets('actual TV native4K is unchanged at initial100',(t)async{
    final h=await openLargeDesktopStream(t,physical:const Size(3840,2160),television:true,resolution:AndroidResolution.auto,limits:limits,streamSizeV2:true);
    try {expect((h.starts.single.maxWidth,h.starts.single.maxHeight),(3840,2160));expect(h.scales.single,100);}finally{await closeLargeDesktopStreamView(t);}
  });
}

class MetricsStream {
  final reads = <Completer<AndroidScreenDensity?>>[];
  final starts = <StreamOptions>[];
  final scales = <int?>[];
  final saved = <AndroidSessionSettings>[];
  late RemoteSession session;
  late SessionSocket socket;

  Future<void> open(WidgetTester t, {AndroidScreenChoice choice = const AndroidScreenChoice()}) async {
    t.view.physicalSize = const Size(2560,1600);
    t.view.devicePixelRatio = 2;
    addTearDown(t.view.reset);
    AndroidNativeFake().install();
    addTearDown(AndroidNativeFake.uninstall);
    await t.pumpWidget(MaterialApp(
      theme: buildAppTheme(),
      home: AndroidRemoteView(
        name:'Metrics fixture',token:'synthetic-only',options:const StreamOptions(),
        settings:AndroidSessionSettings(helpSeen:true,screens:{for(final k in AndroidDeviceClass.values)k:choice}),
        onSettings:saved.add,displayScale:true,streamSize:true,streamSizeV2:false,
        decoderLimits:()async=>limits,
        density:(){final read=Completer<AndroidScreenDensity?>();reads.add(read);return read.future;},
        load:(options,_,scale)async{
          starts.add(options);scales.add(scale);
          return BrokerConnection(sessionId:'metrics-only',webSocketUrl:Uri.parse('wss://fixture.invalid/metrics'),options:options,
            epochMedia:true,viewportProvisioning:true,displayScale:scale,streamSize:limits,streamSizeV2:false,viewportEncoderLimits:true);
        },
        sessionFactory:(connection,media,stats)=>session=RemoteSession(connection:connection,media:media,token:'synthetic-only',statsSink:stats,dialer:(_,_)async=>socket=SessionSocket()),
      ),
    ));
    for(var i=0;i<5;i++){await t.pump(const Duration(milliseconds:20));}
    expect(starts,hasLength(1));expect(reads,hasLength(1));
    socket.incoming.add(jsonEncode({'type':'stream.info','codec':'h264','width':starts.last.maxWidth,'height':starts.last.maxHeight,
      'sourceWidth':starts.last.maxWidth,'sourceHeight':starts.last.maxHeight}));
    await t.pump(const Duration(milliseconds:20));
    socket.incoming.add(Uint8List.fromList([1,1,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0x65]));
    for(var i=0;i<6;i++){await t.pump(const Duration(milliseconds:40));}
    expect(session.frameReady,true);
  }

  Future<void> close(WidgetTester t) async {
    await closeLargeDesktopStreamView(t);
    for(final read in reads){if(!read.isCompleted)read.complete(null);}
    await t.pump();
  }
}

void metricsCases() {
  testWidgets('latest rotated metrics win over a held old fold-panel reply without reconnect or DPI change',(t)async{
    final h=MetricsStream();await h.open(t);
    try{
      t.view.physicalSize=const Size(1600,2560);
      await t.pump();await t.pump(const Duration(milliseconds:100));
      expect(h.reads,hasLength(2));
      h.reads.last.complete(const AndroidScreenDensity(xdpi:274.3,ydpi:274.3,width:2560,height:1600));
      await t.pump();await t.pump(const Duration(milliseconds:900));await t.pump(const Duration(milliseconds:900));
      final resize=h.socket.sent.where((e)=>e['type']=='stream.resize').toList();
      expect(resize,isNotEmpty);expect((resize.last['width'],resize.last['height']),(914,1462));
      h.reads.first.complete(const AndroidScreenDensity(xdpi:400,ydpi:400,width:2448,height:1848));
      await t.pump();await t.pump(const Duration(milliseconds:900));
      expect(h.socket.sent.where((e)=>e['type']=='stream.resize'),hasLength(resize.length));
      expect(h.starts,hasLength(1));expect(h.scales.single,100);
      // Complete the exact synthetic resize before waiting for a stable menu;
      // a pending real resize correctly keeps its progress animation running.
      final request=resize.last;
      final owner={'sessionId':'metrics-only','requestId':request['requestId'],'resizeId':'metrics-resize','epoch':1};
      h.socket.incoming.add(jsonEncode({'type':'stream.resizing',...owner,'width':914,'height':1462,'fps':request['fps'],'monitor':request['monitor']}));
      await t.pump(const Duration(milliseconds:20));
      // v1 retains the initial encoder grant while changing the desktop source.
      final factor=[1.0,h.starts.single.maxWidth/914,h.starts.single.maxHeight/1462].reduce((a,b)=>a<b?a:b);
      final encodedWidth=(914*factor).floor()~/2*2;
      final encodedHeight=(1462*factor).floor()~/2*2;
      h.socket.incoming.add(jsonEncode({'type':'stream.info',...owner,'codec':'h264','width':encodedWidth,'height':encodedHeight,'sourceWidth':914,'sourceHeight':1462,'fpsTarget':request['fps']}));
      await t.pump(const Duration(milliseconds:20));
      expect(h.session.closed,false,reason:'valid v1 encoder grant: ${h.session.diagnosticCode}');
      h.socket.incoming.add(Uint8List.fromList([1,1,1,0,1,0,0,0,0,0,0,0,0,0,0,1,0x65]));
      for(var i=0;i<6;i++){await t.pump(const Duration(milliseconds:60));}
      expect(h.session.resizing,false);expect(h.session.frameReady,true);
      await openLargeDesktopScreenPanel(t);
      final panel=t.widget<AndroidScreenOptions>(find.byType(AndroidScreenOptions).last);
      expect(panel.deviceClass,AndroidDeviceClass.tablet);
      expect(panel.resolutions.first.size,const Size(914,1462));
    }finally{await h.close(t);}
  });
  testWidgets('late measured density preserves explicitly chosen resolution and live manual DPI',(t)async{
    final h=MetricsStream();await h.open(t,choice:const AndroidScreenChoice(resolution:AndroidResolution.longEdge(1920)));
    try{
      expect((h.starts.single.maxWidth,h.starts.single.maxHeight),(1920,1200));expect(h.scales.single,100);
      await openLargeDesktopScreenPanel(t);
      final panel=t.widget<AndroidScreenOptions>(find.byType(AndroidScreenOptions).last);
      panel.onChoice(panel.choice.copyWith(scale:175));
      await t.pump();
      expect(h.saved.last.screen(AndroidDeviceClass.tablet).scale,175);
      final resizeCount=h.socket.sent.where((e)=>e['type']=='stream.resize').length;
      h.reads.single.complete(const AndroidScreenDensity(xdpi:274.3,ydpi:274.3,width:2560,height:1600));
      await t.pump();await t.pump(const Duration(milliseconds:900));
      final current=t.widget<AndroidScreenOptions>(find.byType(AndroidScreenOptions).last);
      expect(current.choice.scale,175);expect(current.choice.resolution,const AndroidResolution.longEdge(1920));
      expect(h.socket.sent.where((e)=>e['type']=='stream.resize'),hasLength(resizeCount));
      expect(h.starts,hasLength(1));
    }finally{await h.close(t);}
  });
}
