#pragma once
#include "native_display_baseline_codec.hpp"
#include "native_display_journal.hpp"
#include "native_display_digest.hpp"
namespace host {
inline NativeRuntimeJournalAccess nativeBaselineFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){return nativeJournalFileAccess(path,nativeBaselineByteLimit,[](const std::string& bytes){(void)parseNativeRecoveryBaseline(bytes);},std::move(trusted));}
class NativeBaselineJournal {
 public:
  explicit NativeBaselineJournal(NativeRuntimeJournalAccess access):access_(std::move(access)){}
  std::optional<NativeRecoveryBaseline> load(const std::string& instance){
    if(!nativeDisplayInstanceId(instance))throw std::runtime_error("native_baseline_invalid");
    if(loaded_){if(instance!=instance_)throw std::runtime_error("native_baseline_device_changed");return record_;}
    expected_=access_.load();if(expected_){record_=parseNativeRecoveryBaseline(*expected_);if(record_->instance!=instance)throw std::runtime_error("native_baseline_device_changed");}instance_=instance;loaded_=true;return record_;
  }
  bool store(const NativeRecoveryBaseline& record)noexcept{
    try{if(!loaded_||record.instance!=instance_||(record_&&record.generation<record_->generation))return false;const auto bytes=nativeRecoveryBaselineBytes(record);if(!access_.save(expected_,bytes))return false;expected_=bytes;record_=record;return true;}catch(...){return false;}
  }
  std::string digest()const{if(!loaded_||!expected_||!record_)throw std::runtime_error("native_baseline_missing");return nativeBaselineDigest(*expected_);}
  // Same durable store, no cached record (after an administrator record reset).
  NativeBaselineJournal reopened()const{return NativeBaselineJournal(access_);}
 private:NativeRuntimeJournalAccess access_;bool loaded_=false;std::string instance_;std::optional<std::string> expected_;std::optional<NativeRecoveryBaseline> record_;
};
}
