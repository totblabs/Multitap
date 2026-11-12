#include "service_worker_drain.hpp"
#include "signin_bridge.hpp"
#include "signin_bridge_wire.hpp"
#include "signin_eligibility_history.hpp"
#include "signin_stream_preference.hpp"
#include "signin_broker_preparation_authority.hpp"
#include "signin_restoration_authority.hpp"
#include "signin_worker_entry.hpp"
#include "signin_original_file.hpp"
#include "signin_owned_release.hpp"
#include "native_display_service.hpp"
#include "broker_status.hpp"
#include "pipe_packet.hpp"
#include "operational_log.hpp"
#include <bcrypt.h>
#include <thread>
#include <deque>
#include <condition_variable>
#include <charconv>
#include <fstream>

namespace host::signin::broker {
namespace {
[[noreturn]]void refused(const char* reason){throw std::runtime_error(reason);}
template<size_t N>std::array<uint8_t,N> random(){std::array<uint8_t,N> out{};if(BCryptGenRandom(nullptr,out.data(),DWORD(out.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)refused("signin_entropy_unavailable");return out;}
template<size_t N>std::string hex(const std::array<uint8_t,N>& data){constexpr char digits[]="0123456789abcdef";std::string out;for(auto b:data){out+=digits[b>>4];out+=digits[b&15];}return out;}
template<size_t N>std::array<uint8_t,N> unhex(std::string text){std::array<uint8_t,N> out{};if(text.size()!=N*2)refused("invalid_signin_request");auto digit=[](char c)->uint8_t{if(c>='0'&&c<='9')return uint8_t(c-'0');if(c>='a'&&c<='f')return uint8_t(c-'a'+10);refused("invalid_signin_request");};for(size_t i=0;i<N;i++)out[i]=uint8_t((digit(text[i*2])<<4)|digit(text[i*2+1]));if(!nonzero(out))refused("invalid_signin_request");return out;}
uint32_t integer(const Json& x,uint32_t minimum,uint32_t maximum){const auto* n=std::get_if<double>(&x.value);if(!n||!std::isfinite(*n)||*n<minimum||*n>maximum||std::floor(*n)!=*n)refused("invalid_signin_request");return uint32_t(*n);}
StreamId streamId(std::string text){if(!sessionStopUuid(text))refused("invalid_signin_request");text.erase(std::remove(text.begin(),text.end(),'-'),text.end());return unhex<16>(text);}
native::Handle duplicate(HANDLE source){HANDLE copy=nullptr;if(!source||!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&copy,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))refused("signin_process_unavailable");return native::Handle(copy);}
bool exited(HANDLE process,Process expected){try{return process&&native::processIdentity(process)==expected&&WaitForSingleObject(process,0)==WAIT_OBJECT_0;}catch(...){return false;}}
// One incremental reader for each private pipe, with its OVERLAPPED storage
// retained until completion/cancellation. Never discard a partially read frame.
class Reader {
 public:
  explicit Reader(HANDLE pipe):pipe_(pipe),event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)){if(!event_)refused("signin_pipe_unavailable");}
  ~Reader(){if(pending_){DWORD n=0;CancelIoEx(pipe_,&overlap_);(void)GetOverlappedResult(pipe_,&overlap_,&n,TRUE);}}
  enum class Read{waiting,packet,failed};
  Read next(bool& text,std::vector<uint8_t>& value){
    for(unsigned pass=0;pass<4;pass++){
      if(pending_){DWORD n=0;if(!GetOverlappedResult(pipe_,&overlap_,&n,FALSE)){if(GetLastError()==ERROR_IO_INCOMPLETE)return Read::waiting;pending_=false;return Read::failed;}pending_=false;if(!append(n))return Read::failed;}
      if(buffer_.size()>=4){const uint32_t n=uint32_t(buffer_[0])|(uint32_t(buffer_[1])<<8)|(uint32_t(buffer_[2])<<16)|(uint32_t(buffer_[3])<<24);if(n<2||n>4*1024*1024+1)return Read::failed;if(buffer_.size()>=size_t(n)+4){if(buffer_[4]>1)return Read::failed;text=buffer_[4]==0;value.assign(buffer_.begin()+5,buffer_.begin()+4+n);buffer_.erase(buffer_.begin(),buffer_.begin()+4+n);return Read::packet;}}
      DWORD available=0;if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr))return Read::failed;if(!available)return Read::waiting;
      overlap_={};overlap_.hEvent=event_.get();ResetEvent(event_.get());DWORD n=0;
      if(!ReadFile(pipe_,chunk_.data(),std::min<DWORD>(available,DWORD(chunk_.size())),&n,&overlap_)){if(GetLastError()!=ERROR_IO_PENDING)return Read::failed;pending_=true;return Read::waiting;}
      if(!append(n))return Read::failed;
    }return Read::waiting;
  }
 private:
  bool append(DWORD n){if(!n||n>chunk_.size()||buffer_.size()+n>4*1024*1024+65541)return false;buffer_.insert(buffer_.end(),chunk_.begin(),chunk_.begin()+n);return true;}
  HANDLE pipe_;native::Handle event_;OVERLAPPED overlap_{};bool pending_=false;std::array<uint8_t,65536> chunk_{};std::vector<uint8_t> buffer_;
};
struct ImagePin {native::Handle file;Nonce digest{};};
// SHA-256 of the whole service image. entry::payloadDigest is the encoded
// frame digest and refuses anything over one frame (4 MiB); the 8 MiB
// MultitapHost.exe fails it, which would end every sign-in start at once
// (hostAction 294 stage 1). Same algorithm as the
// worker's own pinnedOwnImage check, which allows 128 MiB.
Nonce imageDigest(std::span<const uint8_t> bytes){
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;Nonce result{};
  struct Close{BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Close(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}}close{algorithm,hash};
  if(bytes.empty()||BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0||
     BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)<0||BCryptFinishHash(hash,result.data(),ULONG(result.size()),0)<0)refused("signin_service_image_unavailable");
  return result;
}
ImagePin imagePin(){
  const auto image=executablePath();if(!trustedMachineFile(image)||!brokerSystemProcess(GetCurrentProcess()))refused("signin_service_unavailable");
  ImagePin result;result.file=native::Handle(CreateFileW(image.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};if(!result.file||!GetFileInformationByHandle(result.file.get(),&info)||info.nNumberOfLinks!=1||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!GetFileSizeEx(result.file.get(),&size)||size.QuadPart<=0||size.QuadPart>128ll*1024*1024)refused("signin_service_image_unavailable");
  std::vector<uint8_t> bytes(size_t(size.QuadPart));DWORD read=0;if(!ReadFile(result.file.get(),bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())refused("signin_service_image_unavailable");result.digest=imageDigest(bytes);return result;
}
bool h264Idr(std::span<const uint8_t> bytes){bool sps=false,pps=false,idr=false;for(size_t i=0;i+4<bytes.size();i++){size_t n=0;if(bytes[i]==0&&bytes[i+1]==0&&bytes[i+2]==1)n=3;else if(bytes[i]==0&&bytes[i+1]==0&&bytes[i+2]==0&&bytes[i+3]==1)n=4;if(n){const auto type=bytes[i+n]&31;sps|=type==7;pps|=type==8;idr|=type==5;i+=n;}}return sps&&pps&&idr;}
}

struct SigninBridge::Impl {
  struct Session;
  NativeDisplayService& display;HANDLE serviceStop;std::string instance=host::utf8(host::randomName());
  std::mutex lifetimeMutex;std::atomic<bool> permanentlyClosing{false};
  std::optional<service_workers::CleanupPermit> cleanupOwner;
  mutable std::mutex mutex;std::shared_ptr<Session> session;uint64_t scopeGeneration=1;std::atomic<uint32_t> eligibility{1};
  Observation last;bool lastEnabled=false;DiscoveryPublication discovery;EligibilityHistory history;
  HINTERNET mainSocket=nullptr;std::atomic<uint64_t> connectionGeneration{0};std::atomic<bool> negotiated{false};
  int64_t lastDiagnostic=-1;
  // A user stream that ended without its restore (Windows signed out inside
  // it) leaves display-v2.state. Only that session's worker
  // could restore it, so every sign-in start refused it (signin_restore_pending)
  // until a user signed in again. With nobody signed in, the service restores
  // it once from the console with the sealed restorer, then offers sign-in.
  std::thread leftoverThread;std::atomic<bool> leftoverRunning{false};std::atomic<uint64_t> leftoverRetryAt{0};
  void restoreLeftover();
  Impl(NativeDisplayService& d,HANDLE stop):display(d),serviceStop(stop){cleanupOwner.emplace(service_workers::admitCurrentCleanupOwner());}
  Observation observe(std::string* leaseIdentity=nullptr,std::string* configurationIdentity=nullptr){
    if(leaseIdentity)leaseIdentity->clear();if(configurationIdentity)configurationIdentity->clear();
    const auto facts=native::observeConsole(WTSGetActiveConsoleSessionId());Observation result;
    result.service=native::processIdentity(GetCurrentProcess());result.system=brokerSystemProcess(GetCurrentProcess());result.console=facts.session;result.at=facts.observedAt;result.consoleProtocol=facts.valid&&facts.consoleProtocol;result.windows=facts.windows;result.sid=facts.signedInSid;
    try{const auto snapshot=JsonParser::parse(display.viewportSnapshot());result.ownedSingleOutput=display.displayManagerV2()&&snapshot.get("servicePid").number()==result.service.pid&&nativeDisplayTargets(snapshot).size()==1;if(leaseIdentity&&result.ownedSingleOutput)*leaseIdentity=nativeDisplayLeaseKey(snapshot);}catch(...){}
    const auto settings=readPublicConfig();const bool enabled=signinStreamingEnabled(settings);
    if(configurationIdentity){Json selected;selected.value=Json::Object{};auto& fields=std::get<Json::Object>(selected.value);
      if(const auto* all=std::get_if<Json::Object>(&settings.value))for(const char* key:{"origin","desktopId","httpConsentOrigin","displayManager","signinStreamingEnabled","videoPixelBudget"})if(const auto found=all->find(key);found!=all->end())fields.emplace(found->first,found->second);
      const auto bytes=jsonObject(selected);*configurationIdentity=hex(entry::payloadDigest({reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()}));
    }
    std::lock_guard lock(mutex);
    if(last.console!=result.console||last.windows!=result.windows||last.sid!=result.sid){if(scopeGeneration==UINT64_MAX||eligibility==UINT32_MAX)refused("signin_generation_exhausted");++scopeGeneration;++eligibility;}
    if(lastEnabled!=enabled){if(eligibility==UINT32_MAX)refused("signin_generation_exhausted");++eligibility;lastEnabled=enabled;}
    result.generation=scopeGeneration;last=result;return result;
  }
  EligibilityScope eligibilityScope(){EligibilityScope scope;scope.native=observe(&scope.lease,&scope.configuration);scope.connectionGeneration=connectionGeneration.load();return scope;}
};

void SigninBridge::Impl::restoreLeftover(){
  // hostAction 295: stage 1 image, 2 original, 3 lease, 40 restore authority,
  // 41 restorer launch (cause = native step, win32 = error), 42 bind, 43 pipe,
  // 5 restore, 6 retire; ok=true once the original is restored and removed.
  // items/width/height: the saved original's owned outputs and restore size.
  int64_t stage=1,cause=0,win32=0,items=-1,width=0,height=0,sizeOk=0,adoptable=0;std::string text;bool held=false;
  try{
    const auto pin=imagePin();
    stage=2;const auto path=machineDirectory()/L"private"/L"display-v2.state";
    std::string bytes;{std::ifstream in(path,std::ios::binary);if(!in)refused("signin_restore_pending");bytes.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}
    if(bytes.empty()||bytes.size()>65536||!trustedMachineFile(path))refused("signin_restore_pending");
    try{const auto saved=parseDisplayV2State(bytes);items=int64_t(saved.owned.size());if(!saved.owned.empty()){width=saved.owned.front().width;height=saved.owned.front().height;
      for(const auto& p:saved.ccd.paths)if(displayV2Active(p)&&nativeSameTarget(p,saved.owned.front())){const auto& s=nativeCcdSource(saved.ccd,p);width=s.width;height=s.height;break;}}}catch(...){items=-2;}
    preparation::OriginalFile original(path);
    const auto facts=native::observeConsole(WTSGetActiveConsoleSessionId());
    if(!facts.valid||facts.windows!=WindowsState::noUser||!facts.session||facts.session==UINT32_MAX)refused("signin_console_changed");
    stage=3;const StreamId stream=random<16>();text=PreparedServiceAuthority::streamText(stream);
    const auto snapshot=JsonParser::parse(display.viewportSnapshot());
    if(!display.reservePreparation(text,nativeDisplayLeaseKey(snapshot)))refused("display_busy");held=true;
    const auto lease=display.preparationCleanupLease(text,snapshot.get("deviceInstanceId").string(),integer(snapshot.get("generation"),1,UINT32_MAX));if(!lease)refused("signin_display_unavailable");
    restoration::RestorationAdmission a;a.service=native::processIdentity(GetCurrentProcess());a.stream=stream;a.lease={lease->instance,lease->generation};a.operation=random<32>();
    a.installedImageSha256=pin.digest;a.consoleSession=facts.session;a.originalSha256=nativeBaselineDigest(bytes);a.issuedAt=GetTickCount64();a.expiresAt=a.issuedAt+60000;
    if(!cleanupOwner)refused("signin_restore_pending");
    // expected = restore size admissible, fallback = original adoptable (the
    // two native checks inside the authority that can refuse it).
    sizeOk=ownedOutputSize(uint64_t(width),uint64_t(height))?1:0;{preparation::OriginalFile probe(path);adoptable=probe.adopt(a.originalSha256)?1:0;}
    stage=40;restoration::ServiceAuthority authority(display,a,{},original);restoration::OwnedRestorationChild child;
    stage=41;{const auto launched=restoration::launch(a,authority,child,&*cleanupOwner);if(!launched){cause=int64_t(launched.step);win32=int64_t(launched.error);refused("signin_restore_pending");}}
    stage=42;if(!authority.bindChild(child.process()))refused("signin_restore_pending");
    stage=43;if(!child.connect(2000))refused("signin_restore_pending");
    std::atomic<bool> stopped{false};std::mutex writing;
    const auto send=[&](const std::string& value){return pipePacket(child.pipe(),true,{value.begin(),value.end()},stopped,writing);};
    auto reader=std::make_unique<Reader>(child.pipe());if(!send(restoration::initPacket(a,child.binding(),child.delegatedServiceQuery())))refused("signin_restore_pending");
    stage=5;std::optional<restoration::Receipt> receipt;uint64_t serial=0;
    while(!receipt){
      if(GetTickCount64()>=a.expiresAt||WaitForSingleObject(serviceStop,0)==WAIT_OBJECT_0)refused("signin_restore_pending");
      bool isText=false;std::vector<uint8_t> packet;const auto result=reader->next(isText,packet);if(result==Reader::Read::failed)refused("signin_restore_pending");
      if(result==Reader::Read::packet){if(!isText)refused("signin_restore_pending");const auto value=JsonParser::parse(std::string(packet.begin(),packet.end()));
        if(value.get("type").string()=="signin.restore.finished-v1")receipt=restoration::parseReceipt(value);
        else if(!send(restoration::answer(value,a,child.binding(),authority,serial)))refused("signin_restore_pending");}
      (void)display.refreshPreparationCleanupLease(text);Sleep(1);
    }
    reader.reset();(void)child.stopAndWait(2000);
    stage=6;bool finished=false;for(int i=0;i<50&&!(finished=authority.finish(*receipt));i++)Sleep(40);
    if(!finished)refused("signin_restore_pending");
    if(display.releasePreparation(text))held=false;
    recordHostEvent("hostAction",{{"action",295},{"ok",true},{"stage",stage},{"items",items},{"width",width},{"height",height},{"expected",sizeOk},{"fallback",adoptable}});
  }catch(...){recordHostEvent("hostAction",{{"action",295},{"ok",false},{"stage",stage},{"cause",cause},{"win32",win32},{"items",items},{"width",width},{"height",height},{"expected",sizeOk},{"fallback",adoptable}});}
  // Nothing else may take the display while this reservation is still ours.
  if(held)for(int i=0;i<100&&!display.releasePreparation(text);i++){(void)display.refreshPreparationCleanupLease(text);Sleep(100);}
  leftoverRetryAt=GetTickCount64()+30000;leftoverRunning=false;
}

struct SigninBridge::Impl::Session:std::enable_shared_from_this<Session> {
  Impl& owner;const std::string id;const Json request;OutputLease output;DesktopFactory factory;const uint64_t connection;const std::string configuration,initialLease;
  Admission admission;PhaseFence fence;Observation observed;std::atomic<bool> authorized{true},done{false};std::thread thread;
  struct Queued {bool text=false;std::vector<uint8_t> bytes;std::optional<DesktopPacketSource> source;};
  std::mutex queueMutex;std::deque<Queued> queue;size_t queuedBytes=0;std::atomic<uint32_t> desktopSourceGeneration{0};
  std::atomic<bool> closing{false};bool stopQueued=false,releasePending=false;
  native::Handle image;Nonce imageSha{};std::mutex writing;std::atomic<bool> ioStopped{false};
  std::unique_ptr<preparation::OriginalFile> original;bool reserved=false;preparation::PreparationAdmission preparationAdmission;
  std::unique_ptr<preparation::BrokerAuthority> preparationAuthority;std::unique_ptr<preparation::OwnedPreparationChild> preparer;std::unique_ptr<Reader> prepReader;
  std::optional<preparation::Receipt> prepared;uint64_t prepareSerial=0;
  std::unique_ptr<native::OwnedWorker> secure;std::unique_ptr<Reader> secureReader;channel::Init init;std::optional<entry::FirstFrame> secureFirst;std::optional<bool> secureCleanup;
  std::optional<DesktopLane> desktop;native::Handle desktopWorker;Process desktopIdentity{};Json desktopInfo;
  PhaseId firstFrame{};bool decoded=false,published=false;uint32_t authorityRevision=0;uint64_t nextAuthority=0;
  struct FrameEvidence {uint64_t sequence=0,pts=0;Nonce digest{};uint32_t width=0,height=0;};
  std::optional<FrameEvidence> actualFirstFrame;std::deque<Json> challenges;
  std::unique_ptr<restoration::OwnedRestorationChild> restorer;std::unique_ptr<restoration::ServiceAuthority> restoreAuthority;std::unique_ptr<Reader> restoreReader;
  std::optional<restoration::Receipt> restored;restoration::RestorationAdmission restoreAdmission;uint64_t restoreSerial=0;
  Session(Impl& own,std::string name,Json r,AgentBridge::Send output,DesktopFactory desktopFactory,Admission a,Observation o,uint64_t connectionOwner,std::string configurationOwner,std::string initialLeaseOwner):owner(own),id(std::move(name)),request(std::move(r)),output(std::move(output)),factory(std::move(desktopFactory)),connection(connectionOwner),configuration(std::move(configurationOwner)),initialLease(std::move(initialLeaseOwner)),admission(std::move(a)),observed(std::move(o)){if(!fence.begin(admission,observed))refused("invalid_signin_request");}
  bool auth()const{return authorized.load()&&connection&&owner.connectionGeneration.load()==connection&&owner.negotiated.load()&&WaitForSingleObject(owner.serviceStop,0)==WAIT_TIMEOUT&&signinStreamingEnabled(readPublicConfig());}
  bool current(){if(!auth())return false;std::string liveConfiguration;const auto live=owner.observe(nullptr,&liveConfiguration);if(liveConfiguration!=configuration||live.service!=observed.service||live.console!=observed.console||live.generation!=observed.generation)return false;return fresh(live,GetTickCount64())&&live.windows==observed.windows&&live.sid==observed.sid;}
  bool enqueueScoped(bool text,std::vector<uint8_t> bytes,std::optional<DesktopPacketSource> source={}){
    std::lock_guard lock(queueMutex);
    if(source&&desktopSourceGeneration.load()!=source->generation)return true;
    if(!authorized)return false;
    if(queue.size()>=128||bytes.size()>4*1024*1024||queuedBytes+bytes.size()>8*1024*1024){authorized=false;return false;}
    queuedBytes+=bytes.size();queue.push_back({text,std::move(bytes),source});return true;
  }
  bool enqueue(bool text,std::vector<uint8_t> bytes){return enqueueScoped(text,std::move(bytes));}

  void emit(bool text,std::vector<uint8_t> bytes){if(!authorized||!output.send(text,std::move(bytes)))refused("authorization_revoked");}
  void revoke()noexcept{desktopSourceGeneration=0;closing=true;authorized=false;}
  void cancel()noexcept{revoke();try{output.retire();}catch(...){}}
  void text(const std::string& value){if(!auth())refused("authorization_revoked");emit(true,{value.begin(),value.end()});}
  std::string publicBinding()const{return wire::binding(fence.admission(),fence.role());}
  void authority(bool withdraw=false){
    if(authorityRevision==UINT32_MAX){if(withdraw)return;refused("signin_generation_exhausted");}
    const auto value=wire::authority(id,owner.instance,++authorityRevision,fence.admission(),fence.role(),withdraw);
    if(withdraw){try{emit(true,{value.begin(),value.end()});}catch(...){}}else{if(!current())refused("signin_authority_lost");text(value);}nextAuthority=GetTickCount64()+500;
  }
  bool write(HANDLE pipe,const std::string& value){return pipePacket(pipe,true,{value.begin(),value.end()},ioStopped,writing);}
  bool preparerExited()const{return !preparer||(!preparer->process())||exited(preparer->process(),preparer->binding().worker);}
  // Actual preparation authority is distinct from local diagnostic authority.
  void prepare(bool initial=true){
    if(!current()||(initial&&GetTickCount64()>=admission.receivedAt+admission.validForMs))refused("signin_admission_expired");
    original=std::make_unique<preparation::OriginalFile>(machineDirectory()/L"private"/L"display-v2.state");if(!original->absent())refused("signin_restore_pending");
    const auto snapshot=JsonParser::parse(owner.display.viewportSnapshot());
    if(initial&&nativeDisplayLeaseKey(snapshot)!=initialLease)refused("signin_eligibility_changed");
    if(!owner.display.reservePreparation(id,initial?initialLease:nativeDisplayLeaseKey(snapshot)))refused("display_busy");reserved=true;
    const auto generation=integer(snapshot.get("generation"),1,UINT32_MAX);const auto lease=owner.display.preparationLease(id,snapshot.get("deviceInstanceId").string(),generation);if(!lease||lease->targets.size()!=1)refused("signin_display_unavailable");
    auto& a=preparationAdmission;a.purpose=preparation::Purpose::brokerStream;a.service=admission.service;a.operatorProcess=admission.service;a.stream=admission.stream;a.operation=random<32>();a.lease={lease->instance,lease->generation};a.installedImageSha256=imageSha;a.consoleSession=observed.console;
    a.width=integer(request.get("width"),320,8192);a.height=integer(request.get("height"),180,8192);a.issuedAt=GetTickCount64();a.expiresAt=a.issuedAt+60000;a.serviceInstance=admission.serviceInstance;a.grantRevision=admission.grantRevision;a.eligibilityRevision=admission.eligibilityRevision;a.brokerDeadline=initial?admission.receivedAt+admission.validForMs:a.issuedAt+1000; // Re-entry consumes the same still-current broker owner after native phase/exit/restoration proof.
    preparationAuthority=std::make_unique<preparation::BrokerAuthority>(owner.display,a,*original,[this](const auto&){return current();});
    preparer=std::make_unique<preparation::OwnedPreparationChild>();if(!preparation::launch(a,*preparationAuthority,*preparer)||!preparationAuthority->bindChild(preparer->process())||!preparer->connect(2000))refused("signin_preparation_failed");
    prepReader=std::make_unique<Reader>(preparer->pipe());if(!write(preparer->pipe(),preparation::initPacket(a,preparer->binding(),preparer->delegatedServiceQuery())))refused("signin_preparation_failed");
    while(!prepared){if(!current()||GetTickCount64()>=a.expiresAt)refused("signin_preparation_timeout");bool isText=false;std::vector<uint8_t> bytes;const auto result=prepReader->next(isText,bytes);if(result==Reader::Read::failed)refused("signin_preparation_failed");if(result==Reader::Read::packet){if(!isText)refused("invalid_signin_info");const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="signin.prepare.finished-v1"){auto receipt=preparation::parseReceipt(value);const auto retained=preparationAuthority->retained();if(!retained||receipt.binding!=preparer->binding()||preparation::receiptPacket(*retained)!=preparation::receiptPacket(receipt)||receipt.outcome!=preparation::Outcome::prepared)refused("signin_preparation_failed");prepared=receipt;}else if(!write(preparer->pipe(),preparation::answer(value,a,preparer->binding(),*preparationAuthority,prepareSerial)))refused("signin_preparation_failed");}Sleep(1);}
    prepReader.reset();if(!preparer->stopAndWait(1000)||!preparerExited())refused("signin_preparer_exit_pending");
  }
  class CaptureAuthority final:public native::LeaseAuthority {
   public:explicit CaptureAuthority(Session& value):s(value){}
    bool current(const Binding& b,std::span<const native::OwnedTarget> targets)const override{return !s.stopQueued&&s.current()&&bound(b,targets);}
    bool currentForRelease(const Binding& b,std::span<const native::OwnedTarget> targets)const override{
      try{const auto live=s.owner.observe();return (s.stopQueued||s.current())&&fresh(live,GetTickCount64())&&live.service==s.observed.service&&live.console==s.observed.console&&live.generation==s.observed.generation&&live.windows==s.observed.windows&&live.sid==s.observed.sid&&bound(b,targets);}catch(...){return false;}
    }
   private:
    bool bound(const Binding& b,std::span<const native::OwnedTarget> targets)const{
      try{if(!s.secure||targets.size()!=1||!s.prepared||!s.original||!s.original->current()||nativeBaselineDigest(*s.original->current())!=hex(b.baseline)||b.worker!=s.secure->identity()||b.service!=s.admission.service||b.stream!=s.admission.stream||!channel::sameBinding(b,s.init.binding)||targets[0]!=s.init.target||!s.secure->process()||WaitForSingleObject(s.secure->process(),0)!=WAIT_TIMEOUT)return false;
        const auto lease=s.owner.display.preparationLease(s.id,b.lease.instance,b.lease.generation);return lease&&lease->service.pid==b.service.pid&&lease->service.created==b.service.created&&lease->targets.size()==1&&lease->targets[0].adapter.LowPart==targets[0].adapterLow&&lease->targets[0].adapter.HighPart==targets[0].adapterHigh&&lease->targets[0].target==targets[0].targetId&&lease->targets[0].width==targets[0].width&&lease->targets[0].height==targets[0].height;
      }catch(...){return false;}
    }
   private:Session& s;
  };
  std::unique_ptr<CaptureAuthority> captureAuthority;
  void launchSecure(){
    if(!prepared||!current()||!preparerExited())refused("signin_preparation_failed");
    const auto snapshot=JsonParser::parse(owner.display.viewportSnapshot());const auto lease=owner.display.preparationLease(id,preparationAdmission.lease.instance,integer(snapshot.get("generation"),1,UINT32_MAX));if(!lease)refused("signin_display_unavailable");
    Context context;context.stream=admission.stream;context.service=admission.service;context.serviceIsLocalSystem=true;context.lease={lease->instance,lease->generation};context.leaseOwned=true;context.baseline=unhex<32>(original->sha256());context.consoleSession=observed.console;context.workerGeneration=fence.admission().workerGeneration;context.mediaEpoch=fence.admission().mediaEpoch;context.ownedOutputMask=1;context.configuredSid=observed.sid;context.unknownAccountAllowed=observed.windows==WindowsState::noUser&&observed.sid.empty();context.nonce=random<32>();context.assigned=context.clientOptIn=context.clientTransitions=context.signinRuntimeQualified=true;
    secure=std::make_unique<native::OwnedWorker>();
    if(!native::launchWorker({context,observed.generation,imageSha},*secure)||!secure->connect(2000))refused("signin_worker_failed");
    init={context,secure->binding(),prepared->target,secure->delegatedServiceQuery()};captureAuthority=std::make_unique<CaptureAuthority>(*this);if(!captureAuthority->current(init.binding,{&init.target,1})||!fence.worker(secure->identity()))refused("signin_authority_lost");
    secureReader=std::make_unique<Reader>(secure->pipe());if(!write(secure->pipe(),channel::initPacket(init)))refused("signin_worker_failed");
    entry::Start start;start.transition=fence.admission().transition;start.firstFrame=random<16>();start.scopeGeneration=observed.generation;start.fps=integer(request.get("fps"),1,60);start.bitrate=integer(request.get("bitrate"),encoderMinimumBitrate,encoderMaximumBitrate);
    if(!write(secure->pipe(),entry::startPacket(init,start)))refused("signin_worker_failed");
  }
  void info(Json value,PhaseId frame){
    auto* object=std::get_if<Json::Object>(&value.value);if(!object)refused("invalid_signin_info");(*object)["epoch"].value=double(fence.admission().mediaEpoch);(*object)["sessionId"].value=id;
    authority();text(wire::info(id,fence.admission(),fence.role(),frame,value));
  }
  void securePacket(bool isText,std::vector<uint8_t> bytes){
    if(isText){const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="signin.worker.cleanup-v1"){if(secureCleanup.has_value())refused("invalid_signin_info");secureCleanup=entry::cleanupComplete(value,init);return;}}
    if(isText){const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="signin.worker.release-lease-v1"){if(!captureAuthority||!write(secure->pipe(),channel::answerReleaseLease(value,init.binding,{&init.target,1},*captureAuthority)))refused("signin_owned_input_release_pending");return;}}
    if(!captureAuthority||!captureAuthority->current(init.binding,{&init.target,1}))refused("signin_authority_lost");
    if(isText){const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="signin.worker.frame-v1"){
      secureFirst=entry::parseFirstFrame(value,init);firstFrame=secureFirst->firstFrame;if(!fence.firstFrame(secureFirst->sequence,firstFrame))refused("invalid_signin_info");
      const auto infoValue=JsonParser::parse("{\"type\":\"stream.info\",\"codec\":\"h264\",\"width\":"+std::to_string(init.target.width)+",\"height\":"+std::to_string(init.target.height)+",\"sourceWidth\":"+std::to_string(init.target.width)+",\"sourceHeight\":"+std::to_string(init.target.height)+",\"fpsTarget\":"+std::to_string(integer(request.get("fps"),1,60))+",\"audio\":{\"available\":false}}");info(infoValue,firstFrame);
      if(!write(secure->pipe(),entry::grantPacket(init,*secureFirst,entry::Grant::publish)))refused("signin_worker_failed");published=true;
    }else if(value.get("type").string()=="signin.worker.lease-v1"){if(!write(secure->pipe(),channel::answerLease(value,init.binding,{&init.target,1},*captureAuthority)))refused("signin_authority_lost");}
    else refused("invalid_signin_info");
    }else{if(!published||bytes.size()<12||bytes[0]!=1||bytes[2]!=uint8_t(fence.admission().mediaEpoch)||bytes[3]!=uint8_t(fence.admission().mediaEpoch>>8))refused("invalid_signin_media");
      if(!actualFirstFrame){uint64_t pts=0;for(unsigned k=0;k<8;k++)pts|=uint64_t(bytes[4+k])<<(k*8);const auto digest=entry::payloadDigest({bytes.data()+12,bytes.size()-12});if(!secureFirst||!(bytes[1]&1)||!h264Idr({bytes.data()+12,bytes.size()-12})||digest!=secureFirst->payloadSha256||pts!=secureFirst->ptsUs)refused("invalid_signin_frame");actualFirstFrame=FrameEvidence{secureFirst->sequence,pts,digest,init.target.width,init.target.height};}
      emit(false,std::move(bytes));}
  }
  void desktopPacket(bool isText,std::vector<uint8_t> bytes){
    if(!current()||!desktop)refused("signin_authority_lost");
    if(isText){const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="stream.info"){desktopInfo=value;return;}if(value.get("type").string()=="stream.unavailable"||value.get("type").string()=="session.unavailable")refused("desktop_capture_failed");return;}
    if(bytes.size()<12||bytes[0]!=1)return;
    if(!published){if(!(bytes[1]&1)||!h264Idr({bytes.data()+12,bytes.size()-12})||!std::holds_alternative<Json::Object>(desktopInfo.value))return;desktopWorker=desktop->workerQuery();if(!desktopWorker)refused("signin_worker_identity_unavailable");desktopIdentity=native::processIdentity(desktopWorker.get());if(!fence.worker(desktopIdentity))refused("signin_worker_identity_unavailable");firstFrame=random<16>();if(!fence.firstFrame(1,firstFrame))refused("invalid_signin_info");
      uint64_t pts=0;for(unsigned k=0;k<8;k++)pts|=uint64_t(bytes[4+k])<<(k*8);actualFirstFrame=FrameEvidence{1,pts,entry::payloadDigest({bytes.data()+12,bytes.size()-12}),integer(desktopInfo.get("sourceWidth"),16,8192),integer(desktopInfo.get("sourceHeight"),16,8192)};
      info(desktopInfo,firstFrame);published=true;}
    bytes[2]=uint8_t(fence.admission().mediaEpoch);bytes[3]=uint8_t(fence.admission().mediaEpoch>>8);emit(false,std::move(bytes));
  }
  bool match(const Json& value){try{return value.get("sessionId").string()==id&&value.get("protocol").string()=="signin-stream-v1"&&value.get("mediaScope").get("kind").string()=="composite"&&fence.matches(integer(value.get("workerGeneration"),1,UINT32_MAX),uint16_t(integer(value.get("mediaScope").get("epoch"),1,65535)),unhex<16>(value.get("transitionId").string()));}catch(...){return false;}}
  void controls(){
    for(;;){Queued item;{std::lock_guard lock(queueMutex);if(queue.empty())break;item=std::move(queue.front());queue.pop_front();queuedBytes-=item.bytes.size();}
      if(item.source){
        if(fence.role()!=Role::user||!desktop||!item.source->current(fence.admission()))continue;
        desktopPacket(item.text,std::move(item.bytes));continue;
      }
      if(!item.text)continue;
      const auto value=JsonParser::parse(std::string(item.bytes.begin(),item.bytes.end()));const auto type=value.get("type").string();
      if(type=="host.signin.authority.query"){
        if(value.get("protocol").string()=="signin-service-v1"&&value.get("sessionId").string()==id&&jsonObject(value.get("binding"))==jsonObject(JsonParser::parse(publicBinding()))&&challenges.size()<8){(void)unhex<16>(value.get("challengeId").string());challenges.push_back(value);}continue;
      }
      if(!match(value)||!current())continue;
      if(type=="stream.phase.decoded"){
        const auto frame=unhex<16>(value.get("firstFrameId").string());if(!fence.decoded(fence.admission().workerGeneration,fence.admission().mediaEpoch,fence.admission().transition,frame))refused("invalid_signin_ack");decoded=true;
        if(fence.role()==Role::signin&&(!secureFirst||!write(secure->pipe(),entry::grantPacket(init,*secureFirst,entry::Grant::decoded))))refused("signin_worker_failed");
      }else if(type=="input.phase"&&fence.input(fence.admission().workerGeneration,fence.admission().mediaEpoch,fence.admission().transition,owner.observe(),GetTickCount64())){
        if(fence.role()==Role::signin){if(!write(secure->pipe(),entry::inputPacket(init,value)))refused("signin_worker_failed");}
        else if(desktop)desktop->bridge->control(value.get("input"));
      }
    }
  }
  void answerChallenges(){
    for(auto i=challenges.begin();i!=challenges.end();){
      if(!current()||jsonObject(i->get("binding"))!=jsonObject(JsonParser::parse(publicBinding()))){i=challenges.erase(i);continue;}
      const auto operation=i->get("operation").string();if(operation=="first-frame"&&!actualFirstFrame){++i;continue;}
      // Keep the exact already-published lease revision for this challenge;
      // do not mint a different revision between the reply and frame receipt.
      const auto challenge=i->get("challengeId").string();text("{\"type\":\"host.signin.authority.result\",\"protocol\":\"signin-service-v1\",\"sessionId\":"+jsonQuote(id)+",\"binding\":"+publicBinding()+",\"challengeId\":"+jsonQuote(challenge)+",\"serviceInstance\":"+jsonQuote(owner.instance)+",\"revision\":"+std::to_string(authorityRevision)+",\"state\":\"current\"}");
      if(operation=="first-frame"){
        const auto& f=*actualFirstFrame;text("{\"type\":\"host.signin.first-frame\",\"protocol\":\"signin-service-v1\",\"sessionId\":"+jsonQuote(id)+",\"binding\":"+publicBinding()+",\"challengeId\":"+jsonQuote(challenge)+",\"serviceInstance\":"+jsonQuote(owner.instance)+",\"revision\":"+std::to_string(authorityRevision)+",\"firstFrameId\":"+jsonQuote(hex(firstFrame))+",\"sourceSequence\":"+jsonQuote(std::to_string(f.sequence))+",\"ptsUs\":"+jsonQuote(std::to_string(f.pts))+",\"payloadSHA256\":"+jsonQuote(hex(f.digest))+",\"width\":"+std::to_string(f.width)+",\"height\":"+std::to_string(f.height)+'}');
      }i=challenges.erase(i);
    }
  }
  void waitSecureExit(){
    if(!secure||!secure->process())return;
    // Keep the exact authenticated reader/authority alive through owned reset.
    // No forward lease is granted after this point, including on handoff.
    stopQueued=true;if(secureReader&&captureAuthority)(void)write(secure->pipe(),entry::stopPacket(init));
    const auto deadline=GetTickCount64()+5000;
    while(secureReader&&GetTickCount64()<deadline&&(!exited(secure->process(),secure->identity())||!secureCleanup.has_value())){
      bool isText=false;std::vector<uint8_t> bytes;const auto state=secureReader->next(isText,bytes);
      if(state==Reader::Read::failed)break;
      if(state==Reader::Read::packet&&isText){try{const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));const auto type=value.get("type").string();
        if(type=="signin.worker.cleanup-v1"){if(secureCleanup.has_value()){secureCleanup=false;break;}secureCleanup=entry::cleanupComplete(value,init);}
        else if(type=="signin.worker.release-lease-v1"){if(!write(secure->pipe(),channel::answerReleaseLease(value,init.binding,{&init.target,1},*captureAuthority)))break;}
        else if(type=="signin.worker.lease-v1"){if(!write(secure->pipe(),channel::answerLease(value,init.binding,{&init.target,1},*captureAuthority)))break;}
      }catch(...){secureCleanup=false;break;}}
      (void)owner.display.refreshPreparationCleanupLease(id);Sleep(1);
    }
    const bool graceful=exited(secure->process(),secure->identity());
    secureReader.reset();captureAuthority.reset();
    if(!graceful)(void)secure->stopAndWait(1000);
    while(!exited(secure->process(),secure->identity())){(void)owner.display.refreshPreparationCleanupLease(id);Sleep(25);}
    DWORD code=STILL_ACTIVE;const bool observedExit=GetExitCodeProcess(secure->process(),&code)!=FALSE;
    // A native scope handoff may exit with its documented nonzero cause AFTER
    // release() proved no owned records remain. The distinct pending code,
    // unknown exits and forced exits never prove that cleanup.
    const bool resetComplete=code==0||(code>=64+uint32_t(entry::Cause::handoff)&&code<=64+uint32_t(entry::Cause::input));
    releasePending=!graceful||!observedExit||!resetComplete||!secureCleanup.has_value()||!*secureCleanup;
  }
  bool restoreOnce(){
    if(!reserved)return true;if(preparer&&!preparerExited())return false;
    if(!original||!original->current()){if(original&&original->absent()){if(!owner.display.releasePreparation(id))return false;reserved=false;return true;}return false;}
    if(restorer&&!exited(restorer->process(),restorer->binding().worker))return false;
    if(restorer&&restored&&restoreAuthority&&(restoreAuthority->completed()||restoreAuthority->finish(*restored))){
      if(!owner.display.releasePreparation(id))return false;reserved=false;return true;
    }
    if(restorer)return false; // Existing unresolved attempt stays owned; never free its slot on timeout.
    const auto snapshot=JsonParser::parse(owner.display.viewportSnapshot());const auto lease=owner.display.preparationCleanupLease(id,preparationAdmission.lease.instance,integer(snapshot.get("generation"),1,UINT32_MAX));if(!lease)return false;
    auto& a=restoreAdmission;a.service=admission.service;a.stream=admission.stream;a.lease={lease->instance,lease->generation};a.operation=random<32>();a.installedImageSha256=imageSha;a.consoleSession=observed.console;a.originalSha256=original->sha256();a.issuedAt=GetTickCount64();a.expiresAt=a.issuedAt+60000;
    std::vector<HANDLE> handles;if(preparer&&preparer->process()){if(!preparerExited())return false;handles.push_back(preparer->process());a.predecessors.push_back(preparer->binding().worker);}if(secure&&secure->process()){if(!exited(secure->process(),secure->identity()))return false;handles.push_back(secure->process());a.predecessors.push_back(secure->identity());}
    if(!owner.cleanupOwner)refused("signin_restore_pending");
    restoreAuthority=std::make_unique<restoration::ServiceAuthority>(owner.display,a,handles,*original);restorer=std::make_unique<restoration::OwnedRestorationChild>();if(!restoration::launch(a,*restoreAuthority,*restorer,&*owner.cleanupOwner)||!restoreAuthority->bindChild(restorer->process())||!restorer->connect(2000))return false;
    restoreReader=std::make_unique<Reader>(restorer->pipe());return write(restorer->pipe(),restoration::initPacket(a,restorer->binding(),restorer->delegatedServiceQuery()));
  }
  void cleanupSecure(){
    prepReader.reset();if(preparer&&!preparerExited())(void)preparer->stopAndWait(1000);waitSecureExit();
    while(reserved){
      try{
        if(restorer&&restoreReader){bool isText=false;std::vector<uint8_t> bytes;const auto state=restoreReader->next(isText,bytes);if(state==Reader::Read::packet){if(!isText)refused("signin_restore_pending");const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="signin.restore.finished-v1"){restored=restoration::parseReceipt(value);restoreReader.reset();(void)restorer->stopAndWait(1000);}else if(!write(restorer->pipe(),restoration::answer(value,restoreAdmission,restorer->binding(),*restoreAuthority,restoreSerial)))refused("signin_restore_pending");}}
        if(restoreOnce())break;
      }catch(...){/* Keep exact owned original/reservation/children; no false restoration. */}
      (void)owner.display.refreshPreparationCleanupLease(id);Sleep(100);
    }
  }
  void cleanupDesktop(){
    if(!desktop)return;desktop->bridge->control(JsonParser::parse("{\"type\":\"input.reset\"}"));
    (void)desktop->bridge->stopSession(id);
    for(;;){const auto proof=desktop->bridge->stoppedSession(id);if(proof.workerQuiescent&&proof.profileRestored&&desktop->fullOriginalRestored&&desktop->fullOriginalRestored()&&(!desktopWorker||exited(desktopWorker.get(),desktopIdentity)))break;Sleep(25);}
    desktop.reset();desktopWorker.reset();
  }
  void nextPhase(Role next,Observation live){
    const auto oldWorker=fence.owned();if(!fence.transition(next,live,random<16>(),GetTickCount64()))refused("signin_phase_changed");desktopSourceGeneration=0;decoded=published=false;
    text(wire::prepare(id,fence.admission(),next));
    if(desktop)cleanupDesktop();else cleanupSecure();
    if(releasePending)refused("signin_owned_input_release_pending");
    if(!oldWorker||!fence.exited(*oldWorker,true)||!fence.released(*oldWorker,!releasePending)||!fence.restored(true)||!fence.successor())refused("signin_restore_pending");
    text(wire::previousExited(id,owner.instance,fence.admission(),next));
    observed=owner.observe();if(observed.console!=live.console||observed.windows!=live.windows||observed.sid!=live.sid)refused("signin_phase_changed");
    firstFrame={};secureFirst.reset();secureCleanup.reset();actualFirstFrame.reset();challenges.clear();desktopInfo={};
    if(next==Role::user){desktop.emplace(factory());if(!desktop->bridge||!desktop->workerQuery||!desktop->fullOriginalRestored)refused("signin_desktop_adapter_unavailable");auto ordinary=request;auto* fields=std::get_if<Json::Object>(&ordinary.value);if(!fields)refused("invalid_signin_request");fields->erase("signin");fields->erase("signinAdmission");(*fields)["resizeProtocol"].value=std::string("epoch-v1");(*fields)["epoch"].value=0.0;(*fields)["audio"].value=false;
      const auto weak=weak_from_this();const DesktopPacketSource source{fence.admission().workerGeneration,fence.admission().transition};desktopSourceGeneration=source.generation;desktop->bridge->start(ordinary,[weak,source](bool text,std::vector<uint8_t> bytes){if(auto held=weak.lock())held->enqueueScoped(text,std::move(bytes),source);});
    }else{
      // A new secure phase borrows no desktop worker or old child identity.
      // The original layout has been positively restored before this fresh
      // same-session V2 reservation and strictly newer phase launch.
      secureReader.reset();captureAuthority.reset();secure.reset();preparer.reset();preparationAuthority.reset();prepared.reset();original.reset();restorer.reset();restoreAuthority.reset();restoreReader.reset();restored.reset();prepareSerial=restoreSerial=0;stopQueued=false;releasePending=false;
      prepare(false);launchSecure();
    }
  }
  int step=0;
  void run(){
    try{
      step=1;auto pin=imagePin();imageSha=pin.digest;image=std::move(pin.file);step=2;prepare();step=3;launchSecure();step=4;
      while(auth()){
        const auto live=owner.observe();if(live.console!=observed.console)refused("signin_console_changed");
        if(fence.role()==Role::signin&&live.windows==WindowsState::unlocked){nextPhase(Role::user,live);continue;}
        if(fence.role()==Role::user&&live.windows!=WindowsState::unlocked){nextPhase(Role::signin,live);continue;}
        if(!current())refused("signin_authority_lost");controls();
        if(fence.role()==Role::signin){bool text=false;std::vector<uint8_t> bytes;const auto result=secureReader->next(text,bytes);if(result==Reader::Read::failed)refused("signin_capture_failed");if(result==Reader::Read::packet)securePacket(text,std::move(bytes));}
        answerChallenges();if(published&&GetTickCount64()>=nextAuthority)authority();
        if(reserved&&!owner.display.refreshPreparationLease(id))refused("signin_display_unavailable");Sleep(1);
      }
    }catch(const std::exception& error){
      // hostAction 294: where (stage 1 image, 2 prepare, 3 secure worker,
      // 4 running) and why (error = 1-based index in this fixed list, 0 other)
      // a sign-in session ended.
      {static constexpr std::string_view codes[]={"signin_entropy_unavailable","invalid_signin_request","signin_process_unavailable","signin_pipe_unavailable","signin_service_unavailable","signin_service_image_unavailable","signin_generation_exhausted","authorization_revoked","signin_authority_lost","signin_admission_expired","signin_restore_pending","signin_eligibility_changed","display_busy","signin_display_unavailable","signin_preparation_failed","signin_preparation_timeout","invalid_signin_info","signin_preparer_exit_pending","signin_worker_failed","signin_owned_input_release_pending","invalid_signin_media","invalid_signin_frame","desktop_capture_failed","signin_worker_identity_unavailable","invalid_signin_ack","signin_phase_changed","signin_desktop_adapter_unavailable","signin_console_changed","signin_capture_failed"};
        int64_t index=0;for(size_t i=0;i<std::size(codes);i++)if(error.what()==codes[i]){index=int64_t(i+1);break;}
        recordHostEvent("hostAction",{{"action",294},{"ok",false},{"stage",step},{"error",index}});}
      // Only our fixed public codes leave this process. Native exception text
      // may contain account/device/path data and is never forwarded.
      std::string reason="signin_stream_unavailable";
      for(std::string_view known:{"signin_owned_input_release_pending","signin_console_changed","signin_phase_changed","signin_capture_failed","signin_restore_pending","signin_preparer_exit_pending","signin_admission_expired","signin_authority_lost","signin_display_unavailable"})if(error.what()==known){reason=known;break;}
      try{const auto message="{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(id)+",\"reason\":"+jsonQuote(reason)+'}';emit(true,{message.begin(),message.end()});}catch(...){}
    }catch(...){recordHostEvent("hostAction",{{"action",294},{"ok",false},{"stage",step},{"error",0}});try{const auto message="{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(id)+",\"reason\":\"signin_stream_unavailable\"}";emit(true,{message.begin(),message.end()});}catch(...){}}
    try{authority(true);}catch(...){}fence.revoke();cancel();
    // Cleanup exceptions cannot destroy an owner with a live child or an
    // unrestored original. Retain those exact resources and fail closed.
    for(;;){try{if(desktop)cleanupDesktop();cleanupSecure();break;}catch(...){(void)owner.display.refreshPreparationCleanupLease(id);Sleep(100);}}
    done=true;
  }
};

