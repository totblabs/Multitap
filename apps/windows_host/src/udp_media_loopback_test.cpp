// The Host's whole media send path on UDP: the real Agent and its real leg (socket,
// thread, pacer, crypto) against a fake relay on 127.0.0.1, with the shared core's
// Receiver as the client. Loopback only: this says nothing about a real network, a
// NAT, the actual relay or the actual client.
#include "agent.hpp"
#include <iostream>
#include <exception>
#include <map>
#include <mutex>
using namespace host;
namespace core=multitap::udp;
namespace {
int passed=0;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
using Bytes=std::vector<uint8_t>;
void evidenceMaximum(std::atomic<uint64_t>& maximum,uint64_t value){auto old=maximum.load();while(value>old&&!maximum.compare_exchange_weak(old,value)){}}
const std::string testSession="00000000-0000-0000-0000-000000000001";
std::string startCommand(const std::string& extra=R"(,"bitrateMode":"auto","bitrate":20000000,"bitrateMax":50000000)",const std::string& monitor="0"){return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\""+testSession+"\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":1,\"monitor\":\""+monitor+"\",\"fps\":60"+extra+"}";}
std::string base64(std::span<const uint8_t> bytes){char text[128]{};DWORD length=DWORD(sizeof(text));require(CryptBinaryToStringA(bytes.data(),DWORD(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,text,&length),"Base64 failed");return std::string(text,length);}
Bytes media(uint8_t kind,bool key,uint64_t pts,size_t payload){
  Bytes bytes(12+payload);bytes[0]=kind;bytes[1]=key?1:0;bytes[2]=1;for(int n=0;n<8;++n)bytes[4+n]=uint8_t(pts>>(8*n));
  for(size_t n=12;n<bytes.size();++n)bytes[n]=uint8_t(pts*31+n*7);return bytes;
}
Bytes textBytes(const std::string& text){return {text.begin(),text.end()};}
template<class Done> bool waitUntil(Done&& done,DWORD milliseconds){const auto end=GetTickCount64()+milliseconds;while(GetTickCount64()<end){if(done())return true;Sleep(1);}return done();}
struct Winsock{Winsock(){WSADATA data{};require(!WSAStartup(MAKEWORD(2,2),&data),"Winsock unavailable");}~Winsock(){WSACleanup();}};
sockaddr_in loopback(uint16_t port){sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);return address;}
struct Socket {
  SOCKET handle=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);uint16_t port=0;
  std::atomic<uint64_t> sendAttempts{0},sendErrors{0};std::atomic<int> lastSendError{0};
  Socket(){require(handle!=INVALID_SOCKET,"Socket failed");const auto any=loopback(0);require(!::bind(handle,reinterpret_cast<const sockaddr*>(&any),sizeof(any)),"Bind failed");sockaddr_in bound{};int size=sizeof(bound);require(!getsockname(handle,reinterpret_cast<sockaddr*>(&bound),&size),"Name failed");port=ntohs(bound.sin_port);
    int buffer=4<<20;setsockopt(handle,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer));setsockopt(handle,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer));BOOL report=FALSE;DWORD used=0;WSAIoctl(handle,SIO_UDP_CONNRESET,&report,sizeof(report),nullptr,0,&used,nullptr,nullptr);}
  ~Socket(){closesocket(handle);}
  Socket(const Socket&)=delete;Socket& operator=(const Socket&)=delete;
  void timeout(DWORD milliseconds){setsockopt(handle,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&milliseconds),sizeof(milliseconds));}
  void nonblocking(){u_long on=1;ioctlsocket(handle,FIONBIO,&on);}
  std::optional<Bytes> receive(sockaddr_in* from=nullptr){Bytes bytes(1500);sockaddr_in source{};int size=sizeof(source);const int got=recvfrom(handle,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&source),&size);if(got==SOCKET_ERROR)return {};if(from)*from=source;bytes.resize(size_t(got));return bytes;}
  void send(std::span<const uint8_t> bytes,const sockaddr_in& to){
    const int sent=sendto(handle,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&to),sizeof(to));
    const int error=sent==SOCKET_ERROR?WSAGetLastError():0;++sendAttempts;if(sent!=int(bytes.size())){++sendErrors;lastSendError=error;}
  }
  void evidence(std::ostream& out)const{out<<"{\"sendAttempts\":"<<sendAttempts.load()<<",\"sendErrors\":"<<sendErrors.load()<<",\"lastSendError\":"<<lastSendError.load()<<'}';}
};
UdpGrant legGrant(uint8_t seed){UdpGrant grant;for(size_t n=0;n<grant.id.size();++n)grant.id[n]=uint8_t(seed+n);for(size_t n=0;n<grant.ticket.size();++n)grant.ticket[n]=uint8_t(seed*3+n);return grant;}
bool sameAddress(const sockaddr_in& a,const sockaddr_in& b){return a.sin_addr.s_addr==b.sin_addr.s_addr&&a.sin_port==b.sin_port;}
// The relay's forwarding, as server/src/media-udp.mjs has it: BIND proved by HMAC pins
// a source, DATA from a pinned source goes to the other leg. Loss is every Nth media
// datagram toward the client (a TAIL or a repair is not spared).
class FakeRelay {
 public:
  std::atomic<bool> severed{false};std::atomic<uint32_t> dropEvery{0};std::atomic<uint64_t> dropped{0},forwarded{0};
  uint16_t port()const{return socket_.port;}
  FakeRelay(const UdpGrant& host,const UdpGrant& client){require(udpLegDatagrams(host,host_)&&udpLegDatagrams(client,client_),"HMAC failed");socket_.timeout(50);thread_=std::thread([this]{run();});}
  ~FakeRelay(){stop_=true;thread_.join();}
  // From the relay's own address, so the leg takes it as forwarded.
  void toHost(std::span<const uint8_t> bytes){std::lock_guard lock(mutex_);if(hostAt_)socket_.send(bytes,*hostAt_);}
  void evidence(std::ostream& out)const{out<<"{\"intentionalDrops\":"<<dropped.load()<<",\"forwardAttempts\":"<<forwarded.load()<<",\"socket\":";socket_.evidence(out);out<<'}';}
 private:
  void run(){
    while(!stop_){
      sockaddr_in from{};const auto bytes=socket_.receive(&from);if(!bytes||bytes->size()<9)continue;
      std::lock_guard lock(mutex_);
      if((*bytes)[0]==udpTypeBind){
        if(std::equal(bytes->begin(),bytes->end(),host_.bind.begin(),host_.bind.end())){hostAt_=from;socket_.send(host_.ack,from);}
        else if(std::equal(bytes->begin(),bytes->end(),client_.bind.begin(),client_.bind.end())){clientAt_=from;socket_.send(client_.ack,from);}
        continue;
      }
      if((*bytes)[0]!=udpTypeData||!hostAt_||!clientAt_||severed)continue;
      const bool fromHost=sameAddress(from,*hostAt_)&&std::equal(host_.id.begin(),host_.id.end(),bytes->begin()+1),fromClient=sameAddress(from,*clientAt_)&&std::equal(client_.id.begin(),client_.id.end(),bytes->begin()+1);
      if(!fromHost&&!fromClient)continue;
      if(const auto every=dropEvery.load();fromHost&&every&&bytes->size()>100&&++seen_%every==0){++dropped;continue;}
      ++forwarded;socket_.send(*bytes,fromHost?*clientAt_:*hostAt_);
    }
  }
  Socket socket_;UdpLegDatagrams host_,client_;std::mutex mutex_;std::optional<sockaddr_in> hostAt_,clientAt_;std::thread thread_;std::atomic<bool> stop_{false};uint64_t seen_=0;
};
// The client: the shared core behind its own socket, on its own thread once keyed.
class Client {
 public:
  explicit Client(const UdpGrant& grant){require(udpLegDatagrams(grant,own_),"HMAC failed");socket_.timeout(100);}
  ~Client(){halt();}
  std::string publicKey()const{return base64(pair_.publicKey());}
  void bind(uint16_t relay){
    relay_=loopback(relay);
    for(int n=0;n<20;++n){socket_.send(own_.bind,relay_);const auto reply=socket_.receive();if(reply&&std::equal(reply->begin(),reply->end(),own_.ack.begin(),own_.ack.end()))return;}
    throw std::runtime_error("The client got no ACK");
  }
  bool ping(){
    Bytes bytes(udpProbeBytes,0);bytes[0]=udpTypeData;std::copy(own_.id.begin(),own_.id.end(),bytes.begin()+1);bytes[9]=udpPing;
    for(int n=0;n<30;++n){socket_.send(bytes,relay_);const auto reply=socket_.receive();if(reply&&reply->size()==udpProbeBytes&&(*reply)[9]==udpPong)return true;}
    return false;
  }
  void adopt(const std::string& reply){
    core::PublicKey host;require(udpBase64(JsonParser::parse(reply).get("publicKey").string(),host),"The reply's key is not 32 bytes of base64");
    core::Secret<core::keyBytes> shared;require(pair_.agree(host,shared),"No agreement");
    require(core::deriveKeys(shared,pair_.publicKey(),host,reinterpret_cast<const uint8_t*>(testSession.data()),testSession.size(),core::Role::client,keys_),"No keys");
    sealer_=std::make_unique<core::Sealer>(keys_.send,own_.id);opener_=std::make_unique<core::Opener>(keys_.receive);
    receiver_.emplace(uint16_t(1),udpProbeNowUs());socket_.nonblocking();thread_=std::thread([this]{run();});
  }
  // Sealed by the same sealer as FEEDBACK, on the client's thread.
  void sendKeyFrameRequest(){keyFrameWanted_=true;}
  void halt(){stop_=true;if(thread_.joinable())thread_.join();}
  std::map<uint64_t,Bytes> received(bool video){std::lock_guard lock(mutex_);return video?video_:audio_;}
  size_t videoCount(){std::lock_guard lock(mutex_);return video_.size();}
  void evidence(std::ostream& out,uint64_t beganUs){
    size_t video=0,audio=0;{std::lock_guard lock(mutex_);video=video_.size();audio=audio_.size();}
    out<<"{\"uniqueVideoDelivered\":"<<video<<",\"uniqueAudioDelivered\":"<<audio<<",\"videoDeliveries\":"<<videoDeliveries_.load()<<",\"audioDeliveries\":"<<audioDeliveries_.load()
      <<",\"tickCalls\":"<<tickCalls_.load()<<",\"maxTickGapUs\":"<<maxTickGapUs_.load()<<",\"tickGapsOver20ms\":"<<tickGapsOver20ms_.load()
      <<",\"maxDrainBatch\":"<<maxDrainBatch_.load()<<",\"drainsOver256\":"<<drainsOver256_.load()<<",\"maxDrainUs\":"<<maxDrainUs_.load()<<",\"drainCapApplied\":false,\"socket\":";
    socket_.evidence(out);out<<",\"controls\":[";
    {std::lock_guard lock(controlMutex_);for(size_t n=0;n<controlKept_;++n){if(n)out<<',';const auto& row=controls_[n];out<<'['<<int(row.kind)<<','<<(int64_t(row.atUs)-int64_t(beganUs))<<','<<row.sealedBytes<<']';}out<<"],\"controlAttempts\":"<<controlAttempts_<<",\"controlsNotRetained\":"<<(controlAttempts_-controlKept_);}
    out<<",\"controlColumns\":[\"kind\",\"relativeUs\",\"sealedBytes\"]}";
  }
 private:
  void take(core::Receiver::Output& output){
    {std::lock_guard lock(mutex_);for(auto& message:output.messages){++(message.video?videoDeliveries_:audioDeliveries_);(message.video?video_:audio_)[message.pts]=std::move(message.bytes);}}
    for(const auto& plain:output.control)sendSealed(plain);
  }
  void sendSealed(const Bytes& plain){
    const auto at=udpProbeNowUs();std::array<uint8_t,core::maxDatagram> sealed;const size_t size=sealer_->seal(plain.data(),plain.size(),sealed.data());
    if(size)socket_.send(std::span<const uint8_t>(sealed.data(),size),relay_);
    std::lock_guard lock(controlMutex_);++controlAttempts_;if(controlKept_<controls_.size())controls_[controlKept_++]={plain.empty()?uint8_t(0):plain[0],at,size};
  }
  void run(){
    FramePacer pacer;std::array<uint8_t,core::maxPlaintext> plain;uint64_t previousTick=0;
    while(!stop_){
      const auto drainBegan=udpProbeNowUs();uint64_t drained=0;
      while(const auto bytes=socket_.receive()){
        ++drained;
        if(const size_t size=opener_->open(bytes->data(),bytes->size(),plain.data())){core::Receiver::Output output;receiver_->datagram(plain.data(),size,udpProbeNowUs(),output);take(output);}
      }
      const auto at=udpProbeNowUs();evidenceMaximum(maxDrainBatch_,drained);evidenceMaximum(maxDrainUs_,at-drainBegan);if(drained>256)++drainsOver256_;
      if(previousTick){const auto gap=at-previousTick;evidenceMaximum(maxTickGapUs_,gap);if(gap>20000)++tickGapsOver20ms_;}previousTick=at;++tickCalls_;
      core::Receiver::Output output;receiver_->tick(at,output);take(output);
      if(keyFrameWanted_.exchange(false)){Bytes request(core::keyframeBytes);request[0]=core::kindKeyframe;core::put16(&request[2],1);sendSealed(request);}
      pacer.wait(std::chrono::steady_clock::now()+std::chrono::milliseconds(1));
    }
  }
  Socket socket_;UdpLegDatagrams own_;sockaddr_in relay_{};core::X25519 pair_;core::SessionKeys keys_;std::unique_ptr<core::Sealer> sealer_;std::unique_ptr<core::Opener> opener_;std::optional<core::Receiver> receiver_;
  std::mutex mutex_;std::map<uint64_t,Bytes> video_,audio_;std::thread thread_;std::atomic<bool> stop_{false},keyFrameWanted_{false};
  std::atomic<uint64_t> videoDeliveries_{0},audioDeliveries_{0},tickCalls_{0},maxTickGapUs_{0},tickGapsOver20ms_{0},maxDrainBatch_{0},drainsOver256_{0},maxDrainUs_{0};
  struct ControlEvidence{uint8_t kind=0;uint64_t atUs=0;size_t sealedBytes=0;};
  std::mutex controlMutex_;std::array<ControlEvidence,256> controls_{};size_t controlKept_=0;uint64_t controlAttempts_=0;
};
struct Bridge final:AgentBridge {
  Send output;std::atomic<int> keyFrames{0},stops{0},rate{0};
  std::mutex inputMutex;std::vector<Json> input;
  void start(const Json&,Send send)override{output=std::move(send);}
  void control(const Json& value)override{std::lock_guard lock(inputMutex);input.push_back(value);}
  std::vector<Json> controls(){std::lock_guard lock(inputMutex);return input;}
  void stop()override{++stops;}
  std::string capabilities()override{return "{\"service\":true}";}
  void displayControl(const Json&,Send)override{}
  void bitrate(const std::string&,int value)override{rate=value;std::lock_guard lock(rateMutex);++rateChanges;if(ratesKept<rates.size())rates[ratesKept++]={udpProbeNowUs(),value};}
  void keyFrame()override{++keyFrames;}
  void evidence(std::ostream& out,uint64_t beganUs){std::lock_guard lock(rateMutex);out<<"{\"latestTarget\":"<<rate.load()<<",\"changes\":"<<rateChanges<<",\"notRetained\":"<<(rateChanges-ratesKept)<<",\"history\":[";for(size_t n=0;n<ratesKept;++n){if(n)out<<',';out<<'['<<(int64_t(rates[n].atUs)-int64_t(beganUs))<<','<<rates[n].value<<']';}out<<"],\"columns\":[\"relativeUs\",\"targetBps\"],\"syntheticBytesIgnoreTarget\":true}";}
  struct RateEvidence{uint64_t atUs=0;int value=0;};std::mutex rateMutex;std::array<RateEvidence,128> rates{};size_t ratesKept=0;uint64_t rateChanges=0;
};
struct Written {bool text=false;Bytes bytes;};
// One session on the real Agent, with a fake relay and a client around it. Agent::stream
// stops whatever ran before, so a session that stayed up has bridge.stops==1.
struct Scene {
  UdpGrant hostGrant=legGrant(0x40),clientGrant=legGrant(0x80);FakeRelay relay{hostGrant,clientGrant};Client client{clientGrant};Bridge bridge;Agent agent;
  explicit Scene(const std::string& start=startCommand()):agent([](const std::wstring&){},&bridge){agent.publicationTestOpen();agent.localControl(start);}
  void grant(const std::string& extra=""){agent.localControl("{\"type\":\"udp.grant\",\"protocol\":\"udp-media-v1\",\"relays\":[{\"host\":\"127.0.0.1\",\"port\":"+std::to_string(relay.port())+"}],\"id\":\""+base64(hostGrant.id)+"\",\"ticket\":\""+base64(hostGrant.ticket)+"\",\"expiresAt\":1793520000000"+extra+"}");require(bool(agent.publicationTestUdpLeg()),"The grant started no leg");}
  void join(){Sleep(50);client.bind(relay.port());require(client.ping(),"No PONG");}
  void bound(){grant();join();}
  std::string keyMessage(const std::string& session=testSession){return "{\"type\":\"udp.key\",\"protocol\":\"udp-media-v1\",\"publicKey\":\""+client.publicKey()+"\",\"sessionId\":\""+session+"\"}";}
  std::vector<Written> drain(){std::vector<Written> out;require(agent.publicationTestDrain([&](bool text,const std::vector<uint8_t>& bytes){out.push_back({text,bytes});return true;}),"Drain failed");return out;}
  std::optional<std::string> keyReply(){for(const auto& each:drain())if(each.text&&JsonParser::parse(std::string(each.bytes.begin(),each.bytes.end())).get("type").string()=="udp.key")return std::string(each.bytes.begin(),each.bytes.end());return {};}
  void info(){const auto bytes=textBytes(R"({"type":"stream.info","codec":"h264","sessionId":")"+testSession+R"(","epoch":1})");bridge.output(true,bytes);}
  void firstIdr(uint64_t pts){info();bridge.output(false,media(1,true,pts,3000));drain();}
  // Probe passed, keys done, address proven, IDR requested.
  void armed(){
    join();const int before=bridge.keyFrames;agent.localControl(keyMessage());const auto reply=keyReply();require(bool(reply),"The leg gave no key reply");client.adopt(*reply);
    require(waitUntil([&]{return bridge.keyFrames>before;},3000),"Proof never asked for the IDR");
  }
  double stat(const char* key){bridge.output(true,textBytes(R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":19000})"));double value=-1;for(const auto& each:drain())if(each.text){const auto parsed=JsonParser::parse(std::string(each.bytes.begin(),each.bytes.end()));if(parsed.get("type").string()=="stream.stats")value=parsed.get(key).number(-1);}return value;}
};
// Test-only bounded summary survives any original mediaMoves assertion. It
// snapshots live counters; it does not stop workers, drain queues or send controls.
struct MediaMovesSummary {
  Scene& scene;uint64_t beganUs=udpProbeNowUs();int exceptions=std::uncaught_exceptions();uint32_t stage=0;
  Json originalStats;bool originalStatsObserved=false,wssAssertionKnown=false,wssAssertionPassed=false;
  ~MediaMovesSummary()noexcept{
    try{
      std::ostringstream out;out<<"{\"kind\":\"udp-media-forensic-final\",\"stage\":"<<stage<<",\"assertionUnwinding\":"<<(std::uncaught_exceptions()>exceptions?"true":"false")
        <<",\"elapsedUs\":"<<(udpProbeNowUs()-beganUs)<<",\"framesPlanned\":240,\"lossEvery\":33,\"wssAssertionKnown\":"<<(wssAssertionKnown?"true":"false")
        <<",\"wssAssertionPassed\":"<<(wssAssertionPassed?"true":"false")<<",\"liveCounterSnapshot\":true,\"client\":";
      scene.client.evidence(out,beganUs);out<<",\"relay\":";scene.relay.evidence(out);out<<",\"workerTarget\":";scene.bridge.evidence(out,beganUs);
      out<<",\"originalStatsObserved\":"<<(originalStatsObserved?"true":"false")<<",\"originalStats\":{";bool comma=false;for(const auto* name:{"udpPath","udpFallbacks","udpDatagrams","udpRetransmits","udpRecoveries","udpBacklogKeys","udpClientKeys","udpRefused"}){if(comma)out<<',';comma=true;out<<'\"'<<name<<"\":"<<originalStats.get(name).number(-1);}out<<"}}\n";
      std::cout<<out.str()<<std::flush;
    }catch(...){ }
  }
};
void mediaMoves(){
  // The Broker's order: stream.info, its grant, and the first IDR just behind.
  Winsock winsock;Scene scene;MediaMovesSummary summary{scene};summary.stage=1;scene.info();scene.grant();scene.bridge.output(false,media(1,true,1000,3000));scene.drain();summary.stage=2;scene.armed();summary.stage=3;
  uint64_t pts=2000;scene.bridge.output(false,media(1,false,pts,1000));const auto before=scene.drain();
  require(before.size()==1&&!before[0].text&&before[0].bytes==media(1,false,pts,1000),"Armed: a P-frame left WebSocket before the key frame");++passed;
  scene.relay.dropEvery=33;  // 3 % of what crosses toward the client, repairs included
  std::map<uint64_t,Bytes> video,audio;int keyFrames=scene.bridge.keyFrames;bool key=true;const auto began=GetTickCount64();FramePacer pacer;auto next=std::chrono::steady_clock::now();
  struct FrameEvidence {uint64_t sentMs=0;bool key=false,awaitingKey=false;int keyRequests=0;};
  std::array<FrameEvidence,240> evidence{};
  summary.stage=4;
  for(int frame=0;frame<240;++frame){
    evidence[size_t(frame)]={GetTickCount64()-began,key,scene.agent.publicationTestAwaitingKeyFrame(),scene.bridge.keyFrames.load()};
    pts+=16667;const size_t size=key?150000:35000+size_t(frame%7)*1000;
    auto bytes=media(1,key,pts,size);video[pts]=bytes;scene.bridge.output(false,std::move(bytes));key=false;
    auto sound=media(2,false,pts+1,1920);audio[pts+1]=sound;scene.bridge.output(false,std::move(sound));
    if(const int now=scene.bridge.keyFrames;now!=keyFrames){keyFrames=now;key=true;}  // as the worker would
    next+=std::chrono::microseconds(16667);pacer.wait(next);
  }
  const auto written=scene.drain();const auto sentMs=GetTickCount64()-began;summary.stage=5;
  // QA-only forensic evidence. Preserve the original one final WSS drain and
  // every assertion, frame, pacing wait and loss setting. These counts describe
  // records retained by the existing queue, not records it may already have dropped.
  {
    std::array<uint32_t,240> wssVideo{},wssAudio{};uint32_t other=0;int first=-1,last=-1;
    for(const auto& each:written)if(!each.text){
      uint64_t at=0;if(each.bytes.size()>=12)for(int n=0;n<8;++n)at|=uint64_t(each.bytes[size_t(4+n)])<<(8*n);
      const bool videoRecord=!each.bytes.empty()&&each.bytes[0]==1,audioRecord=!each.bytes.empty()&&each.bytes[0]==2;
      const uint64_t base=2000+(audioRecord?1:0);const auto offset=at>=base?at-base:0;
      if((videoRecord||audioRecord)&&at>=base&&offset%16667==0&&offset/16667>=1&&offset/16667<=240){
        const int frame=int(offset/16667)-1;++(videoRecord?wssVideo:wssAudio)[size_t(frame)];if(first<0||frame<first)first=frame;if(frame>last)last=frame;
      }else ++other;
    }
    summary.wssAssertionKnown=true;summary.wssAssertionPassed=first<0&&other==0;
    std::cout<<"{\"kind\":\"udp-switch-forensics\",\"originalWssAssertionPassed\":"<<(first<0&&other==0?"true":"false")
      <<",\"firstRetainedWssFrame\":"<<first<<",\"lastRetainedWssFrame\":"<<last<<",\"firstRetainedWssSendMs\":"<<(first<0?-1:int64_t(evidence[size_t(first)].sentMs))
      <<",\"lastRetainedWssSendMs\":"<<(last<0?-1:int64_t(evidence[size_t(last)].sentMs))<<",\"otherWssBinary\":"<<other<<",\"queueMayHaveDroppedEarlierRecords\":true,\"sentMs\":"<<sentMs;
    std::cout<<",\"frames\":[";for(size_t n=0;n<evidence.size();++n){if(n)std::cout<<',';const auto& row=evidence[n];std::cout<<'['<<n<<','<<row.sentMs<<','<<(row.key?1:0)<<','<<row.keyRequests<<','<<(row.awaitingKey?1:0)<<','<<wssVideo[n]<<','<<wssAudio[n]<<']';}
    std::cout<<"],\"frameColumns\":[\"frame\",\"sendMs\",\"idr\",\"workerKeyRequests\",\"wssAwaitingKey\",\"retainedWssVideo\",\"retainedWssAudio\"]}\n"<<std::flush;
  }
  for(const auto& each:written)require(each.text,"Video or audio stayed on WebSocket after the switch");++passed;summary.stage=6;
  require(waitUntil([&]{return scene.client.videoCount()>=size_t(video.size()*9/10);},3000),"Under 90 % of the video arrived at 3 % loss");
  summary.stage=7;Sleep(200);const auto got=scene.client.received(true);
  for(const auto& [at,bytes]:got){const auto sent=video.find(at);require(sent!=video.end()&&sent->second==bytes,"Delivered video is not byte-exact");}++passed;
  const auto heard=scene.client.received(false);require(heard.size()>=audio.size()/2,"Audio did not arrive");for(const auto& [at,bytes]:heard){const auto sent=audio.find(at);require(sent!=audio.end()&&sent->second==bytes,"Delivered audio is not byte-exact");}++passed;
  // audio-stats-v1: what the UDP sender took and refused reaches the stats; none of it waited in the WebSocket queue.
  {scene.bridge.output(true,textBytes(R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":19000})"));Json send;
    for(const auto& each:scene.drain())if(each.text){const auto parsed=JsonParser::parse(std::string(each.bytes.begin(),each.bytes.end()));if(parsed.get("type").string()=="stream.stats"){send=parsed.get("audioSend");summary.originalStats=parsed;summary.originalStatsObserved=true;}}
    require(send.get("sent").number(-1)+send.get("dropped").number(-1)==double(audio.size())&&send.get("sent").number(-1)>=double(audio.size()*9/10)&&std::holds_alternative<std::nullptr_t>(send.get("queuePackets").value),"The UDP path's audio did not reach the stats");++passed;}
  const auto repairs=scene.stat("udpRetransmits");require(scene.relay.dropped>=100&&repairs>=10,"Loss was not injected, or nothing was repaired");require(scene.stat("udpPath")==1&&scene.stat("udpFallbacks")==0,"Stats do not show UDP");++passed;
  std::cout<<"{\"videoSent\":"<<video.size()<<",\"videoDelivered\":"<<got.size()<<",\"audioSent\":"<<audio.size()<<",\"audioDelivered\":"<<heard.size()<<",\"dropped\":"<<scene.relay.dropped.load()<<",\"retransmits\":"<<repairs<<",\"datagrams\":"<<scene.stat("udpDatagrams")<<",\"forcedIdrs\":"<<scene.bridge.keyFrames.load()<<",\"sentMs\":"<<sentMs<<"}\n";
  // Cursor records and text stay on the channel while video is on UDP.
  scene.bridge.output(false,media(3,false,pts+5,64));const auto cursor=scene.drain();require(cursor.size()==1&&cursor[0].bytes==media(3,false,pts+5,64),"A cursor record left WebSocket");++passed;
  // Hostile input through the relay's own address moves nothing, and the sealed control proves the route.
  {const int keys=scene.bridge.keyFrames;Bytes forged(core::headerBytes+core::keyframeBytes+core::tagBytes);forged[0]=3;std::copy(scene.clientGrant.id.begin(),scene.clientGrant.id.end(),forged.begin()+1);core::put64(&forged[9],1ull<<40);forged[core::headerBytes]=core::kindKeyframe;core::put16(&forged[core::headerBytes+2],1);
    Sleep(600);scene.relay.toHost(forged);Sleep(100);require(scene.bridge.keyFrames==keys,"A forged KEYFRAME forced an IDR");
    scene.client.sendKeyFrameRequest();require(waitUntil([&]{return scene.bridge.keyFrames>keys;},1000),"A sealed KEYFRAME did not reach the worker path");++passed;}
  // A still screen is not silence, on real timers either.
  Sleep(2500);require(scene.stat("udpPath")==1&&scene.stat("udpFallbacks")==0,"An idle screen read as a failed path");++passed;
  // The path dies. No session ends: video returns to WebSocket at a forced IDR.
  scene.relay.severed=true;const int keys=scene.bridge.keyFrames;const auto cut=GetTickCount64();uint64_t back=0;
  while(GetTickCount64()-cut<3000&&!back){pts+=16667;const auto bytes=media(1,false,pts,30000);scene.bridge.output(false,bytes);for(const auto& each:scene.drain())if(!each.text&&each.bytes==bytes)back=GetTickCount64()-cut;Sleep(16);}
  require(back>=800&&back<=2000,"The fallback did not come about 1 s after the silence");require(scene.bridge.keyFrames>keys,"The fallback forced no IDR");++passed;
  pts+=16667;const auto idr=media(1,true,pts,80000);scene.bridge.output(false,idr);const auto after=scene.drain();require(after.size()==1&&after[0].bytes==idr,"The forced IDR did not go on WebSocket");
  auto sound=media(2,false,pts+1,1920);const auto copy=sound;scene.bridge.output(false,std::move(sound));const auto audible=scene.drain();require(audible.size()==1&&audible[0].bytes==copy,"Audio stayed on the dead path");
  require(scene.stat("udpPath")==0&&scene.stat("udpFallbacks")==1,"Stats missed the fallback");
  require(!scene.agent.publicationTestClosed()&&scene.bridge.stops==1,"UDP failing touched the session");++passed;
  std::cout<<"{\"fallbackMs\":"<<back<<"}\n";
  const auto leg=scene.agent.publicationTestUdpLeg();scene.agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\""+testSession+"\"}");require(!scene.agent.publicationTestUdpLeg()&&leg->waitClosed(2000),"The session's end left the leg running");++passed;summary.stage=8;
}
// The grant lost the race: the leg never saw the first IDR, so it gets one on WebSocket.
void lateLeg(){
  Winsock winsock;Scene scene;scene.firstIdr(1000);scene.grant();scene.armed();require(scene.bridge.keyFrames==1,"The first-IDR request repeated");
  const auto idr=media(1,true,2000,3000);scene.bridge.output(false,idr);const auto wire=scene.drain();require(wire.size()==1&&wire[0].bytes==idr,"An unseen first IDR left WebSocket");
  require(waitUntil([&]{return scene.bridge.keyFrames==2;},1000),"The switch was never armed");
  scene.bridge.output(false,media(1,true,3000,3000));require(scene.drain().empty()&&waitUntil([&]{return scene.client.videoCount()==1;},1000),"The second key frame did not take UDP");++passed;
}
// A second grant replaces a leg that carries the stream: the Broker sends one per session,
// but if one came, the client must not be left waiting on the old leg.
void replacedLeg(){
  Winsock winsock;Scene scene;scene.info();scene.grant();scene.bridge.output(false,media(1,true,1000,3000));scene.drain();scene.armed();
  scene.bridge.output(false,media(1,true,2000,3000));require(scene.drain().empty()&&waitUntil([&]{return scene.client.videoCount()==1;},1000),"The key frame did not take UDP");
  const int keys=scene.bridge.keyFrames;const auto old=scene.agent.publicationTestUdpLeg();scene.grant();require(old->waitClosed(2000)&&scene.bridge.keyFrames==keys+1,"A replaced leg forced no IDR");
  const auto next=media(1,false,3000,3000);scene.bridge.output(false,next);const auto wire=scene.drain();require(wire.size()==1&&wire[0].bytes==next,"Media stayed with the replaced leg");++passed;
}
void eligibility(){
  Winsock winsock;
  // Manual bitrate, all monitors and tracks never exchange keys.
  const std::string automatic=R"(,"bitrateMode":"auto","bitrate":20000000,"bitrateMax":50000000)";
  for(const auto& [start,reason]:std::initializer_list<std::pair<std::string,const char*>>{
    {startCommand(R"(,"bitrate":20000000)"),"A manual bitrate session exchanged keys"},
    {startCommand(automatic,"all"),"An all-monitors session exchanged keys"},
    {startCommand(automatic+R"(,"mediaProtocol":"tracks-v1")"),"A tracks session exchanged keys"}}){
    Scene scene(start);scene.firstIdr(1000);scene.bound();scene.agent.localControl(scene.keyMessage());require(!scene.keyReply(),reason);++passed;
  }
  {Scene scene;scene.firstIdr(1000);scene.bound();scene.agent.localControl(scene.keyMessage("00000000-0000-0000-0000-000000000002"));require(!scene.keyReply(),"Another session's key was answered");
    auto noSession=scene.keyMessage();noSession.erase(noSession.find(",\"sessionId\""));noSession+='}';scene.agent.localControl(noSession);require(!scene.keyReply(),"A key without its session was answered");
    scene.agent.localControl(scene.keyMessage());require(bool(scene.keyReply()),"The control key was not answered");++passed;}
  {Scene scene;scene.firstIdr(1000);scene.grant();Sleep(100);scene.agent.localControl(scene.keyMessage());require(!scene.keyReply(),"A key before any PING was answered");
    scene.client.bind(scene.relay.port());require(scene.client.ping(),"No PONG");scene.agent.localControl(scene.keyMessage());require(bool(scene.keyReply()),"A refused early key used up the grant");++passed;}
  {Scene scene;scene.firstIdr(1000);scene.agent.localControl(scene.keyMessage());require(!scene.keyReply(),"A key with no grant was answered");++passed;}
}
// Everything a stream's channel carries, written through the real WebSocket path.
std::vector<std::pair<int,Bytes>> wireOf(Scene& scene,const std::function<void()>& before){
  before();
  scene.info();const std::vector<Bytes> packets{media(1,true,1000,50000),media(1,false,1100,3000),media(2,false,1110,1920),media(3,false,1120,64),media(1,false,1200,4000)};
  for(const auto& packet:packets)scene.bridge.output(false,packet);
  scene.bridge.output(true,textBytes(R"({"type":"stream.stats","capturedFps":60,"encodedFps":60,"bitrateKbps":19000})"));
  scene.bridge.output(false,media(1,false,1300,5000));
  std::vector<std::pair<int,Bytes>> wire;
  require(scene.agent.publicationTestWrite([&](WINHTTP_WEB_SOCKET_BUFFER_TYPE type,const uint8_t* data,DWORD size){wire.push_back({int(type),Bytes(data,data+size)});return DWORD(NO_ERROR);}),"The write path failed");
  require(!scene.agent.publicationTestClosed()&&scene.bridge.stops==1,"The channel suffered");return wire;
}
void noExchangeIsByteIdentical(){
  Winsock winsock;
  for(const auto& start:{startCommand(),startCommand(R"(,"bitrate":20000000)")}){
    for(const char* marker:{"",",\"keys\":true"}){
    Scene plain(start),probed(start);
    auto without=wireOf(plain,[]{}),with=wireOf(probed,[&]{probed.grant(marker);probed.join();Sleep(300);});
    // audio-stats-v1: the stats' audio send-queue wait is a measurement (us), so
    // it is compared apart: present with the one audio packet in both.
    for(auto* wire:{&without,&with})for(auto& [type,bytes]:*wire){
      if(type!=int(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE))continue;
      std::string text(bytes.begin(),bytes.end());const auto at=text.find(",\"audioSend\":{");if(at==std::string::npos)continue;
      require(text.find(",\"audioSend\":{\"sent\":1,\"dropped\":0,\"queuePackets\":1,",at)==at,"The stats lost their audio send window");
      text.erase(at,text.find('}',at)+1-at);bytes.assign(text.begin(),text.end());
    }
    // A real leg, bound and answering PINGs, with no key: identical frame for frame.
    require(without==with,"A probe-only session changed what the channel writes");
    Bytes all;for(const auto& [type,bytes]:with)if(type==int(WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)||type==int(WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE))all.insert(all.end(),bytes.begin(),bytes.end());
    // #98: the audio packet leaves before the video queued ahead of it, never before the cursor record (control).
    Bytes expected;for(const auto& packet:{media(2,false,1110,1920),media(1,true,1000,50000),media(1,false,1100,3000),media(3,false,1120,64),media(1,false,1200,4000),media(1,false,1300,5000)})expected.insert(expected.end(),packet.begin(),packet.end());
    require(all==expected,"The channel's media differs from what the worker produced");
    std::string text;for(const auto& [type,bytes]:with)if(type==int(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE))text.append(bytes.begin(),bytes.end());
    require(text.find("udp")==std::string::npos,"A probe-only session's text mentions UDP");++passed;
    }
  }
}
// The client speaks first, and only the Host's reply may follow: a released Broker drops a
// Host over an unknown udp.key. Not a grant, its marker, a passed probe or a refused key
// draws one, and one client key draws exactly one.
void hostOnlyAnswers(){
  Winsock winsock;Scene scene;scene.firstIdr(1000);
  const auto replies=[&]{size_t count=0;for(const auto& each:scene.drain())if(each.text&&std::string(each.bytes.begin(),each.bytes.end()).find("udp.key")!=std::string::npos)++count;return count;};
  scene.grant(R"(,"keys":true)");require(replies()==0,"A grant drew a udp.key");
  scene.join();scene.info();scene.bridge.output(false,media(1,true,2000,3000));Sleep(1200);scene.bridge.output(false,media(1,false,2100,1000));require(replies()==0,"A passed probe drew a udp.key");++passed;
  const auto valid=scene.keyMessage();auto otherProtocol=valid,noKey=valid;otherProtocol.replace(otherProtocol.find("udp-media-v1"),12,"udp-media-v2");const auto at=noKey.find("publicKey\":\"")+12;noKey.replace(at,noKey.find('"',at)-at,"AAAA");
  scene.agent.localControl(otherProtocol);scene.agent.localControl(noKey);require(replies()==0,"A refused key drew a reply");++passed;
  scene.agent.localControl(valid);require(replies()==1,"A client key did not draw exactly one reply");scene.agent.localControl(valid);Sleep(300);require(replies()==0,"A second client key drew a second reply");++passed;
}
}

