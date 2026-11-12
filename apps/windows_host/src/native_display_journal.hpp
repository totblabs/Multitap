#pragma once
#include "native_display_change.hpp"
#include "owned_output_size.hpp"
namespace host {
struct NativeRuntimeOutput {std::string id;DWORD width=0,height=0;friend bool operator==(const NativeRuntimeOutput&,const NativeRuntimeOutput&)=default;};
struct NativeRuntimeRecord {
  std::string instance,phase;uint32_t generation=0;std::vector<NativeRuntimeOutput> outputs;std::string baselineHash;
  bool uncertain()const{return phase=="starting"||phase=="changing"||phase=="recovery";}
};
inline NativeRuntimeRecord parseNativeRuntimeRecord(const std::string& bytes){
  if(bytes.empty()||bytes.size()>4096)throw std::runtime_error("native_display_journal_invalid");const auto value=JsonParser::parse(bytes);
  const auto* object=std::get_if<Json::Object>(&value.value);const auto* outputs=std::get_if<Json::Array>(&value.get("outputs").value);NativeRuntimeRecord record;
  record.instance=value.get("deviceInstanceId").string();record.phase=value.get("phase").string();const auto generation=value.get("generation").number();
  const auto schema=value.get("schema").number();if(schema==2){if(!std::holds_alternative<std::string>(value.get("baselineSha256").value))throw std::runtime_error("native_display_journal_invalid");record.baselineHash=value.get("baselineSha256").string();}
  if(!object||!((schema==1&&object->size()==6)||(schema==2&&object->size()==7))||value.get("provider").string()!="libvirtualdisplay-1.6.4"||!nativeDisplayInstanceId(record.instance)||!nativeDisplayInteger(generation,1,UINT32_MAX)||!outputs||outputs->empty()||outputs->size()>2||(record.phase!="stable"&&record.phase!="stopped"&&record.phase!="starting"&&record.phase!="changing"&&record.phase!="recovery")||(!record.baselineHash.empty()&&(record.baselineHash.size()!=64||record.baselineHash.find_first_not_of("0123456789abcdef")!=std::string::npos||record.phase=="stable"||record.phase=="stopped")))throw std::runtime_error("native_display_journal_invalid");
  record.generation=uint32_t(generation);
  for(size_t i=0;i<outputs->size();++i){const auto& item=(*outputs)[i];const auto* fields=std::get_if<Json::Object>(&item.value);const auto id=item.get("id").string();const auto width=item.get("width").number(),height=item.get("height").number();
    if(!fields||fields->size()!=3||id!=std::to_string(i)||!nativeDisplayInteger(width,320,ownedOutputMaxSide)||!nativeDisplayInteger(height,180,ownedOutputMaxSide)||!ownedOutputSize(uint64_t(width),uint64_t(height)))throw std::runtime_error("native_display_journal_invalid");record.outputs.push_back({id,DWORD(width),DWORD(height)});
  }return record;
}
inline std::string nativeRuntimeBytes(const NativeRuntimeRecord& record){
  std::string outputs="[";for(const auto& output:record.outputs){if(outputs.size()>1)outputs+=',';outputs+="{\"id\":"+jsonQuote(output.id)+",\"width\":"+std::to_string(output.width)+",\"height\":"+std::to_string(output.height)+'}';}
  auto bytes="{\"schema\":2,\"provider\":\"libvirtualdisplay-1.6.4\",\"deviceInstanceId\":"+jsonQuote(record.instance)+",\"generation\":"+std::to_string(record.generation)+",\"phase\":"+jsonQuote(record.phase)+",\"baselineSha256\":"+jsonQuote(record.baselineHash)+",\"outputs\":"+outputs+"]}";(void)parseNativeRuntimeRecord(bytes);return bytes;
}
inline uint32_t nativeDisplayCleanupRuntimeGeneration(const std::string& instance,uint32_t heldGeneration,const std::optional<std::string>& bytes){
  if(!heldGeneration||!nativeDisplayInstanceId(instance)||!bytes)throw std::runtime_error("native_display_snapshot_invalid");
  const auto runtime=parseNativeRuntimeRecord(*bytes);
  if(runtime.instance!=instance||runtime.phase!="stable"||runtime.generation<heldGeneration)throw std::runtime_error("native_display_snapshot_invalid");
  return runtime.generation;
}
struct NativeRuntimeJournalAccess {
  std::function<std::optional<std::string>()> load;
  std::function<bool(const std::optional<std::string>&,const std::string&)> save;
};
inline std::optional<std::string> readNativeJournalFile(const std::filesystem::path& path,const std::function<bool(const std::filesystem::path&)>& trusted,size_t maximum){
  // Only this typed receipt carries an escaped, bounded immutable profile.
  // All existing journals retain their original 64 KiB ceiling.
  const bool viewportReceipt=maximum==131072&&path.filename()==L"native-stream-viewport-commit.json";
  if(!maximum||(maximum>65536&&!viewportReceipt))throw std::runtime_error("native_display_journal_invalid");
  if(!trusted(path.parent_path()))throw std::runtime_error("native_display_journal_untrusted");
  const auto attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){if(GetLastError()==ERROR_FILE_NOT_FOUND)return {};throw std::runtime_error("native_display_journal_unreadable");}
  if((attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!trusted(path))throw std::runtime_error("native_display_journal_untrusted");
  WinHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
  if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nFileSizeHigh||info.nFileSizeLow>maximum)throw std::runtime_error("native_display_journal_unreadable");
  std::string bytes(info.nFileSizeLow,'\0');DWORD read=0;if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())throw std::runtime_error("native_display_journal_unreadable");return bytes;
}
inline std::optional<std::string> readNativeRuntimeFile(const std::filesystem::path& path,const std::function<bool(const std::filesystem::path&)>& trusted){return readNativeJournalFile(path,trusted,4096);}
inline NativeRuntimeJournalAccess nativeJournalFileAccess(const std::filesystem::path& path,size_t maximum,std::function<void(const std::string&)> validate,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  return {[path,trusted,maximum]{return readNativeJournalFile(path,trusted,maximum);},[path,trusted,maximum,validate](const std::optional<std::string>& expected,const std::string& bytes){
    std::filesystem::path temporary;bool created=false;
    try{
      if(bytes.size()>maximum)return false;validate(bytes);if(readNativeJournalFile(path,trusted,maximum)!=expected)return false;
      temporary=path.wstring()+temporarySuffix();WinHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));created=file.value!=INVALID_HANDLE_VALUE;DWORD written=0;
      if(file.value==INVALID_HANDLE_VALUE||!WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.value))throw std::runtime_error("native_display_journal_write_failed");file=WinHandle();
      if(!trusted(temporary)||readNativeJournalFile(path,trusted,maximum)!=expected||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH|(expected?MOVEFILE_REPLACE_EXISTING:0)))throw std::runtime_error("native_display_journal_write_failed");return true;
    }catch(...){if(created&&!temporary.empty())DeleteFileW(temporary.c_str());return false;}
  }};
}
inline NativeRuntimeJournalAccess nativeRuntimeFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){return nativeJournalFileAccess(path,4096,[](const std::string& bytes){(void)parseNativeRuntimeRecord(bytes);},std::move(trusted));}
class NativeRuntimeJournal {
 public:
  explicit NativeRuntimeJournal(NativeRuntimeJournalAccess access):access_(std::move(access)){}
  std::optional<NativeRuntimeRecord> load(const std::string& instance){if(!nativeDisplayInstanceId(instance))throw std::runtime_error("native_display_journal_invalid");if(loaded_){if(instance_!=instance)throw std::runtime_error("native_display_journal_device_changed");return record_;}expected_=access_.load();if(expected_){record_=parseNativeRuntimeRecord(*expected_);if(record_->instance!=instance)throw std::runtime_error("native_display_journal_device_changed");}instance_=instance;loaded_=true;return record_;}
  bool store(const NativeRuntimeRecord& record)noexcept{try{if(!loaded_||record.instance!=instance_||(record_&&record.generation<record_->generation))return false;const auto bytes=nativeRuntimeBytes(record);if(!access_.save(expected_,bytes))return false;expected_=bytes;record_=record;return true;}catch(...){return false;}}
  const std::optional<NativeRuntimeRecord>& record()const{return record_;}
  // Read-only CAS observation for operations that must not rewrite runtime.
  std::optional<std::string> verifiedBytes()const noexcept{try{if(!loaded_||!record_||!expected_||access_.load()!=expected_)return {};return expected_;}catch(...){return {};}}
  // Same durable store, no cached record (after an administrator record reset).
  NativeRuntimeJournal reopened()const{return NativeRuntimeJournal(access_);}
 private:NativeRuntimeJournalAccess access_;bool loaded_=false;std::string instance_;std::optional<std::string> expected_;std::optional<NativeRuntimeRecord> record_;
};
}
