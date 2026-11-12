#include "signin_diagnostic_coordinator.hpp"
#include "signin_preparation_authority.hpp"
#include "signin_restoration_authority.hpp"
#include "broker_status.hpp"
#include "pipe_packet.hpp"
#include <array>
#include <condition_variable>
#include <thread>

namespace host::signin::diagnostic {
namespace {
using namespace coordinator_detail;
bool signalled(HANDLE event){return event&&WaitForSingleObject(event,0)!=WAIT_TIMEOUT;}
[[noreturn]] void unavailable(){throw std::runtime_error("Private preparation is unavailable");}
template<class Bytes>void randomBytes(Bytes& bytes){
  if(BCryptGenRandom(nullptr,bytes.data(),DWORD(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)unavailable();
}
// Incremental ONE-reader framing. A 262144-byte RPC can exceed the pipe buffer;
// waiting for the complete packet in PeekNamedPipe would deadlock its writer.
// Pending OVERLAPPED storage/buffer live here until completion/cancellation.
class Reader {
 public:
  explicit Reader(HANDLE pipe):pipe_(pipe),event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)){
    if(!event_)unavailable();operation_.hEvent=event_.get();
  }
  ~Reader(){cancel();}
  Reader(const Reader&)=delete;
  enum class State { waiting,message,failed };
  State read(std::string& message){
    for(unsigned pass=0;pass<4;++pass){
      if(pending_){
        DWORD count=0;
        if(!GetOverlappedResult(pipe_,&operation_,&count,FALSE)){
          if(GetLastError()==ERROR_IO_INCOMPLETE)return State::waiting;
          pending_=false;return State::failed;
        }
        pending_=false;if(!append(count))return State::failed;
      }
      const auto ready=extract(message);if(ready!=State::waiting)return ready;
      DWORD available=0;if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr))return State::failed;
      if(!available)return State::waiting;
      const DWORD size=std::min<DWORD>(available,DWORD(chunk_.size()));DWORD count=0;
      ResetEvent(event_.get());operation_={};operation_.hEvent=event_.get();
      if(!ReadFile(pipe_,chunk_.data(),size,&count,&operation_)){
        if(GetLastError()!=ERROR_IO_PENDING)return State::failed;
        pending_=true;return State::waiting;
      }
      if(!append(count))return State::failed;
    }
    return extract(message);
  }
 private:
  bool append(DWORD count){
    if(!count||count>chunk_.size()||bytes_.size()+count>262149)return false;
    bytes_.insert(bytes_.end(),chunk_.begin(),chunk_.begin()+count);return true;
  }
  State extract(std::string& message){
    if(bytes_.size()<4)return State::waiting;
    const uint32_t length=uint32_t(bytes_[0])|(uint32_t(bytes_[1])<<8)|
      (uint32_t(bytes_[2])<<16)|(uint32_t(bytes_[3])<<24);
    if(length<2||length>262145)return State::failed;
    if(bytes_.size()<size_t(length)+4)return State::waiting;
    if(bytes_[4]!=0)return State::failed;
    message.assign(reinterpret_cast<const char*>(bytes_.data()+5),length-1);
    bytes_.erase(bytes_.begin(),bytes_.begin()+length+4);return State::message;
  }
  void cancel()noexcept{
    if(pending_){DWORD count=0;CancelIoEx(pipe_,&operation_);
      (void)GetOverlappedResult(pipe_,&operation_,&count,TRUE);pending_=false;}
  }
  HANDLE pipe_;native::Handle event_;OVERLAPPED operation_{};
  bool pending_=false;std::array<uint8_t,65536> chunk_{};std::vector<uint8_t> bytes_;
};

