#pragma once
#include "signin_restoration.hpp"
#include "signin_original_file.hpp"
#include "native_display_service.hpp"
#include "signin_service_authority.hpp"

namespace host::signin::restoration {
// The service retains this original/controller obligation after the operator
// leaves. No remote request can manufacture predecessor handles or a baseline.
class ServiceAuthority final : public Authority {
 public:
  ServiceAuthority(NativeDisplayService& display,RestorationAdmission admission,
      std::span<const HANDLE> exactPredecessors,preparation::OriginalFile& original)
    :display_(display),admission_(std::move(admission)),original_(original),generation_(admission_.lease.generation){
    if(!validAdmission(admission_,GetTickCount64())||
        admission_.service!=native::processIdentity(GetCurrentProcess())||
        exactPredecessors.size()!=admission_.predecessors.size())invalid();
    for(size_t i=0;i<exactPredecessors.size();++i){
      if(native::processIdentity(exactPredecessors[i])!=admission_.predecessors[i]||
          WaitForSingleObject(exactPredecessors[i],0)!=WAIT_OBJECT_0)invalid();
      predecessors_.push_back(duplicateQuery(exactPredecessors[i]));
    }
    if(!predecessorsExited()||!leaseLocked())invalid();
    bool adopted=false;
    try{
      if(!original_.current()){
        if(!original_.adopt(admission_.originalSha256))invalid();
        adopted=true;
      }
      const auto bytes=original_.current();
      if(!bytes||original_.sha256()!=admission_.originalSha256)invalid();
      const auto saved=parseDisplayV2State(*bytes);
      if(saved.owned.size()!=1||saved.owned.front().id!="0")invalid();
      restoreWidth_=saved.owned.front().width;restoreHeight_=saved.owned.front().height;
      for(const auto& path:saved.ccd.paths)if(displayV2Active(path)&&nativeSameTarget(path,saved.owned.front())){
        const auto& source=nativeCcdSource(saved.ccd,path);restoreWidth_=source.width;restoreHeight_=source.height;break;
      }
      if(!ownedOutputSize(restoreWidth_,restoreHeight_)||!predecessorsExited()||!leaseLocked())invalid();
    }catch(...){if(adopted)original_.releaseForRestore();throw;}
  }
  bool bindChild(HANDLE exactOwnedChild){
    std::lock_guard lock(mutex_);
    try{
      if(child_||!validAdmission(admission_,GetTickCount64())||!predecessorsExited()||!leaseLocked())return false;
      const auto process=native::processIdentity(exactOwnedChild);
      if(!valid(process)||process==admission_.service||
          std::find(admission_.predecessors.begin(),admission_.predecessors.end(),process)!=admission_.predecessors.end()||
          WaitForSingleObject(exactOwnedChild,0)!=WAIT_TIMEOUT)return false;
      child_=duplicateQuery(exactOwnedChild);binding_.service=admission_.service;binding_.worker=process;
      binding_.stream=admission_.stream;binding_.operation=admission_.operation;binding_.consoleSession=admission_.consoleSession;
      return true;
    }catch(...){return false;}
  }
  std::optional<LeaseProof> current(const RestorationAdmission& admission,const RestorationBinding& binding)const override{
    std::lock_guard lock(mutex_);return currentLocked(admission,binding);
  }
  std::optional<OriginalProof> original(const RestorationAdmission& admission,const RestorationBinding& binding)const override{
    std::lock_guard lock(mutex_);
    if(!currentLocked(admission,binding))return {};
    const auto bytes=original_.current();if(!bytes)return {};
    return OriginalProof{*bytes,admission_.originalSha256};
  }
  std::optional<LeaseProof> sizeOriginal(const RestorationAdmission& admission,const RestorationBinding& binding)override{
    std::lock_guard lock(mutex_);
    if(!child_||!currentLocked(admission,binding))return {};
    const auto bytes=original_.current();if(!bytes)return {};
    const auto stream=PreparedServiceAuthority::streamText(admission_.stream);
    const auto response=display_.restorePreparationOriginalBoundSize(stream,admission_.lease.instance,generation_,restoreWidth_,restoreHeight_);
    try{
      const auto snapshot=JsonParser::parse(display_.viewportSnapshot());
      if(snapshot.get("deviceInstanceId").string()!=admission_.lease.instance||
          !nativeDisplayInteger(snapshot.get("generation").number(),generation_,UINT32_MAX))return {};
      generation_=uint32_t(snapshot.get("generation").number());
    }catch(...){return {};}
    const auto proof=currentLocked(admission,binding);
    if(!response||!proof||proof->targets.front().width!=restoreWidth_||proof->targets.front().height!=restoreHeight_||
        original_.current()!=bytes)return {};
    return proof;
  }
  bool finish(const Receipt& receipt){
    std::lock_guard lock(mutex_);
    try{
      if(completed_||!child_||native::processIdentity(child_.get())!=binding_.worker||
          WaitForSingleObject(child_.get(),0)!=WAIT_OBJECT_0||!predecessorsExited()||
          !validAdmission(admission_,GetTickCount64()))return false;
      const auto proof=leaseLocked();auto bytes=original_.current();
      if(proof&&!bytes&&original_.adopt(admission_.originalSha256))bytes=original_.current();
      if(!proof||!bytes||receipt.fallback>1||original_.sha256()!=admission_.originalSha256||
          !verifiedReceipt(admission_,binding_,{*bytes,admission_.originalSha256},*proof,receipt,GetTickCount64()))return false;
      const auto stillRestored=[&]{
        const auto current=leaseLocked();
        return validAdmission(admission_,GetTickCount64())&&predecessorsExited()&&
          native::processIdentity(child_.get())==binding_.worker&&WaitForSingleObject(child_.get(),0)==WAIT_OBJECT_0&&
          current&&current->lease==proof->lease&&current->targets.size()==1&&
          current->targets.front().id==proof->targets.front().id&&
          nativeSameLuid(current->targets.front().adapter,proof->targets.front().adapter)&&
          current->targets.front().target==proof->targets.front().target&&
          current->targets.front().width==proof->targets.front().width&&
          current->targets.front().height==proof->targets.front().height&&
          verifiedReceipt(admission_,binding_,{*bytes,admission_.originalSha256},*current,receipt,GetTickCount64());
      };
      if(!original_.retireRestored(admission_.originalSha256,stillRestored))return false;
      completed_=true;return true;
    }catch(...){return false;}
  }
  bool completed()const{std::lock_guard lock(mutex_);return completed_;}
 private:
  [[noreturn]]static void invalid(){throw std::runtime_error("Sign-in restoration authority is unavailable");}
  static native::Handle duplicateQuery(HANDLE source){
    HANDLE value=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&value,
        PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0))invalid();
    return native::Handle(value);
  }
  bool predecessorsExited()const{
    if(predecessors_.size()!=admission_.predecessors.size())return false;
    for(size_t i=0;i<predecessors_.size();++i)if(native::processIdentity(predecessors_[i].get())!=admission_.predecessors[i]||
        WaitForSingleObject(predecessors_[i].get(),0)!=WAIT_OBJECT_0)return false;
    return true;
  }
  std::optional<LeaseProof> leaseLocked()const{
    if(admission_.service!=native::processIdentity(GetCurrentProcess()))return {};
    const auto stream=PreparedServiceAuthority::streamText(admission_.stream);
    const auto proof=display_.preparationCleanupLease(stream,admission_.lease.instance,generation_);
    if(!proof||proof->service.pid!=admission_.service.pid||proof->service.created!=admission_.service.created||
        proof->targets.size()!=1||proof->targets.front().id!="0")return {};
    return LeaseProof{{proof->instance,proof->generation},proof->targets};
  }
  bool sameAdmission(const RestorationAdmission& value)const{
    return value.purpose==admission_.purpose&&value.service==admission_.service&&value.stream==admission_.stream&&
      value.lease==admission_.lease&&value.operation==admission_.operation&&value.installedImageSha256==admission_.installedImageSha256&&
      value.predecessors==admission_.predecessors&&value.originalSha256==admission_.originalSha256&&
      value.consoleSession==admission_.consoleSession&&value.issuedAt==admission_.issuedAt&&value.expiresAt==admission_.expiresAt;
  }
  std::optional<LeaseProof> currentLocked(const RestorationAdmission& admission,const RestorationBinding& binding)const{
    try{
      if(completed_||!sameAdmission(admission)||!validAdmission(admission_,GetTickCount64())||!predecessorsExited()||
          binding.service!=admission_.service||binding.stream!=admission_.stream||binding.operation!=admission_.operation||
          binding.consoleSession!=admission_.consoleSession||original_.sha256()!=admission_.originalSha256||!original_.current())return {};
      if(child_){
        if(binding!=binding_||native::processIdentity(child_.get())!=binding_.worker||WaitForSingleObject(child_.get(),0)!=WAIT_TIMEOUT)return {};
      }else if(binding.worker.pid||binding.worker.created)return {};
      return leaseLocked();
    }catch(...){return {};}
  }
  NativeDisplayService& display_;const RestorationAdmission admission_;preparation::OriginalFile& original_;
  std::vector<native::Handle> predecessors_;native::Handle child_;RestorationBinding binding_;
  uint32_t generation_=0,restoreWidth_=0,restoreHeight_=0;bool completed_=false;mutable std::mutex mutex_;
};
}
