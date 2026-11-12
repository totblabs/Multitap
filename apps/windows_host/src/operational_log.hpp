#pragma once
#include "machine.hpp"
#include "stream_observability.hpp"
#include <deque>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <mutex>
namespace host {
inline bool operationalLogEnabled=false;
inline bool hostLogEvent(const std::string& name) {
  static const std::set<std::string> allowed{"installerStart","installerStage","installerResult","displayObserved","driverPreserved","driverInstall","displayConfigure","modeCatalog","serviceState","brokerState","hostAction"};
  return allowed.count(name)!=0;
}
inline bool hostLogField(const std::string& name) {
  static const std::set<std::string> allowed{"ok","action","stage","installed","started","active","owned","pending","recovery","reboot","remote","monitors","running","registered","online","width","height","error","changed","restartAttempted",
    "expected","missing","missingOwned","fallback","consoleExpected","consoleActive","stale","remapped",
    // hostAction 240: media packets the broker channel's send queue skipped.
    "dropped",
    // hostAction 253 (clipboard_diagnostic.hpp): format, item count, KiB and the internal cause of a clipboard transfer.
    "format","items","kib","cause","win32",
    // hostAction 200 stage 10 (connect_stages.hpp): a stream start's stages in ms.
    "admitMs","workerMs","displayMs","mediaMs","frameMs",
    // hostAction 274 (boot_timeline.hpp): Windows uptime at service start and each boot stage in ms after it.
    "uptimeMs","configMs","brokerMs","virtualDisplayMs","logonMs","sessionWorkerMs","readyMs",
    // hostAction 275 (process_exit_record.hpp): how long the process that ended had run.
    "runSeconds"};
  return allowed.count(name)!=0;
}
inline bool hostLogBoolean(const std::string& name) {
  static const std::set<std::string> numbers{"action","stage","monitors","width","height","error","expected","missing","missingOwned","fallback","stale","dropped","format","items","kib","cause","win32","admitMs","workerMs","displayMs","mediaMs","frameMs",
    "uptimeMs","configMs","brokerMs","virtualDisplayMs","logonMs","sessionWorkerMs","readyMs","runSeconds"};
  return numbers.count(name)==0;
}
inline std::string hostLogTime() {
  SYSTEMTIME now{};GetSystemTime(&now);char text[32]{};
  sprintf_s(text,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
  return text;
}
inline bool safeHostLogTime(const std::string& value) {
  if(value.size()!=24||value[4]!='-'||value[7]!='-'||value[10]!='T'||value[13]!=':'||value[16]!=':'||value[19]!='.'||value[23]!='Z')return false;
  for(size_t i=0;i<value.size();++i)if(i!=4&&i!=7&&i!=10&&i!=13&&i!=16&&i!=19&&i!=23&&(value[i]<'0'||value[i]>'9'))return false;
  return true;
}
inline bool hostObservationSession(const std::string& value) {
  if(value.size()!=36)return false;
  for(size_t n=0;n<value.size();++n){const char c=value[n];if(n==8||n==13||n==18||n==23){if(c!='-')return false;}else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;}
  return true;
}
inline std::string safeHostObservationLogRecord(const Json& value) {
  const auto source=value.get("source").string(),session=value.get("sessionId").string(),time=value.get("time").string();
  if(value.get("event").string()!="streamObservation"||value.get("aggregation").string()!="latest-snapshot"||!safeHostLogTime(time)||!hostObservationSession(session)||(source!="host-service"&&source!="host-worker"))return {};
  const auto integer=[](const Json& item,uint64_t maximum)->std::optional<uint64_t>{const auto* n=std::get_if<double>(&item.value);if(!n||!std::isfinite(*n)||*n<0||*n>maximum||std::floor(*n)!=*n)return {};return uint64_t(*n);};
  const auto window=integer(value.get("windowMs"),4294967295),samples=integer(value.get("samples"),4096),dropped=integer(value.get("droppedRecords"),4294967295);
  const auto* final=std::get_if<bool>(&value.get("final").value);if(!window||!samples||!*samples||!dropped||!final)return {};
  const bool tracks=!std::holds_alternative<std::nullptr_t>(value.get("topologyEpoch").value);
  const auto epoch=integer(value.get(tracks?"topologyEpoch":"epoch"),tracks?4294967295ULL:65535ULL);
  if(!epoch||(tracks&&!std::holds_alternative<std::nullptr_t>(value.get("epoch").value)))return {};
  const auto* fields=std::get_if<Json::Object>(&value.get("values").value);if(!fields)return {};
  std::string metrics="{";
  for(const auto& [key,item]:*fields){const auto maximum=hostObservationMaximum(key);const bool worker=key=="workerCpuPct10"||key=="readbackUs"||key=="readbackMaxUs";
    if(!maximum||worker!=(source=="host-worker"))continue;const auto number=integer(item,*maximum);if(!number)continue;
    if(metrics.size()>1)metrics+=',';metrics+=jsonQuote(key)+':'+std::to_string(*number);
  }
  if(metrics.size()==1)return {};
  return "{\"time\":"+jsonQuote(time)+",\"event\":\"streamObservation\",\"aggregation\":\"latest-snapshot\",\"source\":"+jsonQuote(source)+",\"sessionId\":"+jsonQuote(session)+
    (tracks?",\"topologyEpoch\":":",\"epoch\":")+std::to_string(*epoch)+",\"windowMs\":"+std::to_string(*window)+",\"samples\":"+std::to_string(*samples)+
    ",\"droppedRecords\":"+std::to_string(*dropped)+",\"final\":"+(*final?"true":"false")+",\"values\":"+metrics+"}}";
}
inline std::string safeHostLogRecord(const Json& value) {
  const auto event=value.get("event").string(),time=value.get("time").string();
  if(event=="streamObservation")return safeHostObservationLogRecord(value);
  const auto* fields=std::get_if<Json::Object>(&value.get("values").value);
  if(!hostLogEvent(event)||!safeHostLogTime(time)||!fields)return {};
  std::string data="{";
  for(const auto& [name,field]:*fields) {
    if(!hostLogField(name))continue;
    std::string encoded;
    if(const auto* bit=std::get_if<bool>(&field.value))encoded=*bit?"true":"false";
    else if(const auto* number=std::get_if<double>(&field.value);number&&std::isfinite(*number)&&std::floor(*number)==*number&&std::abs(*number)<=4294967295.0)encoded=std::to_string(static_cast<int64_t>(*number));
    else continue;
    if(data.size()>1)data+=',';data+=jsonQuote(name)+':'+encoded;
  }
  return "{\"time\":"+jsonQuote(time)+",\"event\":"+jsonQuote(event)+",\"values\":"+data+"}}";
}
inline std::mutex& hostLogProcessMutex(){static std::mutex mutex;return mutex;}
// Caller holds the process lock. Both legacy events and async observations use
// this same cross-process gate, rotation and trusted-file checks.
inline bool appendHostLogRecordLocked(const std::string& line) {
  if(!operationalLogEnabled||line.empty()||line.size()>2048)return false;
  const auto directory=machineDirectory();if(!trustedMachineFile(directory))return false;
  WinHandle gate(CreateMutexW(nullptr,FALSE,L"Global\\MultitapHost.OperationalLog"));if(!gate.value)return false;
  const DWORD wait=WaitForSingleObject(gate.value,100);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)return false;
  struct Unlock{HANDLE handle;~Unlock(){ReleaseMutex(handle);}} unlock{gate.value};
  const auto path=directory/L"events.jsonl",prior=directory/L"events.previous.jsonl";
  const auto attributes=GetFileAttributesW(path.c_str());
  if(attributes!=INVALID_FILE_ATTRIBUTES&&!trustedMachineFile(path))return false;
  if(std::filesystem::exists(path)&&std::filesystem::file_size(path)>64000){
    if(std::filesystem::exists(prior)&&!trustedMachineFile(prior))return false;
    if(!MoveFileExW(path.c_str(),prior.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
  }
  WinHandle output(CreateFileW(path.c_str(),FILE_APPEND_DATA|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(output.value==INVALID_HANDLE_VALUE||!output.value||!trustedMachineFile(path))return false;
  BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(output.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nNumberOfLinks!=1)return false;
  const auto record=line+'\n';DWORD wrote=0;
  if(!WriteFile(output.value,record.data(),DWORD(record.size()),&wrote,nullptr)||wrote!=record.size())return false;
  FlushFileBuffers(output.value);return true;
}
// Fixed public diagnostics only; never accepts exception text or credentials.
// Journal errors cannot fail service control or a streaming transaction.
inline std::string hostLogEventFields(std::initializer_list<std::pair<const char*,int64_t>> values){
  std::string fields="{";for(const auto& [key,value]:values){if(!hostLogField(key)||value<0||value>4294967295LL)continue;if(fields.size()>1)fields+=',';fields+=jsonQuote(key)+':'+(hostLogBoolean(key)?std::string(value?"true":"false"):std::to_string(value));}return fields+'}';
}
inline void recordHostEvent(const char* event,std::initializer_list<std::pair<const char*,int64_t>> values,bool deduplicate=false) noexcept {
  try {
    if(!operationalLogEnabled)return;
    if(!hostLogEvent(event))return;
    const auto fields=hostLogEventFields(values);
    std::lock_guard processLock(hostLogProcessMutex());
    static std::map<std::string,std::string> last;
    if(deduplicate&&last[event]==fields)return;
    const auto record="{\"time\":"+jsonQuote(hostLogTime())+",\"event\":"+jsonQuote(event)+",\"values\":"+fields+"}";
    if(appendHostLogRecordLocked(record))last[event]=fields;
  }catch(...){ }
}
inline std::string hostLogSnapshot() {
  requireAdmin();std::deque<std::string> records;
  const auto directory=machineDirectory();if(!trustedMachineFile(directory))return "[]";
  for(const auto* name:{L"events.previous.jsonl",L"events.jsonl"}) {
    const auto path=directory/name;if(!std::filesystem::exists(path))continue;
    if(!trustedMachineFile(path)||std::filesystem::file_size(path)>65536)continue;
    std::ifstream file(path,std::ios::binary);std::string line;
    while(std::getline(file,line)) {
      if(line.size()>2048)continue;
      try{const auto record=safeHostLogRecord(JsonParser::parse(line));if(!record.empty()){records.push_back(record);if(records.size()>192)records.pop_front();}}catch(...){ }
    }
  }
  std::string result="[";for(const auto& row:records){if(result.size()+row.size()>60000)break;if(result.size()>1)result+=',';result+=row;}return result+']';
}
}