// Hold and hash the actual service image against writes/deletion. This digest
// binds same-image child launch; it is NOT a dynamically accepted Tool allow pin.
Nonce imageIdentity(native::Handle& held){
  const auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!manager)unavailable();
  struct Sc {SC_HANDLE value;~Sc(){if(value)CloseServiceHandle(value);}} scm{manager};
  Sc service{OpenServiceW(scm.value,L"MultitapHost",SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS)};
  if(!service.value)unavailable();
  SERVICE_STATUS_PROCESS state{};DWORD needed=0;
  const auto path=brokerServiceExecutable(service.value);
  if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&state),sizeof(state),&needed)||
      state.dwCurrentState!=SERVICE_RUNNING||state.dwProcessId!=GetCurrentProcessId()||
      !brokerSystemProcess(GetCurrentProcess())||!brokerProcessExecutable(GetCurrentProcess(),path))unavailable();
  held=native::Handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
  if(!held||GetFileType(held.get())!=FILE_TYPE_DISK||!GetFileInformationByHandle(held.get(),&info)||
      info.nNumberOfLinks!=1||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
      !GetFileSizeEx(held.get(),&size)||size.QuadPart<=0||size.QuadPart>128ll*1024*1024)unavailable();
  std::wstring finalName(32768,L'\0');const auto length=GetFinalPathNameByHandleW(held.get(),finalName.data(),DWORD(finalName.size()),FILE_NAME_NORMALIZED);
  if(!length||length>=finalName.size())unavailable();finalName.resize(length);
  if(finalName.starts_with(L"\\\\?\\"))finalName.erase(0,4);
  if(CompareStringOrdinal(finalName.c_str(),int(finalName.size()),path.c_str(),int(path.size()),TRUE)!=CSTR_EQUAL)unavailable();
  struct Hash {BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    ~Hash(){if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);}} hash;
  if(BCryptOpenAlgorithmProvider(&hash.alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)unavailable();
  DWORD objectSize=0,count=0;if(BCryptGetProperty(hash.alg,BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<BYTE*>(&objectSize),sizeof(objectSize),&count,0)<0||!objectSize||objectSize>65536)unavailable();
  std::vector<uint8_t> object(objectSize);
  if(BCryptCreateHash(hash.alg,&hash.hash,object.data(),objectSize,nullptr,0,0)<0)unavailable();
  std::array<uint8_t,65536> buffer{};uint64_t total=0;
  for(;;){DWORD read=0;if(!ReadFile(held.get(),buffer.data(),DWORD(buffer.size()),&read,nullptr))unavailable();
    if(!read)break;total+=read;if(total>uint64_t(size.QuadPart)||BCryptHashData(hash.hash,buffer.data(),read,0)<0)unavailable();}
  Nonce digest{};if(total!=uint64_t(size.QuadPart)||BCryptFinishHash(hash.hash,digest.data(),DWORD(digest.size()),0)<0)unavailable();
  return digest;
}
Exit actualExit(HANDLE handle,Process expected){
  if(!handle)return Exit::none;
  try{if(!valid(expected)||native::processIdentity(handle)!=expected)return Exit::unknown;
    const auto wait=WaitForSingleObject(handle,0);return wait==WAIT_OBJECT_0?Exit::exited:wait==WAIT_TIMEOUT?Exit::alive:Exit::unknown;
  }catch(...){return Exit::unknown;}
}
native::Handle copyWait(HANDLE source){
  HANDLE copy=nullptr;if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&copy,
      PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))unavailable();return native::Handle(copy);
}
class NativeOperations final:public Operations {
 public:
  NativeOperations(NativeDisplayService& display,HANDLE stop):display_(display),stop_(stop){}
  Start begin(VerifiedOperator&& peer,const Request& request)override{
    if(engaged_)return Start::refused;
    try{
      if(forwardCancelled()||signalled(stop_)||!peer.current()||!ownedOutputSize(request.width,request.height)||
          request.width<320||request.width>8192||request.height<180||request.height>8192)return Start::refused;
      const auto console=native::observeConsole(peer.consoleSession());
      if(!console.valid||!console.consoleProtocol||console.session!=peer.consoleSession()||
          (console.windows!=WindowsState::locked&&console.windows!=WindowsState::noUser))return Start::refused;
      original_=std::make_unique<preparation::OriginalFile>(machineDirectory()/L"private"/L"display-v2.state");
      if(!original_->absent())return Start::refused;
      ownImageSha_=imageIdentity(image_);
      auto snapshot=JsonParser::parse(display_.viewportSnapshot());
      const auto instance=snapshot.get("deviceInstanceId").string();uint32_t generation=0;
      if(instance.empty()||!nativeDisplayInteger(snapshot.get("generation").number(),0,UINT32_MAX))return Start::refused;
      generation=uint32_t(snapshot.get("generation").number());
      StreamId stream{};randomBytes(stream);const auto uuid=PreparedServiceAuthority::streamText(stream);
      const auto heartbeatToken=std::make_shared<const std::string>(uuid);
      stream_=stream;streamText_=uuid;
      if(!display_.reservePreparation(uuid,nativeDisplayLeaseKey(snapshot)))return Start::refused;
      // From here onward even an exception leaves this exact obligation owned.
      engaged_=true;reserved_=true;
      heartbeatStream_.store(heartbeatToken);
      peer_=std::move(peer);
      const auto lease=display_.preparationLease(uuid,instance,generation);
      if(!lease||lease->targets.size()!=1||lease->targets.front().id!="0")return Start::failed;
      preparation::PreparationAdmission admission;
      admission.service=native::processIdentity(GetCurrentProcess());admission.operatorProcess=peer_->identity();
      admission.stream=stream;admission.lease={lease->instance,lease->generation};admission.operation=peer_->operation();
      admission.installedImageSha256=ownImageSha_;admission.consoleSession=peer_->consoleSession();
      admission.width=request.width;admission.height=request.height;
      admission.issuedAt=peer_->issuedTick();admission.expiresAt=peer_->deadlineTick();prepAdmission_=admission;
      prepAuthority_=std::make_unique<preparation::ServiceAuthority>(display_,admission,peer_->processHandle(),*original_,
        [this](const auto&){return forwardCurrent();});
      prepChild_=std::make_unique<preparation::OwnedPreparationChild>();
      if(!preparation::launch(admission,*prepAuthority_,*prepChild_))return Start::failed;
      if(!prepAuthority_->bindChild(prepChild_->process())||!prepChild_->connect(2000))return Start::failed;
      prepReader_=std::make_unique<Reader>(prepChild_->pipe());
      if(!send(prepChild_->pipe(),preparation::initPacket(admission,prepChild_->binding(),prepChild_->delegatedServiceQuery()),true))return Start::failed;
      return Start::started;
    }catch(...){return Start::failed;}
  }
  bool engaged()const noexcept override{return engaged_;}
  bool forwardCurrent()const noexcept override{
    try{return !forwardCancelled()&&!signalled(stop_)&&peer_&&peer_->current();}catch(...){return false;}
  }
  Poll preparation()override{
    try{
      if(!prepReader_||!prepChild_||!peerIdentity(prepChild_->pipe(),prepChild_->binding().worker))return Poll::failed;
      std::string bytes;const auto result=prepReader_->read(bytes);
      if(result==Reader::State::waiting)return Poll::waiting;
      if(result==Reader::State::failed)return Poll::failed;
      const auto value=JsonParser::parse(bytes);
      if(value.get("type").string()=="signin.prepare.finished-v1"){
        prepReceipt_=preparation::parseReceipt(value);
        if(prepReceipt_->binding!=prepChild_->binding())return Poll::failed;
        const auto retained=prepAuthority_->retained();
        prepared_=retained&&preparation::receiptPacket(*retained)==preparation::receiptPacket(*prepReceipt_)&&
          prepReceipt_->outcome==preparation::Outcome::prepared;
        return Poll::finished;
      }
      return send(prepChild_->pipe(),preparation::answer(value,prepAdmission_,prepChild_->binding(),*prepAuthority_,prepSerial_),
          value.get("mode").string()=="prepare")?Poll::waiting:Poll::failed;
    }catch(...){return Poll::failed;}
  }
  bool preparationSucceeded()const noexcept override{return prepared_;}
  Exit preparationExit()override{return prepChild_?actualExit(prepChild_->process(),prepChild_->binding().worker):Exit::none;}
  void stopPreparation()noexcept override{try{
    // Drain our OVERLAPPED read BEFORE OwnedChild closes its pipe handle.
    prepReader_.reset();if(prepChild_)prepChild_->stopAndWait(250);
  }catch(...){} }
  Original original()override{
    try{
      if(!original_)return Original::unknown;
      if(original_->current()){
        if(obligationSha_.empty())obligationSha_=original_->sha256();
        return obligationSha_==original_->sha256()?Original::owned:Original::unknown;
      }
      const auto known=original_->sha256();
      if(!known.empty()&&original_->adopt(known)&&original_->current()){
        if(obligationSha_.empty())obligationSha_=known;
        return obligationSha_==known?Original::owned:Original::unknown;
      }
      // A failed retirement/previously held original is never reclassified as
      // "nothing changed" merely because the current pathname disappeared.
      if(!known.empty())return Original::unknown;
      return original_->absent()?Original::absent:Original::unknown;
    }catch(...){return Original::unknown;}
  }
  bool startRestoration()override{
    try{
      if(!engaged_||!reserved_||original()!=Original::owned||preparationExit()!=Exit::exited)return false;
      if(restoreChild_){
        const auto exit=restorationExit();
        if(exit!=Exit::none&&exit!=Exit::exited)return false;
        if(exit==Exit::exited)predecessors_.push_back({copyWait(restoreChild_->process()),restoreChild_->binding().worker});
        restoreReader_.reset();restoreChild_.reset();restoreAuthority_.reset();restoreReceipt_.reset();
      }
      if(predecessors_.size()>=3)return false;
      auto snapshot=JsonParser::parse(display_.viewportSnapshot());
      if(snapshot.get("deviceInstanceId").string()!=prepAdmission_.lease.instance||
          !nativeDisplayInteger(snapshot.get("generation").number(),0,UINT32_MAX))return false;
      const auto lease=display_.preparationCleanupLease(streamText_,prepAdmission_.lease.instance,uint32_t(snapshot.get("generation").number()));
      if(!lease)return false;
      restoration::RestorationAdmission admission;admission.service=prepAdmission_.service;
      admission.stream=stream_;admission.lease={lease->instance,lease->generation};randomBytes(admission.operation);
      admission.installedImageSha256=ownImageSha_;admission.consoleSession=prepAdmission_.consoleSession;
      admission.originalSha256=original_->sha256();admission.issuedAt=now();admission.expiresAt=admission.issuedAt+60000;
      std::vector<HANDLE> handles{prepChild_->process()};
      admission.predecessors.push_back(prepChild_->binding().worker);
      for(const auto& prior:predecessors_){
        if(actualExit(prior.handle.get(),prior.identity)!=Exit::exited)return false;
        handles.push_back(prior.handle.get());admission.predecessors.push_back(prior.identity);
      }
      auto authority=std::make_unique<restoration::ServiceAuthority>(display_,admission,handles,*original_);
      restoreAuthority_=std::move(authority);restoreAdmission_=admission;restoreSerial_=0;
      restoreChild_=std::make_unique<restoration::OwnedRestorationChild>();
      // Once a process exists its object survives failed launch/bind/connect.
      if(!restoration::launch(admission,*restoreAuthority_,*restoreChild_))return false;
      if(!restoreAuthority_->bindChild(restoreChild_->process())||!restoreChild_->connect(2000))return false;
      restoreReader_=std::make_unique<Reader>(restoreChild_->pipe());
      return send(restoreChild_->pipe(),restoration::initPacket(admission,restoreChild_->binding(),restoreChild_->delegatedServiceQuery()),false);
    }catch(...){return false;}
  }
  bool rearmRestoration()override{
    try{
      if(!engaged_||!reserved_||!image_||obligationSha_.empty()||!prepChild_||!restoreChild_||
          preparationExit()!=Exit::exited||restorationExit()!=Exit::exited||original()!=Original::owned||
          original_->sha256()!=obligationSha_)return false;
      const auto bytes=original_->current();if(!bytes)return false;
      for(const auto& prior:predecessors_)if(actualExit(prior.handle.get(),prior.identity)!=Exit::exited)return false;
      const auto snapshot=JsonParser::parse(display_.viewportSnapshot());
      if(snapshot.get("deviceInstanceId").string()!=prepAdmission_.lease.instance||
          !nativeDisplayInteger(snapshot.get("generation").number(),0,UINT32_MAX))return false;
      const auto generation=uint32_t(snapshot.get("generation").number());
      const auto lease=display_.preparationCleanupLease(streamText_,prepAdmission_.lease.instance,generation);
      const auto saved=parseDisplayV2State(*bytes);
      if(!lease||lease->stream!=streamText_||lease->service.pid!=prepAdmission_.service.pid||
          lease->service.created!=prepAdmission_.service.created||lease->targets.size()!=1||
          lease->targets.front().id!="0"||saved.session!=streamText_||saved.owned.size()!=1||saved.owned.front().id!="0"||
          !nativeSameLuid(lease->targets.front().adapter,saved.owned.front().adapter))return false;
      // The same retained SDK controller can recreate public output 0 during
      // our size change. Its fresh typed proof owns that current target; the
      // sealed original keeps the old target for semantic layout restoration.
      // The held file retains its remembered volume/file ID across adoption.
      // Repeat after the controller query before releasing any old wait handle.
      if(original_->current()!=bytes||original_->sha256()!=obligationSha_||
          preparationExit()!=Exit::exited||restorationExit()!=Exit::exited)return false;
      restoreReader_.reset();
      // Only older positively-exited identities are pruned. The original
      // preparer AND immediate prior restorer stay held for the next launch.
      predecessors_.clear();return true;
    }catch(...){return false;}
  }
  Poll restoration()override{
    try{
      if(!restoreReader_||!restoreChild_||!peerIdentity(restoreChild_->pipe(),restoreChild_->binding().worker))return Poll::failed;
      std::string bytes;const auto result=restoreReader_->read(bytes);
      if(result==Reader::State::waiting)return Poll::waiting;
      if(result==Reader::State::failed)return Poll::failed;
      const auto value=JsonParser::parse(bytes);
      if(value.get("type").string()=="signin.restore.finished-v1"){
        restoreReceipt_=restoration::parseReceipt(value);
        return restoreReceipt_->binding==restoreChild_->binding()?Poll::finished:Poll::failed;
      }
      return send(restoreChild_->pipe(),restoration::answer(value,restoreAdmission_,restoreChild_->binding(),*restoreAuthority_,restoreSerial_),false)?Poll::waiting:Poll::failed;
    }catch(...){return Poll::failed;}
  }
  Exit restorationExit()override{return restoreChild_?actualExit(restoreChild_->process(),restoreChild_->binding().worker):Exit::none;}
  void stopRestoration()noexcept override{try{
    restoreReader_.reset();if(restoreChild_)restoreChild_->stopAndWait(250);
  }catch(...){} }
  bool finishRestoration()override{
    return restoreAuthority_&&restoreReceipt_&&restoreAuthority_->finish(*restoreReceipt_);
  }
  bool releaseReservation()override{
    if(!reserved_)return !engaged_;
    if(preparationExit()!=Exit::none&&preparationExit()!=Exit::exited)return false;
    if(restorationExit()!=Exit::none&&restorationExit()!=Exit::exited)return false;
    if(!display_.releasePreparation(streamText_))return false;
    reserved_=false;heartbeatStream_.store({});
    prepReader_.reset();restoreReader_.reset();prepChild_.reset();restoreChild_.reset();
    prepAuthority_.reset();restoreAuthority_.reset();predecessors_.clear();original_.reset();peer_.reset();image_.reset();
    prepReceipt_.reset();restoreReceipt_.reset();prepSerial_=restoreSerial_=0;prepared_=false;engaged_=false;obligationSha_.clear();
    return true;
  }
  bool heartbeat()noexcept override{
    const auto stream=heartbeatStream_.load();
    return !stream||display_.refreshPreparationCleanupLease(*stream);
  }
  uint64_t now()const noexcept override{return GetTickCount64();}
  void pause(uint32_t milliseconds)noexcept override{Sleep(milliseconds);}
 private:
  bool peerIdentity(HANDLE pipe,Process child)const{
    ULONG pid=0;return valid(child)&&GetNamedPipeClientProcessId(pipe,&pid)&&pid==child.pid;
  }
  bool send(HANDLE pipe,const std::string& bytes,bool forward){
    if(bytes.empty()||bytes.size()>262144||(forward&&!forwardCurrent()))return false;
    const std::atomic<bool> neverStopped=false;
    // Existing writer has a bounded logical I/O timeout. No raw RPC body is
    // logged; cleanup's lifetime is independent of operator cancellation.
    return pipePacket(pipe,true,std::vector<uint8_t>(bytes.begin(),bytes.end()),neverStopped,writeMutex_);
  }
  NativeDisplayService& display_;HANDLE stop_;
  bool engaged_=false,reserved_=false,prepared_=false;
  std::optional<VerifiedOperator> peer_;native::Handle image_;Nonce ownImageSha_{};
  StreamId stream_{};std::string streamText_,obligationSha_;std::atomic<std::shared_ptr<const std::string>> heartbeatStream_;
  std::unique_ptr<preparation::OriginalFile> original_;
  preparation::PreparationAdmission prepAdmission_;restoration::RestorationAdmission restoreAdmission_;
  std::unique_ptr<preparation::ServiceAuthority> prepAuthority_;
  std::unique_ptr<restoration::ServiceAuthority> restoreAuthority_;
  std::unique_ptr<preparation::OwnedPreparationChild> prepChild_;
  std::unique_ptr<restoration::OwnedRestorationChild> restoreChild_;
  struct ExitedPredecessor {native::Handle handle;Process identity;};
  std::unique_ptr<Reader> prepReader_,restoreReader_;std::vector<ExitedPredecessor> predecessors_;
  std::optional<preparation::Receipt> prepReceipt_;std::optional<restoration::Receipt> restoreReceipt_;
  uint64_t prepSerial_=0,restoreSerial_=0;std::mutex writeMutex_;
};
} // namespace