SigninBridge::SigninBridge(NativeDisplayService& display,HANDLE stop):impl_(std::make_unique<Impl>(display,stop)){}
void SigninBridge::closeSchedulingAndJoin(){
  std::lock_guard lifetime(impl_->lifetimeMutex);impl_->permanentlyClosing=true;
  stop();std::shared_ptr<Impl::Session> held;{std::lock_guard lock(impl_->mutex);held=impl_->session;}
  if(held&&held->thread.joinable()){if(held->thread.get_id()==std::this_thread::get_id())throw std::runtime_error("signin_shutdown_on_session_thread");held->thread.join();}
  // The leftover restore owns a display reservation and a restorer child; it
  // ends on serviceStop or its 60 s admission and is joined like a session.
  if(impl_->leftoverThread.joinable())impl_->leftoverThread.join();
  // The actual restoration loop has physically returned. New start is closed;
  // only now may this exact pre-admitted cleanup owner leave the drain count.
  impl_->cleanupOwner.reset();
}
SigninBridge::~SigninBridge(){closeSchedulingAndJoin();}
bool SigninBridge::requested(const Json& value){return !std::holds_alternative<std::nullptr_t>(value.get("signin").value);}
bool SigninBridge::start(const Json& request,AgentBridge::Send send,DesktopFactory factory){
  try{
    std::lock_guard lifetime(impl_->lifetimeMutex);if(impl_->permanentlyClosing)return false;
    const auto receivedAt=GetTickCount64();const auto scope=impl_->eligibilityScope();const auto now=GetTickCount64();const auto& view=scope.native;const auto& grant=request.get("signinAdmission"),phase=request.get("signin");
    Admission a;a.stream=streamId(request.get("sessionId").string());a.service=view.service;a.transition=unhex<16>(phase.get("transitionId").string());a.workerGeneration=integer(phase.get("workerGeneration"),1,UINT32_MAX);a.mediaEpoch=uint16_t(integer(phase.get("mediaScope").get("epoch"),1,65535));a.grantRevision=integer(grant.get("grantRevision"),1,UINT32_MAX);a.eligibilityRevision=integer(grant.get("eligibilityRevision"),1,UINT32_MAX);a.serviceInstance=grant.get("serviceInstance").string();a.receivedAt=receivedAt;a.validForMs=integer(grant.get("validForMs"),1,1000);
    // A revision is accepted only if this service actually published it in
    // the same still-current native/config/authenticated-WSS scope.
    bool published=false;{std::lock_guard lock(impl_->mutex);published=impl_->mainSocket&&impl_->negotiated&&impl_->connectionGeneration.load()==scope.connectionGeneration&&impl_->history.accepts(a.eligibilityRevision,scope,now);}
    // hostAction 292: which admission check refused a sign-in start
    // (so that a refused start says why). Bits:
    // 1 mode, 2 monitor, 4 audio, 8 phase protocol, 16 phase, 32 media scope,
    // 64 grant protocol, 128 no factory/send, 256 switch off, 512 not
    // negotiated, 1024 stale/no owned output, 2048 revision not published,
    // 4096 admission not current, 8192 previous sign-in session busy,
    // 16384 malformed request.
    {const bool enabled=signinStreamingEnabled(readPublicConfig());int64_t why=0;
      if(request.get("mode").string()!="stream")why|=1;if(request.get("monitor").string()!="0")why|=2;if(request.get("audio").boolean(true))why|=4;
      if(phase.get("protocol").string()!="signin-stream-v1")why|=8;if(phase.get("phase").string()!="signin")why|=16;if(phase.get("mediaScope").get("kind").string()!="composite")why|=32;
      if(grant.get("protocol").string()!=protocol)why|=64;if(!factory||!send)why|=128;if(!enabled)why|=256;if(!impl_->negotiated)why|=512;if(!fresh(view,now))why|=1024;if(!published)why|=2048;
      if(!admissionCurrent(a,{enabled},signinBackendCompiled,impl_->instance,a.eligibilityRevision,view,now))why|=4096;
      if(why){recordHostEvent("hostAction",{{"action",292},{"ok",false},{"error",why},{"stage",int64_t(view.windows)}});return false;}}
    std::shared_ptr<Impl::Session> old;{std::lock_guard lock(impl_->mutex);if(impl_->session&&!impl_->session->done){recordHostEvent("hostAction",{{"action",292},{"ok",false},{"error",8192}});return false;}old=std::exchange(impl_->session,{});}if(old&&old->thread.joinable())old->thread.join();
    auto session=std::make_shared<Impl::Session>(*impl_,request.get("sessionId").string(),request,std::move(send),std::move(factory),a,view,scope.connectionGeneration,scope.configuration,scope.lease);{std::lock_guard lock(impl_->mutex);impl_->session=session;}session->thread=std::thread([session]{session->run();});
    recordHostEvent("hostAction",{{"action",292},{"ok",true},{"error",0}});return true;
  }catch(...){recordHostEvent("hostAction",{{"action",292},{"ok",false},{"error",16384}});return false;}
}
bool SigninBridge::control(const Json& value){std::shared_ptr<Impl::Session> s;{std::lock_guard lock(impl_->mutex);s=impl_->session;}if(!s||s->done||value.get("sessionId").string()!=s->id)return false;const auto type=value.get("type").string();if(type!="stream.phase.decoded"&&type!="input.phase"&&type!="host.signin.authority.query")return true;const auto bytes=jsonObject(value);return s->enqueue(true,{bytes.begin(),bytes.end()});}
void SigninBridge::stop(const std::string& session)noexcept{try{std::shared_ptr<Impl::Session> held;{std::lock_guard lock(impl_->mutex);if(impl_->session&&(session.empty()||session==impl_->session->id))held=impl_->session;}if(held)held->cancel();}catch(...){}}
bool SigninBridge::busy()const noexcept{try{std::lock_guard lock(impl_->mutex);return impl_->session&&!impl_->session->done;}catch(...){return true;}}
bool SigninBridge::owns(const std::string& id)const noexcept{try{std::lock_guard lock(impl_->mutex);return impl_->session&&impl_->session->id==id;}catch(...){return false;}}
SessionStopEvidence SigninBridge::stoppedSession(const std::string& id)const{std::lock_guard lock(impl_->mutex);if(!impl_->session||impl_->session->id!=id)return {};const bool done=impl_->session->done;return {done,true,done};}
std::string SigninBridge::capabilityFields(){
  if constexpr(!signinBackendCompiled)return {};
  if(!signinBackendCanNegotiate(signinStreamingEnabled(readPublicConfig())))return {};
  return ",\"signinServiceProtocol\":\"signin-service-v1\"";
}
std::string SigninBridge::eligibilityMessage(){
  if constexpr(!signinBackendCompiled)return {};
  const auto scope=impl_->eligibilityScope();const auto& live=scope.native;
  const bool enabled=signinStreamingEnabled(readPublicConfig()),isBusy=busy();const uint64_t at=GetTickCount64();
  // A leftover original is restored first (Impl::restoreLeftover); no offer meanwhile.
  const bool leftover=GetFileAttributesW((machineDirectory()/L"private"/L"display-v2.state").c_str())!=INVALID_FILE_ATTRIBUTES;
  if(leftover&&enabled&&impl_->negotiated&&!isBusy&&live.windows==WindowsState::noUser&&live.sid.empty()&&live.system&&live.consoleProtocol&&live.ownedSingleOutput&&
     !impl_->leftoverRunning&&at>=impl_->leftoverRetryAt&&!impl_->permanentlyClosing){
    if(impl_->leftoverThread.joinable())impl_->leftoverThread.join();
    impl_->leftoverRunning=true;try{impl_->leftoverThread=std::thread([impl=impl_.get()]{impl->restoreLeftover();});}catch(...){impl_->leftoverRunning=false;}
  }
  const bool eligible=!leftover&&!impl_->leftoverRunning&&scope.connectionGeneration&&signinBackendMayPublish(enabled,impl_->negotiated,fresh(live,at))&&!isBusy&&
    ((live.windows==WindowsState::locked&&ordinaryUserSid(live.sid))||(live.windows==WindowsState::noUser&&live.sid.empty()));
  // hostAction 290: which announce condition holds the sign-in screen back
  // (so that a signed-out Host that never offers it says why).
  // Bits: 1 switch off, 2 broker not negotiated, 4 no broker connection,
  // 8 not SYSTEM, 16 no console session, 32 console not local, 64 no single
  // owned virtual output, 128 stale, 256 busy, 512 Windows state, 1024 SID,
  // 2048 a leftover original is being restored first.
  // Recorded only when the result changes.
  {int64_t why=0;if(!enabled)why|=1;if(!impl_->negotiated)why|=2;if(!scope.connectionGeneration)why|=4;if(!live.system)why|=8;if(!live.console||live.console==UINT32_MAX)why|=16;if(!live.consoleProtocol)why|=32;if(!live.ownedSingleOutput)why|=64;
    if(!live.at||live.at>at||at-live.at>1000||!live.generation||!valid(live.service))why|=128;if(isBusy)why|=256;if(leftover||impl_->leftoverRunning)why|=2048;
    if(live.windows!=WindowsState::locked&&live.windows!=WindowsState::noUser)why|=512;else if(live.windows==WindowsState::locked?!ordinaryUserSid(live.sid):!live.sid.empty())why|=1024;
    const int64_t key=why*8+int64_t(live.windows);bool changed=false;{std::lock_guard lock(impl_->mutex);changed=impl_->lastDiagnostic!=key;impl_->lastDiagnostic=key;}
    if(changed)recordHostEvent("hostAction",{{"action",290},{"stage",int64_t(live.windows)},{"ok",eligible},{"error",why}});}
  uint32_t revision=0;DiscoveryPublication::Action action;
  {std::lock_guard lock(impl_->mutex);if(impl_->eligibility==UINT32_MAX)return {};if(!eligible)impl_->history.clear();const auto next=impl_->discovery.observe(eligible);if(!next)return {};action=*next;revision=++impl_->eligibility;if(eligible&&(!impl_->mainSocket||impl_->connectionGeneration.load()!=scope.connectionGeneration||!impl_->history.published(revision,scope,GetTickCount64())))return {};}
  if(action==DiscoveryPublication::Action::withdraw)return wire::eligibilityWithdraw(impl_->instance,revision);
  return wire::eligibility(impl_->instance,revision,live.windows);
}
void SigninBridge::connectionOpened(HINTERNET actual){
  if(!actual)return;std::shared_ptr<Impl::Session> old;
  {std::lock_guard lock(impl_->mutex);if(impl_->mainSocket==actual)return;if(impl_->connectionGeneration==UINT64_MAX)refused("signin_generation_exhausted");impl_->mainSocket=actual;++impl_->connectionGeneration;impl_->negotiated=false;impl_->history.clear();impl_->discovery=DiscoveryPublication{};old=impl_->session;}
  if(old)old->cancel();
}
void SigninBridge::connectionClosed(HINTERNET actual){
  std::shared_ptr<Impl::Session> old;{std::lock_guard lock(impl_->mutex);if(!actual||impl_->mainSocket!=actual)return;impl_->mainSocket=nullptr;impl_->negotiated=false;impl_->history.clear();impl_->discovery=DiscoveryPublication{};old=impl_->session;}
  // closeTransport may run INSIDE an in-flight output callback. Never wait on
  // that callback here. Explicit owner stop drains it before Agent destruction.
  if(old)old->revoke();
}
void SigninBridge::protocolNegotiated(HINTERNET actual,bool accepted){
  std::shared_ptr<Impl::Session> old;{std::lock_guard lock(impl_->mutex);if(!actual||impl_->mainSocket!=actual)return;impl_->negotiated=accepted;if(!accepted){impl_->history.clear();old=impl_->session;}}
  if(old)old->revoke();
}

}
