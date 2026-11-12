#pragma once
#include "machine.hpp"
#include "gpu_h264.hpp"
#include "encoder_diagnostics.hpp"
#include "encoder_settings_policy.hpp"
#include "operational_log.hpp"
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")
namespace host {
inline std::string encoderSettingsHash(const std::string& bytes){
 BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;DWORD length=0,written=0;std::vector<UCHAR> object;std::array<UCHAR,32> digest{};
 const auto cleanup=[&]{if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);};
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0){cleanup();throw std::runtime_error("encoder_settings_unavailable");}
 if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&written,0)<0||!length||length>65536){cleanup();throw std::runtime_error("encoder_settings_unavailable");}try{object.resize(length);}catch(...){cleanup();throw;}
 const bool ok=BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)>=0&&BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),ULONG(bytes.size()),0)>=0&&BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0;cleanup();
 if(!ok)throw std::runtime_error("encoder_settings_unavailable");constexpr char hex[]="0123456789abcdef";std::string out;for(const auto byte:digest){out+=hex[byte>>4];out+=hex[byte&15];}return out;
}
inline std::filesystem::path encoderSettingsPath(){return machineDirectory()/L"encoder.json";}
struct EncoderSettingsSnapshot {Json value;std::string bytes;EncoderSettingsChoice choice;};
inline void encoderSettingsDirectory(bool create){
 const auto parent=machineDirectory().parent_path(),directory=machineDirectory();
 for(const auto& path:{parent,directory}){const auto attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){if(!create)continue;PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error("encoder_settings_unavailable");SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};const bool made=CreateDirectoryW(path.c_str(),&security)!=FALSE;const DWORD error=GetLastError();LocalFree(descriptor);if((!made&&error!=ERROR_ALREADY_EXISTS)||!trustedMachineFile(path))throw std::runtime_error("encoder_settings_untrusted");}else if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)||!trustedMachineFile(path))throw std::runtime_error("encoder_settings_untrusted");}
}
inline EncoderSettingsSnapshot encoderSettingsRead(){
 encoderSettingsDirectory(false);EncoderSettingsSnapshot result;const auto path=encoderSettingsPath();const auto attributes=GetFileAttributesW(path.c_str());
 if(attributes==INVALID_FILE_ATTRIBUTES){if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)throw std::runtime_error("encoder_settings_unavailable");result.value.value=Json::Object{};result.choice.revision=encoderSettingsHash("Multitap.encoder-settings.absent.v1");return result;}
 if((attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!trustedMachineFile(path))throw std::runtime_error("encoder_settings_untrusted");
 std::ifstream file(path,std::ios::binary);for(char c;file.get(c);){if(result.bytes.size()>=8192)throw std::runtime_error("encoder_settings_invalid");result.bytes+=c;}if(!file.eof())throw std::runtime_error("encoder_settings_unavailable");
 result.value=JsonParser::parse(result.bytes);if(!std::holds_alternative<Json::Object>(result.value.value))throw std::runtime_error("encoder_settings_invalid");result.choice.revision=encoderSettingsHash(result.bytes);
 const auto& fields=std::get<Json::Object>(result.value.value);const auto found=fields.find("videoEncoder");
 if(found!=fields.end()&&!std::holds_alternative<std::string>(found->second.value))throw std::runtime_error("encoder_settings_invalid");
 const auto backend=found==fields.end()?std::string("auto"):found->second.string();
 if(backend!="auto"&&backend!="x264"&&backend!="mft"&&backend!="gpu"&&backend!="nvenc"&&backend!="amf"&&backend!="qsv"&&backend!="qsv-mft")throw std::runtime_error("encoder_settings_invalid");
 result.choice.backend=backend=="auto"?"auto":backend=="mft"?"mft":backend=="x264"?"x264":"gpu";
 if(result.choice.backend=="gpu"){result.choice.adapter=gpu::parseAdapterLuid(result.value.get("gpuAdapter").string());if(result.choice.adapter&&!*result.choice.adapter)result.choice.adapter.reset();}
 const auto id=result.value.get("multitapEncoderOperation").string();if(encoderSettingsUuid(id))result.choice.operationId=id;return result;
}
inline std::string encoderSettingsJson(const EncoderSettingsSnapshot& current,const std::vector<gpu::Adapter>& adapters,bool inventoryKnown){
 std::string rows="[";size_t count=0;for(const auto& adapter:adapters){if(!adapter.luid)continue;if(++count>32)throw std::runtime_error("encoder_inventory_unavailable");if(rows.size()>1)rows+=',';
 rows+="{\"id\":"+jsonQuote(gpu::adapterLuidText(adapter.luid))+",\"vendorId\":"+std::to_string(adapter.vendorId)+",\"deviceId\":"+std::to_string(adapter.deviceId)+",\"name\":"+(encoderAdapterName(adapter.description)?jsonQuote(adapter.description):"null")+",\"candidate\":"+(gpu::vendorFromId(adapter.vendorId)!=gpu::Vendor::unknown?"true":"false")+'}';}rows+=']';
 std::string recent="null";
 // A missing/evicted/unreadable journal is unknown, never evidence of CPU use.
 try{const auto journal=JsonParser::parse(hostLogSnapshot());if(const auto* entries=std::get_if<Json::Array>(&journal.value))for(const auto& entry:*entries){const auto& fields=entry.get("values");if(entry.get("event").string()!="hostAction"||fields.get("action").number()!=254||!fields.get("active").boolean())continue;
   const auto stage=fields.get("stage").number(),fallback=fields.get("fallback").number(-1),software=fields.get("error").number(-1);if(!std::isfinite(stage)||std::floor(stage)!=stage||stage<1||stage>5||fallback<0||fallback>44||std::floor(fallback)!=fallback||!encoderGpuCode(uint32_t(fallback))||software<0||software>7||std::floor(software)!=software)continue;
   const std::string backend=encoderUsageBackend(uint32_t(stage));const bool verified=fields.get("owned").boolean();const auto track=fields.get("items").number(-1);if(!fields.get("ok").boolean()||verified!=(encoderVendor(backend)!=0)||(track!=0&&track!=1)||!safeHostLogTime(entry.get("time").string()))continue;
   recent="{\"protocol\":\"host-encoder-usage-v1\",\"trackId\":"+jsonQuote(track==0?"0":"1")+",\"time\":"+jsonQuote(entry.get("time").string())+",\"backend\":"+jsonQuote(backend)+",\"hardwareVerified\":"+(verified?"true":"false")+",\"gpuFallbackCode\":"+std::to_string(uint32_t(fallback))+",\"softwareFallbackCode\":"+std::to_string(uint32_t(software))+'}';
 }}catch(...){ }
 return "{\"schema\":1,\"supported\":true,\"autoSupported\":true,\"administrator\":"+std::string(elevated()?"true":"false")+",\"revision\":"+jsonQuote(current.choice.revision)+",\"backend\":"+jsonQuote(current.choice.backend)+",\"adapterId\":"+(current.choice.adapter?jsonQuote(gpu::adapterLuidText(*current.choice.adapter)):"null")+",\"operationId\":"+(current.choice.operationId.empty()?"null":jsonQuote(current.choice.operationId))+",\"inventoryKnown\":"+(inventoryKnown?"true":"false")+",\"inventoryIsEncoderSupport\":false,\"adapters\":"+rows+",\"recentUsage\":"+recent+'}';
}
inline std::string encoderSettingsStatus(){
 const auto current=encoderSettingsRead();std::vector<gpu::Adapter> adapters;bool known=false;try{adapters=gpu::enumerateAdapters();known=true;}catch(...){ }
 return encoderSettingsJson(current,adapters,known);
}
inline Json encoderSettingsText(std::string value){Json out;out.value=std::move(value);return out;}
inline std::string applyEncoderSettings(const Json& request){
 if(!elevated())throw std::runtime_error("administrator_required");const auto* fields=std::get_if<Json::Object>(&request.value);const std::set<std::string> allowed{"action","operationId","expectedRevision","backend","adapterId","confirmed"};
 if(!fields||fields->size()!=allowed.size())throw std::runtime_error("encoder_settings_invalid_request");for(const auto& [key,value]:*fields){(void)value;if(!allowed.count(key))throw std::runtime_error("encoder_settings_invalid_request");}
 EncoderSettingsRequest input;input.operationId=request.get("operationId").string();input.expectedRevision=request.get("expectedRevision").string();input.backend=request.get("backend").string();const auto* consent=std::get_if<bool>(&request.get("confirmed").value);input.confirmed=consent&&*consent;
 if(const auto* text=std::get_if<std::string>(&request.get("adapterId").value)){input.adapter=gpu::parseAdapterLuid(*text);if(!input.adapter||gpu::adapterLuidText(*input.adapter)!=*text)throw std::runtime_error("encoder_settings_invalid_request");}else if(!std::holds_alternative<std::nullptr_t>(request.get("adapterId").value))throw std::runtime_error("encoder_settings_invalid_request");
 if(const std::string error=encoderSettingsRequestError(input);!error.empty())throw std::runtime_error(error);
 // Product-owned lockfile, no global settings/mutex ACL or active worker changes.
 encoderSettingsDirectory(true);const auto lockPath=machineDirectory()/L"encoder-settings.lock";if(std::filesystem::exists(lockPath)&&!trustedMachineFile(lockPath))throw std::runtime_error("encoder_settings_untrusted");
 PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GR;;;BU)",SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error("encoder_settings_unavailable");
 SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};const auto lock=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,&security,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);LocalFree(descriptor);
 if(lock==INVALID_HANDLE_VALUE)throw std::runtime_error("encoder_settings_busy");struct Lock{HANDLE value;~Lock(){CloseHandle(value);}}held{lock};if(!trustedMachineFile(lockPath))throw std::runtime_error("encoder_settings_untrusted");
 auto current=encoderSettingsRead();std::vector<gpu::Adapter> adapters;bool inventoryKnown=false;try{adapters=gpu::enumerateAdapters();inventoryKnown=true;}catch(...){ }
 std::vector<uint64_t> candidates;for(const auto& adapter:adapters)if(adapter.luid&&gpu::vendorFromId(adapter.vendorId)!=gpu::Vendor::unknown)candidates.push_back(adapter.luid);
 const std::string plan=encoderSettingsPlan(current.choice,input,candidates);if(plan!="apply"&&plan!="duplicate")throw std::runtime_error(plan);if(plan=="duplicate")return encoderSettingsJson(current,adapters,inventoryKnown);
 auto* values=std::get_if<Json::Object>(&current.value.value);(*values)["videoEncoder"]=encoderSettingsText(input.backend);if(input.adapter)(*values)["gpuAdapter"]=encoderSettingsText(gpu::adapterLuidText(*input.adapter));else values->erase("gpuAdapter");(*values)["multitapEncoderOperation"]=encoderSettingsText(input.operationId);
 const auto bytes=jsonObject(current.value);if(bytes.size()>8192)throw std::runtime_error("encoder_settings_invalid");const auto path=encoderSettingsPath(),temporary=std::filesystem::path(path.wstring()+temporarySuffix());
 descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GR;;;BU)",SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error("encoder_settings_unavailable");security.lpSecurityDescriptor=descriptor;
 HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,0,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);LocalFree(descriptor);if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("encoder_settings_save_failed");
 DWORD written=0;const bool saved=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);if(!saved){FILE_DISPOSITION_INFO remove{TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&remove,sizeof(remove));CloseHandle(file);throw std::runtime_error("encoder_settings_save_failed");}CloseHandle(file);
 if(encoderSettingsRead().choice.revision!=input.expectedRevision){DeleteFileW(temporary.c_str());throw std::runtime_error("encoder_settings_changed");}
 if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());throw std::runtime_error("encoder_settings_save_failed");}
 const auto after=encoderSettingsRead();if(after.choice.operationId!=input.operationId||after.choice.backend!=input.backend||after.choice.adapter!=input.adapter)throw std::runtime_error("encoder_settings_result_unknown");
 return encoderSettingsJson(after,adapters,inventoryKnown);
}
}