struct Coordinator::Impl {
  enum class Phase { idle,preparing,preparationExit,restoreStart,restoring,restorationExit,retire,release };
  std::unique_ptr<Operations> operations;HANDLE stop;
  std::shared_ptr<std::atomic<bool>> busy=std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
  std::mutex state,heartbeatMutex;std::condition_variable heartbeatWake;
  bool ending=false;std::thread heartbeatThread;Phase phase=Phase::idle;
  bool prepared=false,restoreFinished=false;uint32_t restoreAttempts=0,rearmCycles=0;
  uint64_t phaseAt=0,nextRestoreAt=0;Result result{};
  Impl(std::unique_ptr<Operations> value,HANDLE event):operations(std::move(value)),stop(event){
    if(!operations)unavailable();operations->bindCancellation(cancelled);
    heartbeatThread=std::thread([this]{
      std::unique_lock lock(heartbeatMutex);
      while(!ending){
        heartbeatWake.wait_for(lock,std::chrono::milliseconds(500),[this]{return ending;});
        if(ending)break;
        lock.unlock();
        if(busy->load(std::memory_order_seq_cst)&&!operations->heartbeat())cancelled->store(true,std::memory_order_seq_cst);
        lock.lock();
      }
    });
  }
  ~Impl(){
    {std::lock_guard lock(heartbeatMutex);ending=true;}heartbeatWake.notify_all();
    if(heartbeatThread.joinable())heartbeatThread.join();
    // Retiring this object never removes an original or releases a pending
    // reservation. Caller must retain it while any resource obligation exists.
  }
  void enter(Phase next){phase=next;phaseAt=operations->now();}
  Result pending()const{return {false,Code::restorePending};}
  uint64_t cleanupBackoff()const{return rearmCycles==0?5000:rearmCycles==1?30000:60000;}
  void scheduleRestore(uint64_t now){
    enter(Phase::restoreStart);const auto delay=restoreAttempts>=2?cleanupBackoff():1000;
    nextRestoreAt=now>UINT64_MAX-delay?UINT64_MAX:now+delay;
  }
  Result step(){
    if(phase==Phase::idle)return result;
    const auto now=operations->now();
    switch(phase){
      case Phase::preparing:{
        if(cancelled->load(std::memory_order_seq_cst)||signalled(stop)||!operations->forwardCurrent()){
          cancelled->store(true,std::memory_order_seq_cst);operations->stopPreparation();enter(Phase::preparationExit);break;
        }
        const auto poll=operations->preparation();
        if(poll!=Poll::waiting){prepared=poll==Poll::finished&&operations->preparationSucceeded();enter(Phase::preparationExit);}
        break;
      }
      case Phase::preparationExit:{
        const auto exit=operations->preparationExit();
        if(exit==Exit::exited||exit==Exit::none){
          const auto original=operations->original();
          if(original==Original::owned){enter(Phase::restoreStart);nextRestoreAt=now;}
          else if(original==Original::absent){prepared=false;enter(Phase::release);}
        }else if(now-phaseAt>=1000)operations->stopPreparation();
        break;
      }
      case Phase::restoreStart:{
        if(now<nextRestoreAt)break;
        if(restoreAttempts>=2){
          const bool rearmed=operations->rearmRestoration();
          rearmCycles=std::min<uint32_t>(2,rearmCycles+1);
          if(!rearmed){scheduleRestore(now);break;}
          restoreAttempts=0;
        }
        const bool started=operations->startRestoration();
        const auto exit=operations->restorationExit();
        if(started){++restoreAttempts;restoreFinished=false;enter(Phase::restoring);}
        else if(exit!=Exit::none){++restoreAttempts;restoreFinished=false;operations->stopRestoration();enter(Phase::restorationExit);}
        else nextRestoreAt=now+1000;
        break;
      }
      case Phase::restoring:{
        // Cleanup is NOT revoked by loss of the forward operator/stop event.
        const auto poll=operations->restoration();
        if(poll!=Poll::waiting){restoreFinished=poll==Poll::finished;enter(Phase::restorationExit);}
        else if(now-phaseAt>=60000){operations->stopRestoration();enter(Phase::restorationExit);}
        break;
      }
      case Phase::restorationExit:{
        const auto exit=operations->restorationExit();
        if(exit==Exit::exited){
          if(restoreFinished)enter(Phase::retire);
          else scheduleRestore(now);
        }else if(now-phaseAt>=1000)operations->stopRestoration();
        break;
      }
      case Phase::retire:
        if(operations->finishRestoration())enter(Phase::release);
        else if(now-phaseAt>2000)scheduleRestore(now);
        break;
      case Phase::release:
        if(operations->releaseReservation()){
          result={prepared,prepared?Code::prepared:cancelled->load(std::memory_order_seq_cst)?Code::cancelled:Code::failed};
          phase=Phase::idle;busy->store(false,std::memory_order_seq_cst);return result;
        }
        break;
      default:break;
    }
    return pending();
  }
};
Coordinator::Coordinator(NativeDisplayService& display,HANDLE stop)
  :Coordinator(std::make_unique<NativeOperations>(display,stop),stop){}