void directInputCopiesStayOwned(){
  Winsock winsock;Scene scene;scene.grant(R"(,"keys":true,"directProtocol":"udp-direct-v1")");
  const auto packet=[&](uint32_t sequence,const std::string& input,const std::string& session=testSession){return "{\"type\":\"input.direct\",\"protocol\":\"udp-direct-v1\",\"sequence\":"+std::to_string(sequence)+",\"epoch\":1,\"sessionId\":"+jsonQuote(session)+",\"input\":"+input+"}";};
  const auto down=R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false})";
  const auto up=R"({"type":"input","event":"key","vk":65,"scan":30,"down":false,"extended":false})";
  scene.agent.localControl(packet(1,down));scene.agent.localControl(packet(1,down));scene.agent.localControl(packet(2,R"({"type":"input.fence"})"));scene.agent.localControl(packet(3,up));
  require(waitUntil([&]{return scene.bridge.controls().size()==2;},1000),"WSS copy failed to inject once or fence injected an action");
  const auto controls=scene.bridge.controls();require(controls[0].get("down").boolean()&&!controls[1].get("down").boolean(),"Key order changed");
  scene.agent.localControl(packet(4,down,"retired-session"));scene.agent.localControl(R"({"type":"input.reset"})");
  require(scene.bridge.controls().size()==3,"A stale owner crossed the reset barrier");
  scene.agent.localControl("{\"type\":\"session.stop\",\"sessionId\":"+jsonQuote(testSession)+"}");scene.agent.localControl(packet(5,down));Sleep(20);
  require(scene.bridge.controls().size()==3,"A stopped session injected");++passed;
}
int main(){
  try{
    // hostAction 271's fields, on the Host's own list (the Broker's is host-diagnostics.mjs).
    require(hostLogEvent("hostAction")&&hostLogField("action")&&hostLogField("stage")&&hostLogField("error")&&hostLogField("ok")&&!hostLogBoolean("stage")&&!hostLogBoolean("error"),"hostAction 271 would lose a field");
    for(const auto& scenario:{directInputCopiesStayOwned,noExchangeIsByteIdentical,hostOnlyAnswers,eligibility,lateLeg,replacedLeg,mediaMoves})scenario();
    std::cout<<"{\"kind\":\"udp-media-loopback\",\"passed\":"<<passed<<",\"network\":\"127.0.0.1 only\"}\n";return 0;
  }catch(const std::exception& error){std::cerr<<"UDP media loopback check failed: "<<error.what()<<"\n";return 1;}
}
