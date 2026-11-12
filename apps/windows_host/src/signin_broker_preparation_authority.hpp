#pragma once
#include "signin_preparation.hpp"
#include "signin_original_file.hpp"
#include "native_display_service.hpp"
#include "signin_service_authority.hpp"

namespace host::signin::preparation {
// Production-only adapter for one current authenticated broker stream.
// The callback belongs to the service session, never a peer verified flag.
// Diagnostic local-operator consent cannot construct this distinct authority.
class BrokerAuthority final : public Authority {
 public:
  using LiveGrant=std::function<bool(const PreparationAdmission&)>;
  BrokerAuthority(NativeDisplayService& display,PreparationAdmission admission,
      OriginalFile& original,LiveGrant consent)
    :display_(display),admission_(std::move(admission)),original_(original),consent_(std::move(consent)){
    if(!consent_||admission_.purpose!=Purpose::brokerStream||!validAdmission(admission_,GetTickCount64())||
        !initialFresh()||admission_.operatorProcess!=admission_.service||
        admission_.service!=native::processIdentity(GetCurrentProcess()))invalid();
    if(!consent_(admission_))invalid();
    native::OwnSystemLaunchMaterial material;
    if(!native::ownSystemLaunchMaterial(admission_.service,admission_.consoleSession,admission_.installedImageSha256,material))invalid();
    service_=queryHandle(GetCurrentProcess());serviceImage_=std::move(material.imageLock);
    generation_=admission_.lease.generation;
    const auto lease=leaseLocked();
    if(!lease||lease->targets.size()!=1||lease->targets.front().id!="0")invalid();
    originalTarget_=lease->targets.front();
    bool adopted=false;
    try{
      if(!consent_(admission_))invalid();
      if(!admission_.expectedOriginalSha256.empty()){
        if(!original_.adopt(admission_.expectedOriginalSha256))invalid();
        adopted=true;
        const auto bytes=original_.current();
        if(!bytes||!baselineMatches(*bytes,false))invalid();
      }else if(!original_.absent())invalid();
      if(!consent_(admission_)||!initialFresh()||!liveService())invalid();
    }catch(...){
      // No child exists and no display change has run. Release only the pin
      // adopted by THIS constructor, preserving any earlier owner's pin.
      if(adopted)original_.releaseForRestore();
      throw;
    }
  }
  bool bindChild(HANDLE exactOwnedChild){
    std::lock_guard lock(mutex_);
    try{
      if(child_||!initialFresh()||!liveService()||!consent_(admission_)||!validAdmission(admission_,GetTickCount64()))return false;
      const auto identity=native::processIdentity(exactOwnedChild);
      if(!valid(identity)||identity==admission_.service||identity==admission_.operatorProcess||
          WaitForSingleObject(exactOwnedChild,0)!=WAIT_TIMEOUT)return false;
      child_=queryHandle(exactOwnedChild);
      if(!initialFresh()||!liveService()||!consent_(admission_)||native::processIdentity(child_.get())!=identity)return false;
      binding_.service=admission_.service;
      binding_.worker=identity;binding_.operatorProcess=admission_.operatorProcess;
      binding_.stream=admission_.stream;binding_.operation=admission_.operation;
      binding_.consoleSession=admission_.consoleSession;
      return true;
    }catch(...){return false;}
  }
  std::optional<LeaseProof> current(const PreparationAdmission& admission,
      const PreparationBinding& binding,Operation operation)const override{
    std::lock_guard lock(mutex_);return currentLocked(admission,binding,operation);
  }
  std::optional<BaselineProof> original(const PreparationAdmission& admission,
      const PreparationBinding& binding,Operation operation)const override{
    std::lock_guard lock(mutex_);
    if(!child_||!currentLocked(admission,binding,operation))return {};
    if(const auto bytes=original_.current())return BaselineProof{*bytes,original_.sha256()};
    if(operation==Operation::prepare&&admission_.expectedOriginalSha256.empty()&&original_.absent())return BaselineProof{};
    return {};
  }
  std::optional<BaselineProof> saveOriginal(const PreparationAdmission& admission,
      const PreparationBinding& binding,const std::string& bytes,
      const std::string& expectedSha256)override{
    std::lock_guard lock(mutex_);
    if(!child_||!currentLocked(admission,binding,Operation::prepare)||
        expectedSha256!=admission_.expectedOriginalSha256||!baselineMatches(bytes,expectedSha256.empty()))return {};
    auto saved=original_.current();
    if(expectedSha256.empty()){
      if(saved){if(*saved!=bytes)return {};}
      else if(!original_.create(bytes,stream()))return {};
    }else if(!saved||*saved!=bytes||original_.sha256()!=expectedSha256)return {};
    saved=original_.current();
    if(!saved||*saved!=bytes||!currentLocked(admission,binding,Operation::prepare))return {};
    return BaselineProof{*saved,original_.sha256()};
  }
  std::optional<LeaseProof> size(const PreparationAdmission& admission,
      const PreparationBinding& binding,uint32_t width,uint32_t height,
      Operation operation)override{
    std::lock_guard lock(mutex_);
    if(!child_||!currentLocked(admission,binding,operation))return {};
    const auto bytes=original_.current();if(!bytes)return {};
    const bool restoring=operation==Operation::cleanup;
    if(restoring){
      try{
        const auto saved=parseDisplayV2State(*bytes);uint32_t restoreWidth=originalTarget_.width,restoreHeight=originalTarget_.height;
        for(const auto& path:saved.ccd.paths)if(displayV2Active(path)&&nativeSameTarget(path,saved.owned.front())){
          const auto& source=nativeCcdSource(saved.ccd,path);restoreWidth=source.width;restoreHeight=source.height;break;
        }
        if(width!=restoreWidth||height!=restoreHeight)return {};
      }catch(...){return {};}
    }else if(width!=admission_.width||height!=admission_.height)return {};
    const bool response=restoring?
      display_.restorePreparationOriginalBoundSize(stream(),admission_.lease.instance,generation_,width,height).has_value():
      display_.setPreparationOriginalBoundSize(stream(),admission_.lease.instance,generation_,width,height).has_value();
    // Read the same retained controller after OUR size call. A refused SDK
    // operation may still have advanced its generation while rolling back.
    // Never infer that generation from the child's request or old snapshot.
    try{
      const auto snapshot=JsonParser::parse(display_.viewportSnapshot());
      if(snapshot.get("deviceInstanceId").string()!=admission_.lease.instance||
          !nativeDisplayInteger(snapshot.get("generation").number(),generation_,UINT32_MAX)){
        forwardFailed_=true;return {};
      }
      generation_=uint32_t(snapshot.get("generation").number());
    }catch(...){forwardFailed_=true;return {};}
    if(!response){forwardFailed_=true;return {};}
    const auto proof=currentLocked(admission,binding,operation);
    if(!proof||proof->targets.front().width!=width||proof->targets.front().height!=height||
        original_.current()!=bytes){forwardFailed_=true;return {};}
    return proof;
  }
  bool retain(const PreparationAdmission& admission,const PreparationBinding& binding,
      const Receipt& receipt)override{
    std::lock_guard lock(mutex_);
    const auto proof=currentLocked(admission,binding,Operation::prepare);
    if(!proof||receipt.binding!=binding_||receipt.outcome!=Outcome::prepared||
        !receipt.originalRetained||receipt.fullOriginalLayoutObserved||
        receipt.originalSha256!=original_.sha256()||!original_.current())return false;
    const auto& target=proof->targets.front();const auto& observed=receipt.target;
    if(target.width!=admission_.width||target.height!=admission_.height||observed.mask!=1||
        observed.left||observed.top||observed.width!=target.width||observed.height!=target.height||
        observed.adapterLow!=target.adapter.LowPart||observed.adapterHigh!=target.adapter.HighPart||
        observed.targetId!=target.target)return false;
    retained_=receipt;return true;
  }
  // No release on pipe EOF, an ACK, or a receipt alone. The exact held child
  // must have physically exited before the service may run final restoration.
  bool releaseOriginalForRestore(){
    std::lock_guard lock(mutex_);
    try{
      if(!child_||native::processIdentity(child_.get())!=binding_.worker||
          WaitForSingleObject(child_.get(),0)!=WAIT_OBJECT_0)return false;
      original_.releaseForRestore();return true;
    }catch(...){return false;}
  }
  std::optional<Receipt> retained()const{std::lock_guard lock(mutex_);return retained_;}
 private:
  [[noreturn]]static void invalid(){throw std::runtime_error("Broker sign-in preparation authority is unavailable");}
  static native::Handle queryHandle(HANDLE source){
    HANDLE handle=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&handle,
        PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))invalid();
    return native::Handle(handle);
  }
  std::string stream()const{return PreparedServiceAuthority::streamText(admission_.stream);}
  bool sameAdmission(const PreparationAdmission& value)const{
    return value.purpose==admission_.purpose&&value.service==admission_.service&&
      value.operatorProcess==admission_.operatorProcess&&value.lease==admission_.lease&&
      value.stream==admission_.stream&&value.operation==admission_.operation&&
      value.installedImageSha256==admission_.installedImageSha256&&
      value.consoleSession==admission_.consoleSession&&value.width==admission_.width&&
      value.height==admission_.height&&value.issuedAt==admission_.issuedAt&&
      value.expiresAt==admission_.expiresAt&&value.expectedOriginalSha256==admission_.expectedOriginalSha256&&
      value.serviceInstance==admission_.serviceInstance&&value.grantRevision==admission_.grantRevision&&
      value.eligibilityRevision==admission_.eligibilityRevision&&value.brokerDeadline==admission_.brokerDeadline;
  }
  bool initialFresh()const{return brokerLaunchDeadlineFresh(admission_,GetTickCount64());}
  bool liveService()const{
    return service_&&serviceImage_&&native::processIdentity(service_.get())==admission_.service&&
      WaitForSingleObject(service_.get(),0)==WAIT_TIMEOUT;
  }
  bool baselineMatches(const std::string& bytes,bool newOriginal)const{
    try{
      if(bytes.empty()||bytes.size()>65536)return false;
      const auto saved=parseDisplayV2State(bytes);
      if(newOriginal&&saved.session!=stream())return false;
      if(saved.owned.size()!=1||saved.owned.front().id!="0")return false;
      const auto& target=saved.owned.front();
      return nativeSameLuid(target.adapter,originalTarget_.adapter)&&target.target==originalTarget_.target&&
        target.width==originalTarget_.width&&target.height==originalTarget_.height;
    }catch(...){return false;}
  }
  std::optional<LeaseProof> leaseLocked(Operation operation=Operation::prepare)const{
    const auto convert=[&](const auto& proof)->std::optional<LeaseProof>{
      if(!proof||proof->service.pid!=admission_.service.pid||proof->service.created!=admission_.service.created||
          proof->targets.size()!=1||proof->targets.front().id!="0")return {};
      return LeaseProof{{proof->instance,proof->generation},proof->targets};
    };
    if(operation==Operation::cleanup)return convert(display_.preparationCleanupLease(stream(),admission_.lease.instance,generation_));
    return convert(display_.preparationLease(stream(),admission_.lease.instance,generation_));
  }
  std::optional<LeaseProof> currentLocked(const PreparationAdmission& admission,
      const PreparationBinding& binding,Operation operation)const{
    try{
      const auto now=GetTickCount64();
      if(!sameAdmission(admission)||binding.service!=admission_.service||
          binding.operatorProcess!=admission_.operatorProcess||binding.stream!=admission_.stream||binding.operation!=admission_.operation||
          binding.consoleSession!=admission_.consoleSession||
          admission_.service!=native::processIdentity(GetCurrentProcess()))return {};
      const bool cleanup=operation==Operation::cleanup;
      if(operation!=Operation::prepare&&!cleanup)return {};
      if(cleanup){
        if(!child_||binding!=binding_||!original_.current()||
            now<admission_.issuedAt||now-admission_.issuedAt>90000)return {};
      }else if(forwardFailed_||!validAdmission(admission_,now)||!liveService()||!consent_(admission_))return {};
      if(child_){
        if(binding!=binding_||native::processIdentity(child_.get())!=binding_.worker||
            WaitForSingleObject(child_.get(),0)!=WAIT_TIMEOUT)return {};
      }else if(cleanup||!initialFresh()||valid(binding.worker)||binding.worker.pid||binding.worker.created)return {};
      const auto proof=leaseLocked(operation);
      if(!proof||!liveService()||(!cleanup&&(!consent_(admission_)||(!child_&&!initialFresh()))))return {};
      return proof;
    }catch(...){return {};}
  }
  NativeDisplayService& display_;const PreparationAdmission admission_;OriginalFile& original_;LiveGrant consent_;
  native::Handle service_,serviceImage_,child_;PreparationBinding binding_;
  NativeDisplayTarget originalTarget_;uint32_t generation_=0;bool forwardFailed_=false;
  std::optional<Receipt> retained_;mutable std::mutex mutex_;
};
}