Coordinator::Coordinator(std::unique_ptr<Operations> operations,HANDLE stop)
  :impl_(std::make_unique<Impl>(std::move(operations),stop)){}
Coordinator::~Coordinator(){cancelForward();}
bool Coordinator::busy()const noexcept{return impl_->busy->load(std::memory_order_seq_cst);}
std::shared_ptr<std::atomic<bool>> Coordinator::busyFlag()const{return impl_->busy;}
void Coordinator::cancelForward()noexcept{impl_->cancelled->store(true,std::memory_order_seq_cst);}
Result Coordinator::prepare(VerifiedOperator&& peer,const Request& request,const FinalAdmission& finalCheck){
  std::unique_lock lock(impl_->state,std::try_to_lock);
  if(!lock.owns_lock()||busy())return {false,Code::busy};
  impl_->cancelled->store(false,std::memory_order_seq_cst);
  bool expected=false;if(!impl_->busy->compare_exchange_strong(expected,true,std::memory_order_seq_cst))return {false,Code::busy};
  try{
    if(!peer.current()||!finalCheck||!finalCheck()||signalled(impl_->stop)){
      impl_->busy->store(false,std::memory_order_seq_cst);return {false,Code::refused};
    }
    const auto deadline=peer.deadlineTick();
    impl_->prepared=false;impl_->restoreFinished=false;impl_->restoreAttempts=0;impl_->rearmCycles=0;
    impl_->result={false,Code::failed};
    const auto start=impl_->operations->begin(std::move(peer),request);
    if(start!=Start::started&&!impl_->operations->engaged()){
      impl_->busy->store(false,std::memory_order_seq_cst);return {false,start==Start::refused?Code::refused:Code::failed};
    }
    impl_->enter(start==Start::started?Impl::Phase::preparing:Impl::Phase::preparationExit);
    do{
      const auto result=impl_->step();if(!busy())return result;
      impl_->operations->pause(10);
    }while(impl_->operations->now()<deadline&&!signalled(impl_->stop));
    cancelForward();impl_->operations->stopPreparation();
    return impl_->pending();
  }catch(...){
    cancelForward();
    if(!impl_->operations->engaged())impl_->busy->store(false,std::memory_order_seq_cst);
    else if(impl_->phase==Impl::Phase::idle)impl_->enter(Impl::Phase::preparationExit);
    return busy()?impl_->pending():Result{false,Code::failed};
  }
}
Result Coordinator::tickCleanup(){
  std::unique_lock lock(impl_->state,std::try_to_lock);if(!lock.owns_lock())return {false,Code::restorePending};
  if(!busy())return impl_->result;
  cancelForward();
  try{return impl_->step();}catch(...){return {false,Code::restorePending};}
}
Result Coordinator::shutdown(){cancelForward();return tickCleanup();}
} // namespace host::signin::diagnostic
