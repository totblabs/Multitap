#pragma once
#include "machine.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>

namespace host {
inline uint64_t connectionNow() { FILETIME time{}; GetSystemTimeAsFileTime(&time); ULARGE_INTEGER value{}; value.LowPart=time.dwLowDateTime; value.HighPart=time.dwHighDateTime; return (value.QuadPart-116444736000000000ULL)/10000; }
inline bool connectionUuid(const std::string& text) {
  if(text.size()!=36)return false;
  for(size_t i=0;i<text.size();i++)if(i==8||i==13||i==18||i==23){if(text[i]!='-')return false;}else if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')||(text[i]>='A'&&text[i]<='F')))return false;
  return true;
}
// Match server/src/security.mjs desktopId; synchronized VM IDs are pve-149,
// not UUIDs. Host/request/machine identities retain their UUID checks.
inline bool connectionDesktopId(const std::string& text) {
  const auto alnum=[](char ch){return (ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9');};
  return !text.empty()&&text.size()<=80&&alnum(text.front())&&std::all_of(text.begin(),text.end(),[&](char ch){return alnum(ch)||ch=='_'||ch=='-';});
}
inline uint64_t connectionExpiry(const std::string& text) {
  if((text.size()!=20&&text.size()!=24)||text[4]!='-'||text[7]!='-'||text[10]!='T'||text[13]!=':'||text[16]!=':'||text.back()!='Z'||(text.size()==24&&text[19]!='.'))throw std::runtime_error("Invalid connection expiry");
  const auto number=[&](size_t at,size_t count){WORD value=0;for(size_t i=at;i<at+count;i++){if(text[i]<'0'||text[i]>'9')throw std::runtime_error("Invalid connection expiry");value=WORD(value*10+text[i]-'0');}return value;};
  SYSTEMTIME time{};time.wYear=number(0,4);time.wMonth=number(5,2);time.wDay=number(8,2);time.wHour=number(11,2);time.wMinute=number(14,2);time.wSecond=number(17,2);if(text.size()==24)time.wMilliseconds=number(20,3);
  FILETIME file{};SYSTEMTIME checked{};if(time.wYear<1970||!SystemTimeToFileTime(&time,&file)||!FileTimeToSystemTime(&file,&checked)||checked.wYear!=time.wYear||checked.wMonth!=time.wMonth||checked.wDay!=time.wDay||checked.wHour!=time.wHour||checked.wMinute!=time.wMinute||checked.wSecond!=time.wSecond)throw std::runtime_error("Invalid connection expiry");
  ULARGE_INTEGER value{};value.LowPart=file.dwLowDateTime;value.HighPart=file.dwHighDateTime;return (value.QuadPart-116444736000000000ULL)/10000;
}
inline std::string connectionRequestKey() {
  std::array<BYTE,32> bytes{};check(BCryptGenRandom(nullptr,bytes.data(),DWORD(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG),"Create connection request key");
  constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";std::string key;unsigned bits=0;int count=0;
  for(BYTE byte:bytes){bits=(bits<<8)|byte;count+=8;while(count>=6){count-=6;key+=alphabet[(bits>>count)&63];}}if(count)key+=alphabet[(bits<<(6-count))&63];SecureZeroMemory(bytes.data(),bytes.size());return key;
}
// host-request-code-v1 (server/src/verification-codes.mjs hostRequestCode):
// the code the administrator compares with the console before approving.
// Crockford base32 of the first 40 bits of
//   SHA-256("host-request-code-v1:" + lowercase hex SHA-256(requestKey)),
// grouped XXXX-XXXX. The server derives it from the stored key hash; the key
// itself never leaves the protected request file.
inline std::array<BYTE,32> connectionSha256(const std::string& text) {
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::array<BYTE,32> digest{};
  check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0),"Request code SHA256 provider");
  NTSTATUS status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
  if(status>=0)status=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),ULONG(text.size()),0);
  if(status>=0)status=BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0);
  if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);check(status,"Hash request code");return digest;
}
inline std::string connectionVerificationCode(const std::string& key) {
  const auto keyHash=connectionSha256(key);std::string hex;hex.reserve(64);constexpr char digits[]="0123456789abcdef";
  for(BYTE byte:keyHash){hex+=digits[byte>>4];hex+=digits[byte&15];}
  const auto digest=connectionSha256("host-request-code-v1:"+hex);SecureZeroMemory(hex.data(),hex.size());
  uint64_t bits=0;for(int i=0;i<5;i++)bits=(bits<<8)|digest[size_t(i)];
  constexpr char alphabet[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ";std::string code;
  for(int i=0;i<8;i++){if(i==4)code+='-';code+=alphabet[(bits>>(35-5*i))&31];}
  return code;
}
// Tells the broker this Host shows the code (released brokers ignore it).
constexpr const wchar_t* connectionRequestHeaders=L"Content-Type: application/json\r\nX-Multitap-Host-Request-Code: v1\r\n";
inline void wipeConnectionJson(Json& value) {if(auto* text=std::get_if<std::string>(&value.value))SecureZeroMemory(text->data(),text->size());else if(auto* object=std::get_if<Json::Object>(&value.value))for(auto& item:*object)wipeConnectionJson(item.second);else if(auto* array=std::get_if<Json::Array>(&value.value))for(auto& item:*array)wipeConnectionJson(item);}
struct ConnectionJsonScrubber {Json& value;~ConnectionJsonScrubber(){wipeConnectionJson(value);}};
struct ConnectionRequest {
  std::wstring origin, httpConsentOrigin;
  std::string key,id,hostname,uuid,version,status="pending",expiresAt,error,baseHostId;
  uint64_t createdAt=0,expires=0,nextAttempt=0;unsigned failures=0;
  bool committed=false,cancelAcknowledged=false;
  Credentials approved;
  ~ConnectionRequest(){SecureZeroMemory(key.data(),key.size());SecureZeroMemory(approved.token.data(),approved.token.size());}
  bool same(const ConnectionRequest& other)const{return origin==other.origin&&httpConsentOrigin==other.httpConsentOrigin&&key==other.key&&!key.empty();}
  bool active()const{return status=="pending"||(status=="approved"&&!committed)||(status=="cancelled"&&!cancelAcknowledged);}
};
struct ConnectionGuard {virtual ~ConnectionGuard()=default;};
using ConnectionGuardPtr=std::unique_ptr<ConnectionGuard>;
struct ConnectionAccess {
  std::function<ConnectionGuardPtr(DWORD)> stateLock,networkLock;
  std::function<std::optional<ConnectionRequest>()> load;
  std::function<void(const ConnectionRequest&)> save;
  std::function<Json(const Origin&,const std::wstring&,const std::string&)> send;
  std::function<void(const Credentials&,const std::string&)> commit;
  std::function<std::string()> currentHostId;
  std::function<std::string()> machineIdentity;
  std::function<uint64_t()> now=connectionNow;
};
inline ConnectionGuardPtr connectionStateLock(ConnectionAccess& access) {auto lock=access.stateLock(5000);if(!lock)throw std::runtime_error("Connection settings are busy");return lock;}
inline void beginConnectionRequest(ConnectionAccess& access,const std::wstring& address,const std::string& hostname,const std::string& uuid,const std::string& version,const std::string& key,const std::wstring& consent={}) {
  const Origin origin(address,consent);if(!validToken(key)||hostname.empty()||hostname.size()>255||!connectionUuid(uuid)||version.empty()||version.size()>40)throw std::runtime_error("Invalid connection request identity");
  auto lock=connectionStateLock(access);auto existing=access.load();const auto now=access.now();
  if(existing&&existing->active()&&existing->expires>now){
    if(existing->status!="cancelled"&&existing->origin==origin.text&&existing->uuid==uuid)return;
    throw std::runtime_error(ui_text::utf8(ui_text::Id::PendingCancelWait));
  }
  ConnectionRequest request;request.origin=origin.text;request.httpConsentOrigin=origin.httpConsentOrigin;request.key=key;request.hostname=hostname;request.uuid=uuid;request.version=version;request.createdAt=now;request.expires=now+10*60*1000;request.baseHostId=access.currentHostId();access.save(request);
}
inline void cancelConnectionRequest(ConnectionAccess& access) {
  auto lock=connectionStateLock(access);auto current=access.load();if(!current)return;
  if(current->committed)throw std::runtime_error(ui_text::utf8(ui_text::Id::PendingCommitted));
  if(current->status=="rejected"||current->status=="expired")current->cancelAcknowledged=true;
  current->status="cancelled";current->nextAttempt=0;current->error.clear();access.save(*current);
}
// A public request identity, never an authorization token. The protected key
// distinguishes a successor even before the broker has returned a request ID.
// ID/expiry/nextAttempt may change while the SAME request is being recovered.
inline std::string connectionCancellationRevision(const ConnectionRequest& request) {
  if(!validToken(request.key)||!request.createdAt||!connectionUuid(request.uuid))throw std::runtime_error("connection_request_changed");
  std::string bound="connection-cancel-v1\n"+utf8(request.origin)+"\n"+request.uuid+"\n"+std::to_string(request.createdAt)+"\n"+request.key;
  ProtectedTextScrubber scrubber{bound};const auto digest=connectionSha256(bound);
  constexpr char hex[]="0123456789abcdef";std::string revision;revision.reserve(64);
  for(const auto byte:digest){revision+=hex[byte>>4];revision+=hex[byte&15];}return revision;
}
inline void cancelConnectionRequestV1(ConnectionAccess& access,const std::string& expectedRevision) {
  if(expectedRevision.size()!=64||!std::all_of(expectedRevision.begin(),expectedRevision.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))
    throw std::runtime_error("connection_request_changed");
  auto lock=connectionStateLock(access);auto current=access.load();
  // Compare under the SAME lock as the save. An earlier UI confirmation can
  // never cancel the current successor just because its visible ID is absent.
  if(!current||current->committed||connectionCancellationRevision(*current)!=expectedRevision)throw std::runtime_error("connection_request_changed");
  if(current->status=="rejected"||current->status=="expired")current->cancelAcknowledged=true;
  current->status="cancelled";current->nextAttempt=0;current->error.clear();access.save(*current);
}
inline void connectionRetry(ConnectionRequest& request,uint64_t now,const char* error) {request.error=error;const unsigned shift=std::min(request.failures++,3u);request.nextAttempt=now+std::min<uint64_t>(3000ULL<<shift,30000);}
inline void connectionResponse(ConnectionRequest& current,const Json& response,bool creating,uint64_t now) {
  const auto status=response.get("status").string(),expiry=response.get("expiresAt").string();
  if(status!="pending"&&status!="approved"&&status!="rejected"&&status!="expired")throw std::runtime_error("Invalid request state");
  const auto until=connectionExpiry(expiry);const double retry=response.get("retryAfterSeconds").number(-1);
  if(retry<3||retry>60||std::floor(retry)!=retry||until>now+24*60*60*1000ULL)throw std::runtime_error("Invalid request timing");
  if(creating){const auto id=response.get("requestId").string();if(!connectionUuid(id))throw std::runtime_error("Invalid request identity");current.id=id;}
  else {const auto id=response.get("requestId").string();if(!id.empty()&&id!=current.id)throw std::runtime_error("Unexpected request identity");}
  current.expiresAt=expiry;current.expires=until;current.nextAttempt=now+uint64_t(retry)*1000;current.failures=0;current.error.clear();
  if(current.status=="cancelled"){
    if(status=="rejected"||status=="expired"){current.cancelAcknowledged=true;SecureZeroMemory(current.key.data(),current.key.size());current.key.clear();}
    else if(status=="approved"){current.status="pending";current.error="already_approved";}
    return;
  }
  current.status=status;
  const bool hasCredentials=!std::holds_alternative<std::nullptr_t>(response.get("credentials").value);
  if(hasCredentials&&(status!="approved"||creating))throw std::runtime_error("Credentials outside approval status");
  if(status=="approved"&&!creating){
    const auto& value=response.get("credentials");Credentials approved{current.origin,value.get("hostId").string(),value.get("hostToken").string(),value.get("desktopId").string(),current.httpConsentOrigin,current.uuid};
    if(!connectionUuid(approved.hostId)||!connectionDesktopId(approved.desktopId)||!validToken(approved.token))throw std::runtime_error("Invalid approved credentials");
    current.approved=std::move(approved);
  }
  if(status=="rejected"||status=="expired"){SecureZeroMemory(current.key.data(),current.key.size());current.key.clear();}
}
// One network owner across service/RPC processes; the short state lock is not
// held during HTTPS, so cancellation and replacement invalidate late replies.
inline bool tickConnectionRequest(ConnectionAccess& access) {
  auto network=access.networkLock(0);if(!network)return false;
  std::optional<ConnectionRequest> snapshot;
  {
    auto lock=connectionStateLock(access);snapshot=access.load();if(!snapshot||!snapshot->active())return false;
    const auto now=access.now();
    if(access.machineIdentity&&access.machineIdentity()!=snapshot->uuid){snapshot->status="rejected";snapshot->error="machine_identity_changed";SecureZeroMemory(snapshot->key.data(),snapshot->key.size());snapshot->key.clear();access.save(*snapshot);return false;}
    if(snapshot->nextAttempt>now)return false;
    if(snapshot->status=="approved"&&validToken(snapshot->approved.token)){
      try{access.commit(snapshot->approved,snapshot->baseHostId);}catch(...){connectionRetry(*snapshot,now,"registration_save_failed");access.save(*snapshot);return false;}
      auto completed=*snapshot;completed.committed=true;completed.error.clear();SecureZeroMemory(completed.key.data(),completed.key.size());completed.key.clear();SecureZeroMemory(completed.approved.token.data(),completed.approved.token.size());completed.approved.token.clear();try{access.save(completed);}catch(...){}return true;
    }
    if(snapshot->expires<=now){snapshot->status=snapshot->status=="cancelled"?"cancelled":"expired";snapshot->cancelAcknowledged=true;SecureZeroMemory(snapshot->key.data(),snapshot->key.size());snapshot->key.clear();access.save(*snapshot);return false;}
    snapshot->nextAttempt=now+3000;access.save(*snapshot);
  }
  const bool creating=snapshot->id.empty(),cancelling=snapshot->status=="cancelled"&&!creating;
  const auto path=creating?L"/api/v1/hosts/requests":cancelling?L"/api/v1/hosts/requests/cancel":L"/api/v1/hosts/requests/status";
  std::string body=creating?"{\"requestKey\":"+jsonQuote(snapshot->key)+",\"hostname\":"+jsonQuote(snapshot->hostname)+",\"machineUuid\":"+jsonQuote(snapshot->uuid)+",\"version\":"+jsonQuote(snapshot->version)+'}':"{\"requestId\":"+jsonQuote(snapshot->id)+",\"requestKey\":"+jsonQuote(snapshot->key)+'}';ProtectedTextScrubber bodyScrubber{body};
  Json response;ConnectionJsonScrubber responseScrubber{response};std::string error;bool alreadyApproved=false,notFound=false;
  try{response=access.send(Origin(snapshot->origin,snapshot->httpConsentOrigin),path,body);}catch(const BrokerHttpError& failure){alreadyApproved=cancelling&&failure.status==409&&failure.code=="host_request_already_approved";notFound=failure.status==404;error="network_unavailable";}catch(...){error="network_unavailable";}
  auto lock=connectionStateLock(access);auto current=access.load();if(!current||!current->same(*snapshot)||current->status!=snapshot->status)return false;
  const auto now=access.now();
  if(alreadyApproved){current->status="pending";current->error="already_approved";current->nextAttempt=now+3000;access.save(*current);return false;}
  if(notFound){current->status=current->status=="cancelled"?"cancelled":"expired";current->cancelAcknowledged=true;current->error="request_expired";SecureZeroMemory(current->key.data(),current->key.size());current->key.clear();access.save(*current);return false;}
  if(!error.empty()){connectionRetry(*current,now,error.c_str());access.save(*current);return false;}
  try{connectionResponse(*current,response,creating,now);}catch(...){connectionRetry(*current,now,"invalid_response");access.save(*current);return false;}
  // Seal approved credentials before applying them. A restart/storage failure
  // retries the same approval without losing the proof key or old registration.
  access.save(*current);
  if(current->status=="approved"&&validToken(current->approved.token)){
    try{access.commit(current->approved,current->baseHostId);}catch(...){connectionRetry(*current,now,"registration_save_failed");access.save(*current);return false;}
    auto completed=*current;completed.committed=true;completed.error.clear();SecureZeroMemory(completed.key.data(),completed.key.size());completed.key.clear();SecureZeroMemory(completed.approved.token.data(),completed.approved.token.size());completed.approved.token.clear();try{access.save(completed);}catch(...){}return true;
  }
  return false;
}
inline std::string publicConnectionRequest(const std::optional<ConnectionRequest>& value) {
  if(!value)return "{\"status\":\"none\"}";
  const auto& request=*value;const auto status=request.status=="approved"&&!request.committed?"pending":request.status;
  const auto error=request.status=="cancelled"&&!request.cancelAcknowledged?"cancel_pending":request.error;
  // Only while the administrator still has to approve this request.
  const bool awaiting=(request.status=="pending"||(request.status=="approved"&&!request.committed))&&validToken(request.key);
  return "{\"status\":"+jsonQuote(status)+",\"origin\":"+jsonQuote(utf8(request.origin))+",\"expiresAt\":"+jsonQuote(request.expiresAt)+",\"requestId\":"+jsonQuote(request.id)+",\"error\":"+jsonQuote(error)+",\"cancelAcknowledged\":"+(request.cancelAcknowledged?"true":"false")+(awaiting?",\"verificationCode\":"+jsonQuote(connectionVerificationCode(request.key))+",\"cancelProtocol\":\"cancel-request-v1\",\"cancelRevision\":"+jsonQuote(connectionCancellationRevision(request)):"")+'}';
}
// Production persistence is machine DPAPI in a protected private directory.
// This file is never placed in stdout, command arguments or public settings.
inline std::filesystem::path connectionRequestPath(){return machineDirectory()/L"private"/L"connection-request.dpapi";}
inline bool privateConnectionAcl(PACL dacl) {return privateOnlyAcl(dacl);}
inline bool trustedPrivateConnectionFile(const std::filesystem::path& path) {return trustedPrivateFile(path);}
inline std::string encodeConnectionRequest(const ConnectionRequest& r) {
  return "{\"origin\":"+jsonQuote(utf8(r.origin))+(r.httpConsentOrigin.empty()?"":",\"httpConsentOrigin\":"+jsonQuote(utf8(r.httpConsentOrigin)))+",\"requestKey\":"+jsonQuote(r.key)+",\"requestId\":"+jsonQuote(r.id)+",\"hostname\":"+jsonQuote(r.hostname)+",\"machineUuid\":"+jsonQuote(r.uuid)+",\"version\":"+jsonQuote(r.version)+",\"status\":"+jsonQuote(r.status)+",\"expiresAt\":"+jsonQuote(r.expiresAt)+",\"error\":"+jsonQuote(r.error)+",\"baseHostId\":"+jsonQuote(r.baseHostId)+",\"createdAt\":"+std::to_string(r.createdAt)+",\"expires\":"+std::to_string(r.expires)+",\"nextAttempt\":"+std::to_string(r.nextAttempt)+",\"failures\":"+std::to_string(r.failures)+",\"committed\":"+(r.committed?"true":"false")+",\"cancelAcknowledged\":"+(r.cancelAcknowledged?"true":"false")+",\"approvedHostId\":"+jsonQuote(r.approved.hostId)+",\"approvedToken\":"+jsonQuote(r.approved.token)+",\"approvedDesktopId\":"+jsonQuote(r.approved.desktopId)+'}';
}
inline void saveConnectionRequest(const ConnectionRequest& request) {
  prepareMachineDirectory();std::string bytes=encodeConnectionRequest(request);ProtectedTextScrubber scrubber{bytes};DATA_BLOB plain{DWORD(bytes.size()),reinterpret_cast<BYTE*>(bytes.data())},sealed{};
  winCheck(CryptProtectData(&plain,L"Multitap connection request",nullptr,nullptr,nullptr,CRYPTPROTECT_LOCAL_MACHINE|CRYPTPROTECT_UI_FORBIDDEN,&sealed),"Protect connection request");
  const bool saved=writePrivateFile(connectionRequestPath(),sealed.pbData,sealed.cbData);LocalFree(sealed.pbData);
  if(!saved)throw std::runtime_error("Cannot save private connection request");
}
// host-machine-v1: a request file this Windows cannot unseal (copied disk after sysprep).
struct ConnectionRequestUnreadable:std::runtime_error{ConnectionRequestUnreadable():std::runtime_error("Read private connection request"){}};
inline std::optional<ConnectionRequest> loadConnectionRequest() {
  const auto path=connectionRequestPath();if(!std::filesystem::exists(path))return std::nullopt;if(!trustedPrivateConnectionFile(path))throw std::runtime_error("Unsafe connection request permissions");
  std::ifstream file(path,std::ios::binary);std::vector<BYTE> bytes(16385);file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));bytes.resize(size_t(file.gcount()));if(bytes.empty()||bytes.size()>16384)throw std::runtime_error("Invalid protected connection request");DATA_BLOB source{DWORD(bytes.size()),bytes.data()},plain{};
  if(!CryptUnprotectData(&source,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain))throw ConnectionRequestUnreadable();std::string text(reinterpret_cast<char*>(plain.pbData),plain.cbData);SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);ProtectedTextScrubber textScrubber{text};Json value=JsonParser::parse(text);ConnectionJsonScrubber valueScrubber{value};
  ConnectionRequest r;const Origin savedOrigin(wide(value.get("origin").string()),wide(value.get("httpConsentOrigin").string()));r.origin=savedOrigin.text;r.httpConsentOrigin=savedOrigin.httpConsentOrigin;r.key=value.get("requestKey").string();r.id=value.get("requestId").string();r.hostname=value.get("hostname").string();r.uuid=value.get("machineUuid").string();r.version=value.get("version").string();r.status=value.get("status").string();r.expiresAt=value.get("expiresAt").string();r.error=value.get("error").string();r.baseHostId=value.get("baseHostId").string();
  const auto integer=[&](const char* key){const double n=value.get(key).number(-1);if(n<0||n>9007199254740991.0||std::floor(n)!=n)throw std::runtime_error("Invalid private request timing");return uint64_t(n);};r.createdAt=integer("createdAt");r.expires=integer("expires");r.nextAttempt=integer("nextAttempt");r.failures=unsigned(std::min<uint64_t>(integer("failures"),16));r.committed=value.get("committed").boolean();r.cancelAcknowledged=value.get("cancelAcknowledged").boolean();r.approved={r.origin,value.get("approvedHostId").string(),value.get("approvedToken").string(),value.get("approvedDesktopId").string(),r.httpConsentOrigin,r.uuid};
  if((r.active()&&!validToken(r.key))||(!r.id.empty()&&!connectionUuid(r.id))||!connectionUuid(r.uuid)||r.hostname.empty()||r.hostname.size()>255||r.version.empty()||r.version.size()>40||(r.status!="pending"&&r.status!="approved"&&r.status!="rejected"&&r.status!="expired"&&r.status!="cancelled")||r.error.size()>48||r.error.find_first_not_of("abcdefghijklmnopqrstuvwxyz_")!=std::string::npos)throw std::runtime_error("Invalid private connection request");
  return r;
}
class NativeConnectionGuard final:public ConnectionGuard {
 public:
  NativeConnectionGuard(HANDLE handle,bool locked):handle_(handle),locked_(locked){}
  ~NativeConnectionGuard(){if(locked_)ReleaseMutex(handle_);if(handle_)CloseHandle(handle_);}
 private:HANDLE handle_;bool locked_;
};
inline ConnectionGuardPtr nativeConnectionLock(const wchar_t* name,DWORD wait) {
  PSECURITY_DESCRIPTOR descriptor=nullptr;winCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr),"Create connection lock ACL");SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};HANDLE mutex=CreateMutexW(&attributes,FALSE,name);LocalFree(descriptor);if(!mutex)winCheck(FALSE,"Create connection lock");
  PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR actual=nullptr;const DWORD queried=GetSecurityInfo(mutex,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&actual);const bool trusted=queried==ERROR_SUCCESS&&owner&&(IsWellKnownSid(owner,WinLocalSystemSid)||IsWellKnownSid(owner,WinBuiltinAdministratorsSid))&&privateConnectionAcl(dacl);if(actual)LocalFree(actual);if(!trusted){CloseHandle(mutex);throw std::runtime_error("Unsafe connection lock owner or ACL");}
  const DWORD result=WaitForSingleObject(mutex,wait);if(result==WAIT_OBJECT_0||result==WAIT_ABANDONED)return std::make_unique<NativeConnectionGuard>(mutex,true);CloseHandle(mutex);if(result==WAIT_TIMEOUT)return {};winCheck(FALSE,"Acquire connection lock");return {};
}
inline ConnectionAccess nativeConnectionAccess() {
  ConnectionAccess access;
  access.stateLock=[](DWORD wait){return nativeConnectionLock(L"Global\\MultitapHost.Connection.State",wait);};access.networkLock=[](DWORD wait){return nativeConnectionLock(L"Global\\MultitapHost.Connection.Network",wait);};access.load=loadConnectionRequest;access.save=saveConnectionRequest;
  // A registration this Windows cannot unseal (copied disk after sysprep) does
  // not block a new request; its approval keeps that file aside (commit below).
  access.currentHostId=[](){Credentials existing;try{if(!loadCredentials(existing))return std::string();}catch(const CredentialUnreadable&){return std::string();}const auto id=existing.hostId;SecureZeroMemory(existing.token.data(),existing.token.size());return id;};
  access.machineIdentity=machineUuid;
  access.send=[](const Origin& origin,const std::wstring& path,const std::string& body){return post(origin,path,body,connectionRequestHeaders);};
  access.commit=[](const Credentials& approved,const std::string& expected){
    Credentials old;bool hadOld=false;try{hadOld=loadCredentials(old);}catch(const CredentialUnreadable&){if(!expected.empty())throw;keepCredentialFileAside(L"unreadable");}ProtectedTextScrubber oldScrubber{old.token};
    if((hadOld?old.hostId:std::string())!=expected&&(!hadOld||old.hostId!=approved.hostId))throw std::runtime_error("Registration changed during approval");
    saveCredentials(approved);
    try{savePublicEnrollment(approved);}catch(...){if(hadOld)saveCredentials(old);else if(!DeleteFileW(credentialPath().c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Registration rollback failed");throw;}
  };
  return access;
}
inline std::string connectionRequestStatus() {
  try{if(!std::filesystem::exists(connectionRequestPath()))return publicConnectionRequest(std::nullopt);auto access=nativeConnectionAccess();auto lock=connectionStateLock(access);return publicConnectionRequest(access.load());}catch(...){return "{\"status\":\"unavailable\",\"error\":\"storage_unavailable\"}";}
}
inline bool requestHostConnection(const std::wstring& origin,const std::wstring& consent={}) {
  requireAdmin();testBroker(origin,consent);wchar_t hostname[MAX_COMPUTERNAME_LENGTH+1]{};DWORD size=MAX_COMPUTERNAME_LENGTH+1;winCheck(GetComputerNameW(hostname,&size),"Read request hostname");auto access=nativeConnectionAccess();
  // A request file this Windows cannot unseal would block every new request: keep it aside (never deleted).
  {auto lock=connectionStateLock(access);try{(void)access.load();}catch(const ConnectionRequestUnreadable&){keepPrivateFileAside(connectionRequestPath(),L"unreadable");}}
  std::string key=connectionRequestKey();ProtectedTextScrubber keyScrubber{key};beginConnectionRequest(access,origin,utf8(hostname),machineUuid(),MULTITAP_HOST_VERSION_UTF8,key,consent);return tickConnectionRequest(access);
}
inline void cancelHostConnection(){requireAdmin();auto access=nativeConnectionAccess();cancelConnectionRequest(access);tickConnectionRequest(access);}
inline void cancelHostConnectionV1(const std::string& revision){requireAdmin();auto access=nativeConnectionAccess();cancelConnectionRequestV1(access,revision);tickConnectionRequest(access);}
// One pass over the saved request: `changed` when a registration was committed
// (the service reloads), `active` while the request still has broker work to do.
struct PendingConnectionTick { bool changed = false, active = false; };
inline PendingConnectionTick pendingConnectionTick(std::string& notified) {
  auto access=nativeConnectionAccess();const bool applied=tickConnectionRequest(access);auto lock=connectionStateLock(access);auto current=access.load();
  PendingConnectionTick result;result.active=current&&current->active();
  if(applied||(current&&current->committed&&current->approved.hostId!=notified)){if(current)notified=current->approved.hostId;result.changed=true;}
  return result;
}
// Where the worker looks. Tests substitute a private folder and the pass.
struct PendingConnectionWatch {
  std::filesystem::path request=connectionRequestPath();
  std::function<PendingConnectionTick()> tick;  // empty: pendingConnectionTick
  DWORD activeMs=1000,unwatchedMs=1000;
};
// A registered Host normally has no request, and reading the request folder
// every second for the service's whole life would cost about 60 wakeups a
// minute, and two DPAPI decryptions a second while an old finished request
// file stays (215 wakeups and ~60 ms CPU a minute, measured). So the worker
// waits for a file name change in the request's folder,
// which Host settings' atomic save produces: a new request is seen at once.
// Only an active request keeps the one-second pass its retries and approval
// checks need. Without a folder to watch it keeps the one-second check.
class PendingConnectionWorker {
 public:
  explicit PendingConnectionWorker(std::function<void()> changed,PendingConnectionWatch watch={}):changed_(std::move(changed)),watch_(std::move(watch)),stop_(CreateEventW(nullptr,TRUE,FALSE,nullptr)){if(!stop_.value)winCheck(FALSE,"Pending connection stop event");thread_=std::thread([this]{run();});}
  ~PendingConnectionWorker(){SetEvent(stop_.value);if(thread_.joinable())thread_.join();}
  PendingConnectionWorker(const PendingConnectionWorker&)=delete;
 private:
  void run(){
    std::string notified;HANDLE folder=INVALID_HANDLE_VALUE;
    struct Close{HANDLE& handle;~Close(){if(handle!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(handle);}}closer{folder};
    for(;;){
      // Armed before the file is checked, so a save in between still wakes the wait.
      if(folder==INVALID_HANDLE_VALUE)folder=FindFirstChangeNotificationW(watch_.request.parent_path().c_str(),FALSE,FILE_NOTIFY_CHANGE_FILE_NAME);
      bool active=false;
      try{if(std::filesystem::exists(watch_.request)){const auto pass=watch_.tick?watch_.tick():pendingConnectionTick(notified);active=pass.active;if(pass.changed)changed_();}}catch(...){active=true;}
      const HANDLE events[]{stop_.value,folder};const bool watched=folder!=INVALID_HANDLE_VALUE;
      const DWORD waited=WaitForMultipleObjects(watched?2:1,events,FALSE,active?watch_.activeMs:watched?INFINITE:watch_.unwatchedMs);
      if(waited==WAIT_OBJECT_0)return;
      if(waited==WAIT_OBJECT_0+1&&!FindNextChangeNotification(folder)){FindCloseChangeNotification(folder);folder=INVALID_HANDLE_VALUE;}
      if(waited==WAIT_FAILED){if(watched){FindCloseChangeNotification(folder);folder=INVALID_HANDLE_VALUE;}if(WaitForSingleObject(stop_.value,watch_.unwatchedMs)==WAIT_OBJECT_0)return;}
    }
  }
  std::function<void()> changed_;PendingConnectionWatch watch_;WinHandle stop_;std::thread thread_;
};
}
