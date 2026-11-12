#include <winsock2.h>  // before windows.h (the fake broker of liveTunnelResume)
#include "track_source.hpp"
#include "track_input.hpp"
#include "track_tunnel.hpp"
#include "remote_track_decoder.h"
#include "../../windows_client/test/native/track_gpu_fixture.hpp"
#include <barrier>
#include <future>
#include <iostream>
#include <tuple>
#include <wincrypt.h>
#pragma comment(lib,"ws2_32.lib")
namespace wire=multitap::tracks;
namespace {
unsigned checks=0;
void require(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
template<class F>void rejects(F action,const char* reason){bool failed=false;try{action();}catch(...){failed=true;}require(failed,reason);}
constexpr const char* testSession="12345678-1234-4234-8234-123456789abc";
host::Json command(uint32_t topology=1){
  return host::JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":\""+std::string(testSession)+"\",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\",\"topologyEpoch\":"+std::to_string(topology)+",\"monitor\":\"all\",\"maxWidth\":1280,\"maxHeight\":720,\"fps\":30,\"bitrate\":8000000,\"audio\":true,\"tracks\":[{\"id\":\"0\",\"width\":640,\"height\":360,\"x\":-640,\"y\":-100,\"epoch\":1},{\"id\":\"1\",\"width\":"+std::to_string(topology==1?800:960)+",\"height\":"+std::to_string(topology==1?600:540)+",\"x\":0,\"y\":0,\"epoch\":"+std::to_string(topology)+"}]}");
}
host::DisplayState display(uint32_t topology=1){
  host::DisplayState value;value.installed=value.deviceReady=value.owned=value.active=true;
  value.monitors={{"0",L"\\\\.\\DISPLAY2",-1600,300,640,360,false},{"1",L"\\\\.\\DISPLAY3",-960,400,DWORD(topology==1?800:960),DWORD(topology==1?600:540),false}};
  value.foreign=0;value.left=-1600;value.top=300;value.width=topology==1?1440:1600;value.height=topology==1?700:640;value.name=L"\\\\.\\DISPLAY2";
  return value;
}
std::vector<host::CaptureMonitor> observed(uint32_t topology=1){
  return {{L"\\\\.\\DISPLAY1",L"foreign-console",{0,0,1920,1080},true},
    {L"\\\\.\\DISPLAY2",L"owned-adapter-0",{-1600,300,-960,660},false},
    {L"\\\\.\\DISPLAY3",L"owned-adapter-1",{-960,400,topology==1?-160:0,topology==1?1000:940},false}};
}
struct Pixels final:host::TrackCapture {
  std::vector<uint8_t> bytes;uint32_t width,height;uint8_t id;unsigned frameIndex=0;
  explicit Pixels(const host::OwnedTrackSource& source):bytes(size_t(source.media.width)*source.media.height*3/2,128),width(source.media.width),height(source.media.height),id(source.media.id){}
  host::TrackCapturedFrame frame()override{
    std::fill(bytes.begin(),bytes.begin()+size_t(width)*height,uint8_t(id?180:40));
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)bytes[size_t(y)*width+(x+frameIndex*7)%(width-32)]=uint8_t(80+frameIndex%60);
    frameIndex++;return {&bytes,true,0,0};
  }
};
host::Json ack(const host::TrackSourcePlan& plan){
  std::string tracks="[";for(const auto& output:plan.outputs){if(tracks.size()>1)tracks+=',';tracks+="{\"id\":"+host::jsonQuote(std::to_string(output.media.id))+",\"epoch\":"+std::to_string(output.media.epoch)+'}';}
  return host::JsonParser::parse("{\"type\":\"stream.tracks.ready\",\"sessionId\":"+host::jsonQuote(plan.sessionId)+",\"topologyEpoch\":"+std::to_string(plan.topology)+",\"tracks\":"+tracks+"]}");
}
host::Json pointer(const host::TrackSourcePlan& plan){
  return host::JsonParser::parse("{\"type\":\"input\",\"sessionId\":"+host::jsonQuote(plan.sessionId)+",\"topologyEpoch\":"+std::to_string(plan.topology)+",\"event\":\"move\",\"track\":\"1\",\"epoch\":"+std::to_string(plan.outputs.back().media.epoch)+",\"x\":0.25,\"y\":0.75}");
}
void contracts(){
  const auto plan=host::bindOwnedTrackSources(command(),display(),observed());
  require(plan.outputs.size()==2&&plan.outputs[0].bitrate+plan.outputs[1].bitrate==8000000,"Source bitrate budget was duplicated across encoders");
  require(plan.outputs[0].capture.monitors.size()==1&&plan.outputs[1].capture.monitors.size()==1&&plan.outputs[0].capture.bounds.right==plan.outputs[1].capture.bounds.left,"Sources were combined or confused with the console");
  rejects([&]{host::selectOwnedCapture(display(),"all",observed());},"Legacy composite unexpectedly accepted unequal-height fixture");
  for(int scenario=0;scenario<10;++scenario){
    auto request=command();auto state=display();auto monitors=observed();auto& fields=std::get<host::Json::Object>(request.value);
    if(scenario==0)state.owned=false;
    if(scenario==1)fields["mediaProtocol"].value=std::string("media-v1");
    if(scenario==2)fields["maxWidth"].value=319.0;
    if(scenario==3)fields["audio"].value=std::string("true");
    if(scenario==4)fields["monitor"].value=std::string("0");
    if(scenario==5)state.monitors[1].width=960;
    if(scenario==6)state.monitors[1].left-=10;
    if(scenario==7)fields["topologyEpoch"].value=0.0;
    if(scenario==8)std::get<host::Json::Object>(std::get<host::Json::Array>(fields["tracks"].value)[1].value)["id"].value=std::string("0");
    if(scenario==9)monitors[0].bounds={-1600,300,-1000,800};
    rejects([&]{host::bindOwnedTrackSources(request,state,monitors);},"Invalid ownership/topology/request reached a track source");
  }
  host::TrackSourceInput input(plan);auto ready=ack(plan);const auto move=pointer(plan);unsigned injected=0;
  const auto receive=[&](const host::Json& value){injected++;const auto x=value.get("x").number(),y=value.get("y").number();
    require(std::abs((plan.inputArea.left+x*(plan.inputArea.right-plan.inputArea.left-1))-(-960+.25*799))<.00001,"Pointer did not map to the selected output's actual horizontal position");
    require(std::abs((plan.inputArea.top+y*(plan.inputArea.bottom-plan.inputArea.top-1))-(400+.75*599))<.00001,"Pointer did not map to the selected output's actual vertical position");
  };
  require(!input.acknowledge(ready)&&!input.apply(move,receive),"Input became active before real source keys");
  input.encodedReady();require(!input.apply(move,receive)&&input.acknowledge(ready)&&input.apply(move,receive)&&injected==1,"Matching two-output decoder readiness did not unlock input");
  for(int scenario=0;scenario<6;++scenario){auto bad=move;auto& fields=std::get<host::Json::Object>(bad.value);
    if(scenario==0)fields["sessionId"].value=std::string("other");
    if(scenario==1)fields["topologyEpoch"].value=2.0;
    if(scenario==2)fields["epoch"].value=2.0;
    if(scenario==3)fields["track"].value=std::string("foreign");
    if(scenario==4)fields["x"].value=1.5;
    if(scenario==5)fields["unexpected"].value=true;
    require(!input.apply(bad,receive),"Wrong-session/epoch/shape pointer reached injection");
  }
  for(const std::string button:{"x1","x2","x3"}){auto side=move;auto& fields=std::get<host::Json::Object>(side.value);
    fields["event"].value=std::string("button");fields["button"].value=button;fields["down"].value=true;
    const auto before=injected;require(input.apply(side,receive)==(button!="x3")&&injected==before+(button!="x3"),"A side button of an output was refused, or an unknown button passed");
  }
  std::promise<void> began,release;auto releaseFuture=release.get_future().share();std::atomic<bool> closeDone=false;
  std::thread admitted([&]{input.apply(move,[&](const host::Json&){began.set_value();releaseFuture.wait();});});
  const bool started=began.get_future().wait_for(std::chrono::seconds(60))==std::future_status::ready;
  if(!started){release.set_value();admitted.join();require(false,"Input race fixture did not begin");}
  std::thread closing([&]{input.close();closeDone=true;});release.set_value();admitted.join();closing.join();
  require(closeDone&&!input.apply(move,receive),"A late source command injected after close");
}
// relative-mouse-tracks-v1 (게임 마우스 on 모든 모니터): movement and buttons/wheel
// at the current cursor, only with both granted markers; positions unchanged.
void relativeInput(){
  const auto plan=host::bindOwnedTrackSources(command(),display(),observed());
  const auto start=[](const std::string& fields){return host::JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\""+fields+'}');};
  require(host::relativeMouseTrackSessionRequested(start(",\"relativeMouseProtocol\":\"relative-mouse-v1\",\"relativeMouseTracksProtocol\":\"relative-mouse-tracks-v1\""))&&
    !host::relativeMouseTrackSessionRequested(start(",\"relativeMouseProtocol\":\"relative-mouse-v1\""))&&!host::relativeMouseTrackSessionRequested(start(",\"relativeMouseTracksProtocol\":\"relative-mouse-tracks-v1\""))&&
    !host::relativeMouseSessionRequested(start(",\"relativeMouseProtocol\":\"relative-mouse-v1\",\"relativeMouseTracksProtocol\":\"relative-mouse-tracks-v1\"")),"The tracks relative grant did not need both markers");
  const auto message=[&](const std::string& fields){return host::JsonParser::parse("{\"type\":\"input\",\"sessionId\":"+host::jsonQuote(plan.sessionId)+",\"topologyEpoch\":1"+fields+'}');};
  const auto move=message(",\"event\":\"moveRelative\",\"dx\":12,\"dy\":-3"),press=message(",\"event\":\"button\",\"button\":\"left\",\"down\":true"),wheel=message(",\"event\":\"wheel\",\"delta\":-120");
  for(const bool granted:{false,true}){
    host::TrackSourceInput input(plan,granted);input.encodedReady();require(input.acknowledge(ack(plan)),"Relative fixture input did not start");
    std::vector<host::Json> injected;const auto receive=[&](const host::Json& value){injected.push_back(value);};
    require(input.apply(move,receive)==granted&&input.apply(press,receive)==granted&&input.apply(wheel,receive)==granted&&injected.size()==(granted?3u:0u),"Relative input did not follow the grant");
    if(!granted)continue;
    require(injected[0].get("event").string()=="moveRelative"&&injected[0].get("dx").number()==12&&injected[0].get("dy").number()==-3&&injected[1].get("button").string()=="left"&&injected[2].get("delta").number()==-120,"Relative input changed on its way to injection");
    for(const auto& bad:{message(",\"event\":\"moveRelative\",\"dx\":0,\"dy\":0"),message(",\"event\":\"moveRelative\",\"dx\":16385,\"dy\":0"),message(",\"event\":\"moveRelative\",\"dx\":1.5,\"dy\":0"),
        message(",\"event\":\"moveRelative\",\"dx\":1"),message(",\"event\":\"moveRelative\",\"dx\":1,\"dy\":1,\"x\":0.5"),message(",\"event\":\"button\",\"button\":\"x3\",\"down\":true"),
        message(",\"event\":\"button\",\"button\":\"left\",\"down\":true,\"track\":\"1\""),message(",\"event\":\"wheel\",\"delta\":12001")})
      require(!input.apply(bad,receive)&&injected.size()==3,"Malformed relative input reached injection");
    // Positions keep working beside it.
    require(input.apply(pointer(plan),receive)&&injected.size()==4&&injected[3].get("event").string()=="move","Positioned input stopped with the relative grant");
  }
}
void tunnelContracts(){
  for(const auto* type:{"input.direct","udp.tracks.grant","udp.tracks.key","udp.tracks.peer","udp.tracks.lease"})require(host::trackTunnelControlAllowed(type,true)&&!host::trackTunnelControlAllowed(type,false),"UDP track controls bypassed negotiation or closed their negotiated tunnel");
  for(const auto* type:{"udp.tracks.local","udp.tracks.unknown","session.start","power.shutdown"})require(!host::trackTunnelControlAllowed(type,true),"Unrelated control reached a track worker");
  host::Credentials credentials;credentials.origin=L"https://track.example:8443";credentials.token="synthetic-source-token";
  const std::string prefix="wss://track.example:8443/api/v1/hosts/sessions/"+std::string(testSession);
  require(host::trackTunnelPath(credentials,testSession,prefix+"/tracks")==host::wide("/api/v1/hosts/sessions/"+std::string(testSession)+"/tracks"),"Exact same-origin track source URL was rejected");
  for(const auto& url:{prefix+"/clipboard",prefix+"/tracks?secret=x",prefix+"/tracks#fragment",
      std::string("wss://other.example:8443/api/v1/hosts/sessions/")+testSession+"/tracks",
      std::string("wss://track.example/api/v1/hosts/sessions/")+testSession+"/tracks",
      std::string("wss://user@track.example:8443/api/v1/hosts/sessions/")+testSession+"/tracks",
      std::string("ws://track.example:8443/api/v1/hosts/sessions/")+testSession+"/tracks"}){
    rejects([&]{host::trackTunnelPath(credentials,testSession,url);},"Foreign/credentialed/unbound track source URL was accepted");
  }
  host::TrackSourceTunnel source;require(!source.send(true,{'a'})&&!source.httpStatus(),"Stopped source accepted an outgoing packet");source.stop();source.stop();
  // tracks-resume-v1: only an established tunnel lost to the transport is held,
  // only where the broker granted it, and only for TrackTunnelHold::holdMs.
  using Ended=host::TrackTunnelHold::Ended;constexpr auto hold=host::TrackTunnelHold::holdMs;
  {host::TrackTunnelHold policy;require(!policy.ended(true,Ended::failed,0),"A first dial that failed was retried instead of ending as before");}
  {host::TrackTunnelHold policy;policy.established();require(!policy.ended(false,Ended::lost,0),"A tunnel without the grant was held");}
  for(const auto why:{Ended::closed,Ended::stopped}){host::TrackTunnelHold policy;policy.established();require(!policy.ended(true,why,0)&&!policy.holding(),"A tunnel the broker closed or the service stopped was held");}
  {host::TrackTunnelHold policy;policy.established();
    require(policy.ended(true,Ended::lost,1000)&&policy.holding(),"An established tunnel lost to the transport was not held");
    std::vector<uint32_t> waits;for(int n=0;n<5;++n)waits.push_back(policy.wait());
    require(waits==std::vector<uint32_t>{250,500,1000,2000,2000},"The redial wait did not grow to its 2 s bound");
    require(policy.ended(true,Ended::failed,1000+hold-1)&&policy.ended(true,Ended::lost,1000+hold-1),"A redial within the hold was not retried");
    require(!policy.ended(true,Ended::failed,1000+hold),"A held tunnel outlived its hold");
    require(!policy.ended(true,Ended::closed,2000),"The broker's close frame during the hold was retried");}
  {host::TrackTunnelHold policy;policy.established();require(policy.ended(true,Ended::lost,0),"Hold fixture");
    policy.established();require(!policy.holding()&&policy.wait()==250&&policy.ended(true,Ended::lost,hold+5)&&!policy.ended(true,Ended::failed,2*hold+5),"A redialled tunnel did not start a fresh hold");}
}
// A fake broker on 127.0.0.1 over plain HTTP (the Host's consented http
// origin): /healthz as a broker answers it, then the session's track tunnel
// upgrade and raw WebSocket frames. Loopback only; no real broker, TLS trust,
// display or device.
struct FakeTunnelBroker {
  SOCKET listener=INVALID_SOCKET;uint16_t port=0;std::string authorization;
  FakeTunnelBroker(){WSADATA data{};require(!WSAStartup(MAKEWORD(2,2),&data),"Winsock");listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int length=sizeof(address);require(listener!=INVALID_SOCKET&&!bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))&&!listen(listener,8)&&!getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length),"Fake broker listen");port=ntohs(address.sin_port);}
  ~FakeTunnelBroker(){closesocket(listener);WSACleanup();}
  std::string origin()const{return "http://127.0.0.1:"+std::to_string(port);}
  static bool readable(SOCKET socket,DWORD ms){fd_set set;FD_ZERO(&set);FD_SET(socket,&set);timeval wait{LONG(ms/1000),LONG(ms%1000)*1000};return select(0,&set,nullptr,nullptr,&wait)==1;}
  static void write(SOCKET socket,const std::string& bytes){require(send(socket,bytes.data(),int(bytes.size()),0)==int(bytes.size()),"Fake broker write");}
  static std::string exact(SOCKET socket,size_t count){std::string bytes(count,'\0');size_t at=0;while(at<count){require(readable(socket,5000),"Fake broker read timeout");const int got=recv(socket,bytes.data()+at,int(count-at),0);require(got>0,"Fake broker read");at+=size_t(got);}return bytes;}
  static std::string header(const std::string& request,const std::string& name){
    auto lower=request;for(auto& c:lower)c=char(tolower(static_cast<unsigned char>(c)));auto key=name;for(auto& c:key)c=char(tolower(static_cast<unsigned char>(c)));
    const auto at=lower.find("\r\n"+key+":");if(at==std::string::npos)return {};auto start=at+key.size()+3;while(request[start]==' ')++start;return request.substr(start,request.find("\r\n",start)-start);}
  static std::string accept(const std::string& key){
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;BYTE digest[20]{};DWORD size=sizeof(digest);const auto text=key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    require(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA1,0,0,&hash)&&CryptHashData(hash,reinterpret_cast<const BYTE*>(text.data()),DWORD(text.size()),0)&&CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0),"Fake broker SHA-1");
    CryptDestroyHash(hash);CryptReleaseContext(provider,0);char encoded[64]{};DWORD length=sizeof(encoded);
    require(CryptBinaryToStringA(digest,size,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded,&length),"Fake broker base64");return std::string(encoded,length);}
  // Answers /healthz on the way; the next track tunnel's socket, or INVALID_SOCKET.
  SOCKET tunnel(DWORD ms){
    const auto until=GetTickCount64()+ms;
    while(GetTickCount64()<until){
      if(!readable(listener,DWORD(std::max<int64_t>(1,int64_t(until-GetTickCount64())))))continue;
      const SOCKET peer=::accept(listener,nullptr,nullptr);if(peer==INVALID_SOCKET)continue;
      std::string request;while(request.find("\r\n\r\n")==std::string::npos){if(!readable(peer,5000))break;char chunk[1024];const int got=recv(peer,chunk,sizeof(chunk),0);if(got<=0)break;request.append(chunk,size_t(got));}
      if(request.rfind("GET /healthz ",0)==0){
        const auto body="{\"service\":\"multitap-broker\",\"status\":\"ok\",\"authentication\":\"id-password\",\"media\":\"relay-v1\",\"transport\":{\"protocol\":\"broker-transport-v1\",\"publicOrigin\":\""+origin()+"\",\"scheme\":\"http\",\"allowInsecureHttp\":true,\"directTls\":false}}";
        write(peer,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body);shutdown(peer,SD_SEND);closesocket(peer);continue;
      }
      if(request.rfind("GET /api/v1/hosts/sessions/"+std::string(testSession)+"/tracks ",0)!=0){closesocket(peer);continue;}
      authorization=header(request,"Authorization");
      write(peer,"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+accept(header(request,"Sec-WebSocket-Key"))+"\r\n\r\n");
      return peer;
    }
    return INVALID_SOCKET;
  }
  static void text(SOCKET socket,const std::string& value){require(value.size()<126,"Fake frame size");write(socket,std::string{static_cast<char>(uint8_t(0x81)),static_cast<char>(uint8_t(value.size()))}+value);}
  static void close(SOCKET socket){const uint8_t frame[]{0x88,2,0x03,0xe8};write(socket,std::string(reinterpret_cast<const char*>(frame),sizeof(frame)));}
  // A transport loss: the connection resets without a close frame.
  static void reset(SOCKET socket){linger value{1,0};setsockopt(socket,SOL_SOCKET,SO_LINGER,reinterpret_cast<const char*>(&value),sizeof(value));closesocket(socket);}
  // The next text message from the Host (masked, as every client frame).
  static std::string message(SOCKET socket){
    for(;;){const auto head=exact(socket,2);const unsigned opcode=uint8_t(head[0])&15;uint64_t length=uint8_t(head[1])&127;
      if(length==126){const auto more=exact(socket,2);length=(uint64_t(uint8_t(more[0]))<<8)|uint8_t(more[1]);}
      else if(length==127){const auto more=exact(socket,8);length=0;for(const char c:more)length=(length<<8)|uint8_t(c);}
      const auto mask=(uint8_t(head[1])&128)?exact(socket,4):std::string(4,'\0');auto payload=exact(socket,size_t(length));
      for(size_t n=0;n<payload.size();++n)payload[n]=char(payload[n]^mask[n%4]);
      if(opcode==1)return payload;
    }
  }
};
// tracks-resume-v1 through the actual WinHTTP tunnel: the broker's
// stream.keyframe reaches the service; a reset tunnel is held (media dropped,
// input released once) and dialled again; the broker's close frame, or a
// session without the grant, ends it as before.
void liveTunnelResume(){
  FakeTunnelBroker broker;
  host::Credentials credentials;credentials.origin=host::wide(broker.origin());credentials.httpConsentOrigin=credentials.origin;credentials.token="synthetic-tunnel-resume-token-not-a-real-host";
  const auto url="ws://127.0.0.1:"+std::to_string(broker.port)+"/api/v1/hosts/sessions/"+testSession+"/tracks";
  std::mutex mutex;std::condition_variable changed;std::vector<std::string> received;unsigned lost=0,closed=0;
  const auto until=[&](auto predicate){std::unique_lock lock(mutex);return changed.wait_for(lock,std::chrono::seconds(10),predicate);};
  const auto start=[&](host::TrackSourceTunnel& tunnel,bool resumable,bool udp=false){
    const auto request=host::JsonParser::parse(udp?R"({"udpTracksProtocol":"udp-tracks-v1"})":"{}");
    tunnel.start(credentials,testSession,url,[&](const std::string& value){std::lock_guard lock(mutex);received.push_back(value);changed.notify_all();},
      [&]{std::lock_guard lock(mutex);++closed;changed.notify_all();},&request,resumable,[&]{std::lock_guard lock(mutex);++lost;changed.notify_all();});};
  {
    host::TrackSourceTunnel tunnel;start(tunnel,true);
    const SOCKET first=broker.tunnel(15000);require(first!=INVALID_SOCKET&&tunnel.waitReady(5000),"The track tunnel did not reach the fake broker");
    require(broker.authorization=="Host "+credentials.token,"The track tunnel lost its Host credential");
    FakeTunnelBroker::text(first,"{\"type\":\"stream.keyframe\",\"sessionId\":\""+std::string(testSession)+"\"}");
    require(until([&]{return received.size()==1;})&&host::JsonParser::parse(received[0]).get("type").string()=="stream.keyframe","stream.keyframe did not reach the service from the track tunnel");
    require(tunnel.send(true,{'a'})&&FakeTunnelBroker::message(first)=="a","Fixture: the tunnel did not carry a message");
    FakeTunnelBroker::reset(first);
    require(until([&]{return lost==1;})&&closed==0,"A reset tunnel of a granted session was not held");
    require(tunnel.send(true,{'h','e','l','d'}),"A held tunnel refused (instead of dropping) media");
    const SOCKET second=broker.tunnel(15000);require(second!=INVALID_SOCKET,"The held tunnel did not dial again");
    require(tunnel.waitReady(5000)&&tunnel.send(true,{'o','k'})&&FakeTunnelBroker::message(second)=="ok","The redialled tunnel did not carry new media (or carried what the hold dropped)");
    {std::lock_guard lock(mutex);require(lost==1&&closed==0,"The redial ended the session or released input twice");}
    FakeTunnelBroker::close(second);
    require(until([&]{return closed==1;}),"The broker's close frame did not end the tunnel");
    require(broker.tunnel(1500)==INVALID_SOCKET,"A tunnel the broker closed dialled again");
    tunnel.stop();closesocket(second);
  }
  {
    {std::lock_guard lock(mutex);lost=closed=0;}
    host::TrackSourceTunnel tunnel;start(tunnel,false);
    const SOCKET only=broker.tunnel(15000);require(only!=INVALID_SOCKET&&tunnel.waitReady(5000),"Fixture: the ungranted tunnel did not connect");
    FakeTunnelBroker::reset(only);
    require(until([&]{return closed==1;})&&lost==0,"A tunnel without the grant was held");
    require(broker.tunnel(1500)==INVALID_SOCKET,"A tunnel without the grant dialled again");
    tunnel.stop();
  }
  {
    {std::lock_guard lock(mutex);received.clear();lost=closed=0;}
    host::TrackSourceTunnel tunnel;start(tunnel,true,true);
    const SOCKET peer=broker.tunnel(15000);require(peer!=INVALID_SOCKET&&tunnel.waitReady(5000),"Negotiated UDP track tunnel did not connect");
    for(const auto* type:{"udp.tracks.grant","udp.tracks.key","udp.tracks.peer","udp.tracks.lease","input.direct"})FakeTunnelBroker::text(peer,"{\"type\":"+host::jsonQuote(type)+",\"sessionId\":"+host::jsonQuote(testSession)+'}');
    require(until([&]{return received.size()==5;})&&closed==0,"Negotiated UDP controls closed the actual WinHTTP tunnel");
    FakeTunnelBroker::close(peer);require(until([&]{return closed==1;}),"Negotiated tunnel did not close");tunnel.stop();closesocket(peer);
  }
  {
    {std::lock_guard lock(mutex);received.clear();lost=closed=0;}
    host::TrackSourceTunnel tunnel;start(tunnel,false);
    const SOCKET peer=broker.tunnel(15000);require(peer!=INVALID_SOCKET&&tunnel.waitReady(5000),"Ungrant fixture did not connect");
    FakeTunnelBroker::text(peer,"{\"type\":\"udp.tracks.grant\",\"sessionId\":"+host::jsonQuote(testSession)+'}');
    require(until([&]{return closed==1;})&&received.empty(),"Unnegotiated UDP track control reached the service");tunnel.stop();closesocket(peer);
  }
  std::cout<<"{\"kind\":\"track-tunnel-resume\",\"actualWinHttp\":true,\"broker\":\"loopback fake\",\"redialled\":1,\"keyframeAllowed\":true}\n";
}
// audio-opus-tracks-v1: a granted command binds Opus, the metadata states what
// is sent, and the gate, the service and the wire take an Opus payload of any
// length only in an Opus stream. tracks-resume-v1: keyFrame() restarts every
// output at an IDR (the relay's stream.keyframe after a resume).
void opusAndKeyFrames(){
  auto granted=command();std::get<host::Json::Object>(granted.value)["audioCodec"].value=std::string("opus");
  const auto opusPlan=host::bindOwnedTrackSources(granted,display(),observed());auto pcmPlan=opusPlan;pcmPlan.audioOpus=false;
  require(opusPlan.audioOpus&&!host::bindOwnedTrackSources(command(),display(),observed()).audioOpus,"The tracks Opus grant was not read from the command");
  const auto format=[](const host::TrackSourcePlan& plan,bool available){const auto info=host::JsonParser::parse(host::trackSourceInfo(plan,available));return info.get("audio").get("format").string()+(info.get("audio").get("available").boolean()?"+":"-");};
  require(format(opusPlan,true)=="opus+"&&format(pcmPlan,true)=="s16le+"&&format(opusPlan,false)=="s16le-","The tracks metadata did not state the audio actually sent");
  require(opusPlan.media(true).audioOpus&&!opusPlan.media(false).audioOpus&&!pcmPlan.media(true).audioOpus,"The gate's audio format did not follow the plan");
  // The wire: an Opus payload (u16 sequence + packet) is not whole PCM frames.
  std::vector<uint8_t> payload(163,0xfc),opusBytes,refused;payload[0]=7;payload[1]=0;
  const wire::Packet opusPacket{wire::audio,0,wire::audioTrack,7,0,1,payload.data(),payload.size()};
  require(wire::pack(opusPacket,opusBytes,true)&&!wire::pack(opusPacket,refused),"The TVT1 audio format did not follow the stream's codec");
  wire::Packet parsed;require(wire::parse(opusBytes.data(),opusBytes.size(),parsed,true)&&parsed.size==163&&!wire::parse(opusBytes.data(),opusBytes.size(),parsed),"Opus TVT1 audio parsed outside an Opus stream");
  require(host::trackWorkerPacket(opusBytes,false,true)==host::TrackWorkerPacket::media&&host::trackWorkerPacket(opusBytes,false,false)==host::TrackWorkerPacket::invalid,"The service forwarded Opus audio of an ungranted session, or refused a granted one");
  for(const bool opus:{false,true}){
    uint64_t now=0;host::TrackSourcePipeline source(opus?opusPlan:pcmPlan,true,[&]{return now;},[]{return observed();},[](const auto& output){return std::make_unique<Pixels>(output);});
    // One step: the IDRs of each output.
    const auto keys=[&]{now+=33333;std::array<unsigned,2> found{};for(auto& bytes:source.step()){wire::Packet packet;require(wire::parse(bytes.data(),bytes.size(),packet,opus),"A source step emitted an invalid packet");if(packet.flags&1)found[packet.track]++;}return found;};
    for(unsigned step=0;step<60&&!source.keysReady();++step)(void)keys();
    require(source.keysReady(),"Opus fixture outputs did not start");
    if(opus){const auto wrapped=source.audio(host::mediaPacket(2,now+1,payload));
      require(wrapped&&wrapped->size()==wire::headerBytes+163&&wire::parse(wrapped->data(),wrapped->size(),parsed,true)&&parsed.kind==wire::audio,"A granted stream did not send its Opus packet");}
    else rejects([&]{(void)source.audio(host::mediaPacket(2,now+1,payload));},"A PCM stream sent a payload of partial frames");
    // Moving pictures code P frames; after keyFrame() the next step is an IDR on every output.
    std::array<unsigned,2> later{};for(unsigned step=0;step<4;++step){const auto found=keys();later[0]+=found[0];later[1]+=found[1];}
    require(later[0]==0&&later[1]==0,"Fixture: the outputs coded IDRs without being asked");
    source.keyFrame();
    std::array<unsigned,2> restarted{};for(unsigned step=0;step<3&&(!restarted[0]||!restarted[1]);++step){const auto found=keys();restarted[0]+=found[0];restarted[1]+=found[1];}
    require(restarted[0]==1&&restarted[1]==1,"keyFrame() did not restart every output at exactly one IDR");
  }
}
void liveRejectedTunnel(const std::wstring& origin){
  host::Credentials credentials;credentials.origin=origin;credentials.token="synthetic-source-negative-token-not-a-real-host";
  const host::Origin target(origin);const auto base=host::utf8(target.text);
  std::promise<void> closed;std::atomic<unsigned> received=0;host::TrackSourceTunnel source;
  source.start(credentials,testSession,"wss:"+base.substr(6)+"/api/v1/hosts/sessions/"+testSession+"/tracks",[&](const std::string&){received++;},[&]{closed.set_value();});
  const bool completed=closed.get_future().wait_for(std::chrono::seconds(20))==std::future_status::ready;
  source.stop();require(completed&&(source.httpStatus()==401||source.httpStatus()==404)&&!received,"Actual default-TLS source failure did not receive an authorization/session rejection");
  std::cout<<"{\"actualWinHttpSourceRequest\":true,\"defaultTlsVerification\":true,\"httpStatus\":"<<source.httpStatus()<<",\"credential\":\"synthetic invalid fixture\",\"hostRegistrationChanged\":false,\"vmOperations\":0}\n";
}
struct Phase {host::TrackSourcePlan plan;std::array<std::vector<std::vector<uint8_t>>,2> packets;std::string info;};
Phase pipeline(uint32_t topology){
  Phase phase;auto monitors=observed(topology);phase.plan=host::bindOwnedTrackSources(command(topology),display(topology),monitors);uint64_t now=0;unsigned created=0;
  std::mutex monitorMutex;std::condition_variable monitorObserved;bool changedObserved=false;unsigned synchronousQueries=0;const auto owner=std::this_thread::get_id();
  host::TrackSourcePipeline source(phase.plan,true,[&]{return now;},[&]{std::lock_guard lock(monitorMutex);if(std::this_thread::get_id()==owner)++synchronousQueries;if(monitors[1].identity==L"replaced-device"){changedObserved=true;monitorObserved.notify_all();}return monitors;},[&](const auto& output){created++;return std::make_unique<Pixels>(output);},
    // The display modes agree with the monitors: a replaced device is no mode change to follow (it fails after captureFollowSettle).
    [&](const std::wstring& name)->std::optional<RECT>{std::lock_guard lock(monitorMutex);for(const auto& m:monitors)if(m.name==name)return m.bounds;return std::nullopt;});
  auto sound=host::mediaPacket(2,1,std::vector<uint8_t>(8));require(!source.audio(sound),"Audio preceded both source IDRs");
  for(unsigned step=0;step<60&&(phase.packets[0].size()<8||phase.packets[1].size()<8);++step){now=uint64_t(step)*33333;
    for(auto& bytes:source.step()){wire::Packet packet;require(wire::parse(bytes.data(),bytes.size(),packet)&&packet.topology==topology,"Source did not emit TVT1 for its current topology");auto& output=phase.packets[packet.track];if(output.size()<8)output.push_back(std::move(bytes));}
  }
  require(created==2&&source.keysReady()&&phase.packets[0].size()==8&&phase.packets[1].size()==8,"Two production source encoders did not produce live independent frames");
  phase.info=host::trackSourceInfo(phase.plan,true);
  multitap::RemoteTrackDecoder decoder;host::check(decoder.Configure(phase.plan.media(true)),"Source decoder configuration");
  std::array<unsigned,2> decoded{};std::array<double,2> levels{};std::array<std::vector<std::vector<uint8_t>>,2> cpuPictures;
  const auto receive=[&](uint8_t id,uint32_t config,uint32_t epoch,multitap::DecodedFrame&& frame){
    require(config==topology&&epoch==phase.plan.outputs[id].media.epoch&&frame.width==phase.plan.outputs[id].media.width&&frame.height==phase.plan.outputs[id].media.height,"Actual decoded source dimensions/identity were crossed");
    decoded[id]++;levels[id]=frame.rgba[(size_t(frame.height/2)*frame.width+frame.width/2)*4];cpuPictures[id].push_back(frame.rgba);
  };
  for(unsigned n=0;n<8;++n)for(unsigned id=0;id<2;++id){const auto& bytes=phase.packets[id][n];host::check(decoder.Push(bytes.data(),bytes.size(),receive),"Actual source packet decode");}
  require(decoded[0]&&decoded[1]&&decoder.ready()&&levels[1]>levels[0]+50,"The two actual decoder outputs were duplicated, delayed or visually indistinguishable");
  {
    // The same packets through the GPU presentation path (WARP stands in for a GPU,
    // see track_gpu_fixture.hpp): shared BGRA textures, one converter per output,
    // and every picture identical to the CPU decoder's RGBA.
    trackgpu::Pipelines gpuPipelines;multitap::RemoteTrackDecoder gpuDecoder;
    host::check(gpuDecoder.ConfigurePipelines(phase.plan.media(true),gpuPipelines.pipelines),"GPU source decoder configuration");
    require(gpuDecoder.gpu(0)&&gpuDecoder.gpu(1)&&!gpuDecoder.hardware(0)&&!gpuDecoder.hardware(1),"GPU pipelines were not configured per output");
    std::array<unsigned,2> gpuDecoded{};
    const auto gpuReceive=[&](uint8_t id,uint32_t config,uint32_t epoch,multitap::DecodedFrame&& frame){
      require(config==topology&&epoch==phase.plan.outputs[id].media.epoch&&frame.width==phase.plan.outputs[id].media.width&&frame.height==phase.plan.outputs[id].media.height,"GPU decoded source dimensions/identity were crossed");
      require(frame.gpu&&frame.rgba.empty()&&frame.gpu->shared&&frame.gpu->width==frame.width&&frame.gpu->height==frame.height,"A GPU output delivered a CPU picture");
      require(gpuDecoded[id]<cpuPictures[id].size()&&trackgpu::rgba(frame)==cpuPictures[id][gpuDecoded[id]],"A GPU picture differs from the CPU decoder's pixels");
      gpuDecoded[id]++;
    };
    for(unsigned n=0;n<8;++n)for(unsigned id=0;id<2;++id){const auto& bytes=phase.packets[id][n];host::check(gpuDecoder.Push(bytes.data(),bytes.size(),gpuReceive),"GPU source packet decode");}
    require(gpuDecoded==decoded&&gpuDecoder.ready(),"The GPU path lost or delayed frames against the CPU path");
  }
  unsigned audio=0;const auto wrapped=source.audio(host::mediaPacket(2,now+1,std::vector<uint8_t>(8)));
  require(wrapped.has_value(),"Shared audio did not unlock after both source keys");
  host::check(decoder.Push(wrapped->data(),wrapped->size(),receive,[&](const uint8_t*,size_t size,uint64_t){audio++;return size==8?S_OK:E_INVALIDARG;}),"Shared source audio decode");
  require(audio==1,"Shared source PCM was duplicated");
  const auto metadata=host::JsonParser::parse(phase.info),stats=host::JsonParser::parse(source.stats(1000));
  require(metadata.get("tracks").value.index()!=0&&stats.get("topologyEpoch").number()==topology,"Source metadata/statistics lost their topology");
  require(synchronousQueries==1,"Capture steps repeated synchronous topology enumeration");
  {std::lock_guard lock(monitorMutex);monitors[1].identity=L"replaced-device";}
  {std::unique_lock lock(monitorMutex);require(monitorObserved.wait_for(lock,std::chrono::seconds(2),[&]{return changedObserved;}),"Topology watcher did not observe the changed identity");}
  bool changedRejected=false;const auto until=host::Clock::now()+std::chrono::seconds(2);
  while(!changedRejected&&host::Clock::now()<until){now+=33333;try{source.step();}catch(const std::runtime_error& e){changedRejected=std::string_view(e.what())=="Selected virtual monitor changed; reconnect the session";if(!changedRejected)throw;}if(!changedRejected){std::unique_lock lock(monitorMutex);monitorObserved.wait_for(lock,std::chrono::milliseconds(1));}}
  require(changedRejected,"Changed source identity kept capturing");
  host::check(decoder.Drain(receive),"Source decoder drain");require(decoded[0]==8&&decoded[1]==8,"Actual source/decoder path lost frames");return phase;
}
// The outputs of one step capture and encode at the same time, each on one
// thread for its capture's whole life (the first output's on the stepping
// thread). Every frame() is a rendezvous of both outputs that a one-thread
// step can never complete (the first waits for the second until its timeout).
struct Rendezvous {
  std::mutex mutex;std::condition_variable met;unsigned arrived=0,generation=0;
  bool meet(){std::unique_lock lock(mutex);const auto mine=generation;
    if(++arrived==2){arrived=0;++generation;met.notify_all();return true;}
    if(met.wait_for(lock,std::chrono::milliseconds(500),[&]{return generation!=mine;}))return true;
    --arrived;return false;}
};
struct Placement {std::mutex mutex;std::array<std::thread::id,2> made{};std::atomic<unsigned> together=0,alone=0,misplaced=0;};
struct Meeting final:host::TrackCapture {
  Pixels pixels;Rendezvous& rendezvous;Placement& placement;std::thread::id made=std::this_thread::get_id();
  Meeting(const host::OwnedTrackSource& source,Rendezvous& meeting,Placement& where):pixels(source),rendezvous(meeting),placement(where){std::lock_guard lock(placement.mutex);placement.made[source.media.id]=made;}
  ~Meeting()override{if(std::this_thread::get_id()!=made)++placement.misplaced;}
  host::TrackCapturedFrame frame()override{if(std::this_thread::get_id()!=made)++placement.misplaced;(rendezvous.meet()?placement.together:placement.alone)++;return pixels.frame();}
};
void concurrentOutputs(){
  Rendezvous rendezvous;Placement placement;uint64_t now=0;constexpr unsigned steps=4;
  {
    const auto plan=host::bindOwnedTrackSources(command(),display(),observed());
    host::TrackSourcePipeline source(plan,false,[&]{return now;},[]{return observed();},[&](const auto& output){return std::make_unique<Meeting>(output,rendezvous,placement);});
    std::array<unsigned,2> packets{};
    for(unsigned step=0;step<steps;++step){now=uint64_t(step)*33333;
      for(auto& bytes:source.step()){wire::Packet packet;require(wire::parse(bytes.data(),bytes.size(),packet),"Concurrent step emitted an invalid packet");packets[packet.track]++;}}
    require(placement.alone==0&&placement.together==2*steps,"The outputs were captured one after the other, not at the same time");
    require(packets[0]==steps&&packets[1]==steps&&source.keysReady(),"Concurrent outputs lost or crossed frames");
    std::lock_guard lock(placement.mutex);
    require(placement.made[0]==std::this_thread::get_id()&&placement.made[1]!=std::this_thread::get_id()&&placement.made[1]!=std::thread::id{},"The second output was not made on its own thread");
  }
  require(placement.misplaced==0,"A capture was used or destroyed off the thread that made it");
}
// cursor-tracks-v1 through the production pipeline with a substituted cursor
// extraction (no GDI): the composition switch of every output and the records
// on their own TVT1 kind, after both outputs' first key frames.
struct CursorFixture {std::array<std::optional<bool>,2> composed;host::cursor::Observation seen;};
struct Pointer final:host::TrackCapture {
  Pixels pixels;CursorFixture& fixture;
  Pointer(const host::OwnedTrackSource& source,CursorFixture& value):pixels(source),fixture(value){}
  host::TrackCapturedFrame frame()override{return pixels.frame();}
  void composeCursor(bool enabled)override{fixture.composed[pixels.id]=enabled;}
  host::cursor::Observation cursorSample()override{return fixture.seen;}
};
void cursorRecords(){
  const auto plan=host::bindOwnedTrackSources(command(),display(),observed());
  host::cursor::Shape shape;shape.format=host::cursor::Format::monochrome;shape.width=shape.height=8;shape.hotspotX=1;shape.hotspotY=2;
  shape.mask.assign(host::cursor::maskStride(8)*16,0);shape.mask[0]=0x80;shape.mask[size_t(8)*host::cursor::maskStride(8)]=0x40;
  const auto arrow=reinterpret_cast<HCURSOR>(uintptr_t(0x101)),unreadable=reinterpret_cast<HCURSOR>(uintptr_t(0x666));
  const host::cursor::Channel::Extract extract=[&](HCURSOR handle)->std::optional<host::cursor::Shape>{if(handle==arrow)return shape;return std::nullopt;};
  for(const bool granted:{false,true}){
    CursorFixture fixture;fixture.seen={true,CURSOR_SHOWING,arrow,POINT{-1300,400}};uint64_t now=0;
    host::TrackSourcePipeline source(plan,false,[&]{return now;},[]{return observed();},[&](const auto& output){return std::make_unique<Pointer>(output,fixture);});
    host::cursor::SessionShapeIds ids;if(granted)source.enableCursor([&]{return ids.next(plan.sessionId);},extract);
    require(granted?fixture.composed[0]==false&&fixture.composed[1]==false:!fixture.composed[0]&&!fixture.composed[1],"The grant did not decide whether the outputs compose the cursor");
    std::array<bool,2> keyed{};std::vector<std::vector<uint8_t>> records;
    const auto run=[&](unsigned steps){records.clear();for(unsigned n=0;n<steps;++n){now+=33333;for(auto& bytes:source.step()){wire::Packet packet;
      if(wire::parse(bytes.data(),bytes.size(),packet)){if(packet.kind==wire::video&&(packet.flags&1))keyed[packet.track]=true;
        require(host::trackWorkerPacket(bytes,granted)==(packet.kind==wire::video?host::TrackWorkerPacket::video:host::TrackWorkerPacket::media),"The service misread track media");continue;}
      require(host::trackWorkerPacket(bytes,granted)==host::TrackWorkerPacket::cursor,"A step emitted a packet that is neither track media nor a granted cursor record");
      require(keyed[0]&&keyed[1],"A cursor record preceded an output's first key frame");records.push_back(std::move(bytes));}}};
    run(12);
    if(!granted){require(records.empty()&&!fixture.composed[0]&&!fixture.composed[1],"An ungranted pipeline sent cursor records or changed composition");continue;}
    require(records.size()==2&&records[0][28]==host::cursor::shapeRecordType&&records[1][28]==host::cursor::stateRecordType&&records[1][29]==1&&
      std::equal(records[0].begin()+32,records[0].begin()+36,records[1].begin()+32),"The first records were not the shape and its visible state");
    for(const auto& bytes:records)require(bytes[4]==host::trackCursorKind&&bytes[5]==0&&bytes[6]==255&&wire::read32(bytes.data()+8)==plan.topology&&!wire::read32(bytes.data()+12),"A cursor record left its TVT1 envelope");
    fixture.seen.position=POINT{-500,900};run(2);require(records.empty(),"Moving to the second output sent records");
    fixture.seen.position=POINT{-1300,700};run(1);require(records.size()==1&&records[0][29]==0,"A cursor outside both outputs was not hidden");
    fixture.seen={true,CURSOR_SHOWING,unreadable,POINT{-500,900}};run(1);
    require(fixture.composed[0]==true&&fixture.composed[1]==true,"An unrepresentable cursor was not composed by the outputs");
    fixture.seen={true,CURSOR_SHOWING,arrow,POINT{-500,900}};run(1);
    require(records.size()==1&&records[0][29]==1&&fixture.composed[0]==false&&fixture.composed[1]==false,"The separated cursor did not return");
    // cursor-compose-v1 (게임 마우스): the outputs draw the cursor and the client is told hidden, until it is let go.
    source.composeRequested(true);run(1);
    require(records.size()==1&&records[0][29]==0&&fixture.composed[0]==true&&fixture.composed[1]==true,"A requested compose did not draw the cursor into the outputs");
    source.composeRequested(false);run(1);
    require(records.size()==1&&records[0][29]==1&&fixture.composed[0]==false&&fixture.composed[1]==false,"The cursor stayed composed after the request ended");
  }
  {
    const auto start=[](const std::string& fields){return host::JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\""+fields+'}');};
    const std::string tracksCursor=",\"cursorProtocol\":\"cursor-v1\",\"cursorTracksProtocol\":\"cursor-tracks-v1\"";
    require(host::cursor::composeTrackSessionRequested(start(tracksCursor+",\"cursorComposeProtocol\":\"cursor-compose-v1\""))&&!host::cursor::composeTrackSessionRequested(start(tracksCursor))&&
      !host::cursor::composeTrackSessionRequested(start(",\"cursorProtocol\":\"cursor-v1\",\"cursorComposeProtocol\":\"cursor-compose-v1\""))&&
      !host::cursor::composeSessionRequested(start(tracksCursor+",\"cursorComposeProtocol\":\"cursor-compose-v1\"")),"Compose on independent outputs did not need cursor-tracks-v1 and its grant");
  }
  const auto record=host::cursor::stateRecord(false,0,std::nullopt);const auto good=host::trackCursorPacket(7,1234,record);
  wire::Packet packet;require(host::trackCursorPacketValid(good.data(),good.size())&&!wire::parse(good.data(),good.size(),packet),"The cursor envelope was not its own TVT1 kind");
  const struct {size_t at;uint8_t value;} fields[]={{0,uint8_t('X')},{4,uint8_t(4)},{5,uint8_t(1)},{6,uint8_t(0)},{7,uint8_t(1)},{12,uint8_t(1)}};
  for(const auto& field:fields){auto bad=good;bad[field.at]=field.value;require(!host::trackCursorPacketValid(bad.data(),bad.size()),"A malformed cursor envelope was forwarded");}
  {auto bad=good;wire::write32(bad.data()+8,0);require(!host::trackCursorPacketValid(bad.data(),bad.size()),"A cursor record without topology was forwarded");}
  {auto bad=good;bad.push_back(0);require(!host::trackCursorPacketValid(bad.data(),bad.size()),"A cursor record with a wrong length was forwarded");}
  // The service forwards a cursor record only for a granted session.start.
  std::vector<uint8_t> audio;const std::vector<uint8_t> pcm(8);
  require(wire::pack({wire::audio,0,wire::audioTrack,7,0,1,pcm.data(),pcm.size()},audio),"Audio fixture");
  require(host::trackWorkerPacket(good,true)==host::TrackWorkerPacket::cursor&&host::trackWorkerPacket(good,false)==host::TrackWorkerPacket::invalid,"The service's cursor forwarding did not follow the grant");
  require(host::trackWorkerPacket(audio,false)==host::TrackWorkerPacket::media&&host::trackWorkerPacket(std::vector<uint8_t>(40),true)==host::TrackWorkerPacket::invalid,"The service's track media check changed");
  rejects([&]{host::trackCursorPacket(0,1,record);},"A cursor record without topology was framed");
  rejects([&]{host::trackCursorPacket(1,1,std::vector<uint8_t>(8));},"A short cursor record was framed");
}
// capture_follow.hpp on independent outputs (모든 모니터): a game switches one
// output to another mode, and Windows moves the other beside it. The stream
// keeps both tracks; each moved output gets a new capture on its own thread at
// its new rectangle, an IDR and no tiles any more, while the other keeps its
// tiles; input maps onto the new rectangle. Substituted monitors, display
// modes and captures: no Windows display is read or changed.
struct FollowFixture {
  std::mutex mutex;std::vector<host::CaptureMonitor> monitors=observed();
  std::array<std::atomic<bool>,2> geometry{};  // the output's next frame() is of another size
  std::vector<std::tuple<uint8_t,RECT,std::thread::id>> made;
  std::vector<host::CaptureMonitor> now(){std::lock_guard lock(mutex);return monitors;}
  std::optional<RECT> mode(const std::wstring& name){std::lock_guard lock(mutex);for(const auto& m:monitors)if(m.name==name)return m.bounds;return std::nullopt;}
  void move(size_t index,RECT bounds){std::lock_guard lock(mutex);monitors[index].bounds=bounds;}
};
struct Still final:host::TrackCapture {
  FollowFixture& fixture;uint8_t id;int width,height;std::vector<uint8_t> bgra,nv12;std::unique_ptr<host::BufferComposedFrame> view;
  Still(const host::OwnedTrackSource& source,FollowFixture& value):fixture(value),id(source.media.id),width(int(source.media.width)),height(int(source.media.height)),bgra(size_t(width)*height*4),nv12(size_t(width)*height*3/2){
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto* p=&bgra[(size_t(y)*width+x)*4];p[0]=uint8_t(x*3+id*90);p[1]=uint8_t(y*5);p[2]=uint8_t((x^y)+id*40);p[3]=255;}
    host::bgraToNv12(bgra.data(),nv12.data(),width,height);
    std::lock_guard lock(fixture.mutex);fixture.made.emplace_back(id,source.capture.bounds,std::this_thread::get_id());
  }
  host::TrackCapturedFrame frame()override{
    if(fixture.geometry[id].exchange(false))throw host::CaptureGeometryChanged();
    view=std::make_unique<host::BufferComposedFrame>(bgra.data(),width,height);return {&nv12,true,0,0};
  }
  host::ComposedFrame* composed()override{return view.get();}
};
void followsOutputs(){
  namespace tiles=multitap::lossless;
  const auto plan=host::bindOwnedTrackSources(command(),display(),observed());FollowFixture fixture;uint64_t now=0;
  host::TrackSourcePipeline source(plan,false,[&]{return now;},[&]{return fixture.now();},[&](const auto& output){return std::make_unique<Still>(output,fixture);},[&](const std::wstring& name){return fixture.mode(name);});
  host::TrackSourceInput input(plan);std::vector<std::pair<uint8_t,RECT>> followed;RECT area{};
  source.onFollow([&](uint8_t id,RECT rect){followed.emplace_back(id,rect);input.follow(id,rect,[&](RECT value){area=value;});});
  input.encodedReady();require(input.acknowledge(ack(plan)),"Follow fixture input did not start");
  // One step: per track, its tile packets (drops, adds) and video key flags, in wire order.
  struct Seen{std::array<std::vector<std::pair<unsigned,unsigned>>,2> tiles;std::array<std::vector<bool>,2> video;std::array<uint64_t,2> tilePts{},keyPts{};};
  const auto run=[&](bool lossless){source.lossless(lossless);now+=33333;Seen seen;
    for(auto& bytes:source.step()){wire::Packet packet;require(wire::parse(bytes.data(),bytes.size(),packet),"A follow step emitted an invalid packet");
      if(tiles::isTilePayload(packet.payload,packet.size)){seen.tiles[packet.track].push_back({tiles::read16(packet.payload+20),tiles::read16(packet.payload+22)});seen.tilePts[packet.track]=packet.pts;}
      else{seen.video[packet.track].push_back(packet.flags&1);if(packet.flags&1)seen.keyPts[packet.track]=packet.pts;}}
    return seen;};
  // Both outputs settle into tiles (static pictures).
  std::array<unsigned,2> added{};
  for(unsigned n=0;n<240&&(!added[0]||!added[1]);++n){const auto seen=run(true);for(unsigned id=0;id<2;++id)for(const auto& [drops,adds]:seen.tiles[id])added[id]+=adds;}
  require(added[0]&&added[1],"Fixture: the outputs did not reach lossless tiles");
  {std::lock_guard lock(fixture.mutex);require(fixture.made.size()==2,"Fixture: captures were remade before any mode change");}
  // A game switches output 0 to 800x450; Windows moves output 1 beside it.
  const RECT game{-1600,300,-800,750},beside{-800,400,0,1000};
  fixture.move(1,game);fixture.move(2,beside);
  Seen after;const auto until=host::Clock::now()+std::chrono::seconds(10);
  while(followed.empty()&&host::Clock::now()<until){after=run(true);if(followed.empty())std::this_thread::sleep_for(std::chrono::milliseconds(20));}
  require(followed.size()==2&&followed[0].first==0&&host::sameRect(followed[0].second,game)&&followed[1].first==1&&host::sameRect(followed[1].second,beside),"A mode change of one output ended the stream instead of following both outputs");
  {std::lock_guard lock(fixture.mutex);
    require(fixture.made.size()==4&&std::get<0>(fixture.made[2])==0&&host::sameRect(std::get<1>(fixture.made[2]),game)&&std::get<0>(fixture.made[3])==1&&host::sameRect(std::get<1>(fixture.made[3]),beside),"The followed captures were not made on the new rectangles");
    require(std::get<2>(fixture.made[2])==std::this_thread::get_id()&&std::get<2>(fixture.made[3])==std::get<2>(fixture.made[1])&&std::get<2>(fixture.made[3])!=std::this_thread::get_id(),"A followed capture was made off its output's thread");}
  // The step that followed leaves no video of the old captures; the next one
  // drops every tile of each followed output just before its IDR.
  if(after.video[0].empty()&&after.video[1].empty())after=run(true);
  for(unsigned id=0;id<2;++id){
    const uint32_t count=tiles::tileColumns(plan.outputs[id].media.width)*tiles::tileRows(plan.outputs[id].media.height);
    require(after.tiles[id].size()==1&&after.tiles[id][0]==std::pair<unsigned,unsigned>{count,0}&&!after.video[id].empty()&&after.video[id].front()&&after.keyPts[id]==after.tilePts[id]+1,"A followed output did not drop every tile with an IDR of its new capture");
  }
  // Tiles stay off for followed outputs; the stream goes on.
  for(unsigned n=0;n<30;++n){const auto seen=run(true);require(seen.tiles[0].empty()&&seen.tiles[1].empty(),"A followed output made tiles again");}
  // Input on a followed output maps onto its new rectangle.
  require(host::sameRect(area,RECT{-1600,300,0,1000}),"The input area did not follow the outputs");
  unsigned injected=0;
  const auto corner=host::JsonParser::parse("{\"type\":\"input\",\"sessionId\":"+host::jsonQuote(plan.sessionId)+",\"topologyEpoch\":1,\"event\":\"move\",\"track\":\"0\",\"epoch\":1,\"x\":1,\"y\":1}");
  require(input.apply(corner,[&](const host::Json& value){injected++;
    require(std::abs((area.left+value.get("x").number()*(area.right-area.left-1))-(game.right-1))<.00001&&std::abs((area.top+value.get("y").number()*(area.bottom-area.top-1))-(game.bottom-1))<.00001,"A position on a followed output mapped onto its old rectangle");})&&injected==1,"Input on a followed output was refused");
  // A WGC frame of another size on an unchanged rectangle rebuilds that capture alone.
  fixture.geometry[1]=true;
  Seen rebuilt=run(false);if(rebuilt.video[1].empty())rebuilt=run(false);
  {std::lock_guard lock(fixture.mutex);require(fixture.made.size()==5&&std::get<0>(fixture.made[4])==1&&host::sameRect(std::get<1>(fixture.made[4]),beside),"A capture that saw another frame size was not rebuilt on its rectangle");}
  require(!rebuilt.video[1].empty()&&rebuilt.video[1].front(),"A rebuilt capture's first video was not an IDR");
}
// Informational: two 1920x1080 outputs with a full-frame moving texture through
// the real encoders (x264 here, no GPU), stepped back to back. Public API only.
struct Texture final:host::TrackCapture {
  std::array<std::vector<uint8_t>,8> frames;unsigned next=0;
  explicit Texture(const host::OwnedTrackSource& source){const uint32_t w=source.media.width,h=source.media.height;
    for(unsigned n=0;n<frames.size();++n){auto& f=frames[n];f.resize(size_t(w)*h*3/2);
      for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)f[size_t(y)*w+x]=uint8_t(16+((x*7+y*3+n*29)^(x*y/3+n*5))%200);
      for(size_t i=size_t(w)*h;i<f.size();++i)f[i]=uint8_t(96+(i*5+n*3)%64);}}
  host::TrackCapturedFrame frame()override{return {&frames[next++%frames.size()],true,0,0};}
};
void benchmark(){
  const auto request=host::JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":\""+std::string(testSession)+"\",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\",\"topologyEpoch\":1,\"monitor\":\"all\",\"maxWidth\":1920,\"maxHeight\":1080,\"fps\":60,\"bitrate\":20000000,\"audio\":false,\"tracks\":[{\"id\":\"0\",\"width\":1920,\"height\":1080,\"x\":0,\"y\":0,\"epoch\":1},{\"id\":\"1\",\"width\":1920,\"height\":1080,\"x\":1920,\"y\":0,\"epoch\":1}]}");
  host::DisplayState state;state.installed=state.deviceReady=state.owned=state.active=true;
  state.monitors={{"0",L"\\\\.\\DISPLAY2",-3840,0,1920,1080,false},{"1",L"\\\\.\\DISPLAY3",-1920,0,1920,1080,false}};
  state.foreign=0;state.left=-3840;state.top=0;state.width=3840;state.height=1080;state.name=L"\\\\.\\DISPLAY2";
  const std::vector<host::CaptureMonitor> monitors{{L"\\\\.\\DISPLAY1",L"foreign-console",{0,0,1920,1080},true},{L"\\\\.\\DISPLAY2",L"owned-adapter-0",{-3840,0,-1920,1080},false},{L"\\\\.\\DISPLAY3",L"owned-adapter-1",{-1920,0,0,1080},false}};
  const auto plan=host::bindOwnedTrackSources(request,state,monitors);uint64_t now=0;
  host::TrackSourcePipeline source(plan,false,[&]{return now;},[&]{return monitors;},[](const auto& output){return std::make_unique<Texture>(output);});
  std::vector<double> ticks;const auto began=host::Clock::now();
  for(unsigned step=0;step<240;++step){now=uint64_t(step)*16667;const auto start=host::Clock::now();(void)source.step();ticks.push_back(std::chrono::duration<double,std::milli>(host::Clock::now()-start).count());}
  const auto ms=uint32_t(std::max<int64_t>(1,std::chrono::duration_cast<std::chrono::milliseconds>(host::Clock::now()-began).count()));
  std::sort(ticks.begin(),ticks.end());double sum=0;for(const auto t:ticks)sum+=t;
  std::cout<<"{\"kind\":\"track-source-benchmark\",\"outputs\":2,\"size\":\"1920x1080\",\"steps\":"<<ticks.size()<<",\"stepMeanMs\":"<<sum/double(ticks.size())<<",\"stepP95Ms\":"<<ticks[ticks.size()*95/100]<<",\"stats\":"<<source.stats(std::min<uint32_t>(ms,10000))<<"}\n";
}
void writeNew(const std::filesystem::path& path,const uint8_t* bytes,size_t size){
  HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(file!=INVALID_HANDLE_VALUE,"Fixture output already exists");
  DWORD written=0;const bool ok=size<=MAXDWORD&&WriteFile(file,bytes,DWORD(size),&written,nullptr)&&written==size&&FlushFileBuffers(file);CloseHandle(file);require(ok,"Fixture output was not durable");
}
void exportPhases(const std::filesystem::path& directory,const std::array<Phase,2>& phases){
  require(std::filesystem::is_directory(directory)&&std::filesystem::is_empty(directory)&&!(GetFileAttributesW(directory.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"Fixture output must be an empty ordinary directory");
  std::ostringstream manifest;manifest<<"{\"format\":\"multitap-native-track-fixture-v1\",\"actualMicrosoftCpuEncoder\":true,\"sourcePipeline\":true,\"syntheticPixels\":true,\"windowsDisplaysChanged\":false,\"sourceMetadata\":[";
  manifest<<phases[0].info<<','<<phases[1].info<<"],\"frames\":[";unsigned count=0;
  for(const auto& phase:phases)for(unsigned n=0;n<8;++n)for(unsigned id=0;id<2;++id){
    const auto& bytes=phase.packets[id][n];wire::Packet packet;require(wire::parse(bytes.data(),bytes.size(),packet),"Fixture packet changed");
    const auto name="topology-"+std::to_string(phase.plan.topology)+"-track-"+std::to_string(id)+"-frame-"+std::to_string(n)+".tvt1";writeNew(directory/name,bytes.data(),bytes.size());
    if(count++)manifest<<',';manifest<<"{\"file\":"<<host::jsonQuote(name)<<",\"track\":"<<host::jsonQuote(std::to_string(id))<<",\"topologyEpoch\":"<<packet.topology<<",\"epoch\":"<<packet.epoch<<",\"width\":"<<phase.plan.outputs[id].media.width<<",\"height\":"<<phase.plan.outputs[id].media.height<<",\"ptsUs\":"<<packet.pts<<",\"key\":"<<((packet.flags&1)?"true":"false")<<'}';
  }manifest<<"]}";const auto json=manifest.str();writeNew(directory/L"manifest.json",reinterpret_cast<const uint8_t*>(json.data()),json.size());
}
}
int wmain(int argc,wchar_t** argv){
  try{host::Runtime runtime;
    if(argc==2&&std::wstring(argv[1])==L"--benchmark"){benchmark();return 0;}
    if(argc==2&&std::wstring(argv[1])==L"--follow-only"){followsOutputs();std::cout<<"{\"kind\":\"track-follow\",\"checks\":"<<checks<<"}\n";return 0;}
    contracts();relativeInput();tunnelContracts();liveTunnelResume();opusAndKeyFrames();concurrentOutputs();cursorRecords();followsOutputs();const std::array<Phase,2> phases{pipeline(1),pipeline(2)};
    if(argc==3&&std::wstring(argv[1])==L"--fixture-output")exportPhases(argv[2],phases);else if(argc==3&&std::wstring(argv[1])==L"--broker")liveRejectedTunnel(argv[2]);else require(argc==1,"Unknown source QA arguments");
    std::cout<<"{\"checks\":"<<checks<<",\"actualSourceEncoders\":4,\"actualClientDecoders\":4,\"gpuPathIdenticalPixels\":true,\"nativeSourcePackets\":32,\"sourceCapture\":\"synthetic owned-output fixture\",\"inputInjection\":\"recorded callback only\",\"actualWindowsCapture\":false,\"actualPhysicalPresentation\":false,\"windowsDisplayChanges\":0}\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
