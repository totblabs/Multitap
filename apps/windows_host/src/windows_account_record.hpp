#pragma once
// The Windows account of the desktop that Host settings stores once: the stream
// resumes only this account's own Windows session (stream_console_recovery.hpp).
// The record's file name (rdp-credentials.dpapi), its DPAPI description and the
// rdp_* error codes are the stored and wire names since the first release and
// stay as they are; nothing here is Remote Desktop.
#include "pending_connection.hpp"

namespace host {
struct WindowsAccountError:std::runtime_error {explicit WindowsAccountError(const char* code):std::runtime_error(code){} };
struct WindowsAccount {
  std::wstring enteredName,username,domain,password;
  ~WindowsAccount(){SecureZeroMemory(password.data(),password.size()*sizeof(wchar_t));}
};
inline bool windowsAccountText(const std::wstring& value,size_t minimum,size_t maximum) {
  return value.size()>=minimum&&value.size()<=maximum&&std::none_of(value.begin(),value.end(),[](wchar_t ch){return ch<32||ch==127;});
}
inline WindowsAccount parseWindowsAccount(const std::string& username,const std::string& password) {
  WindowsAccount value;
  try{value.enteredName=wide(username);value.password=wide(password);}catch(...){throw WindowsAccountError("rdp_credentials_invalid_input");}
  if(!windowsAccountText(value.enteredName,1,256)||!windowsAccountText(value.password,1,256))throw WindowsAccountError("rdp_credentials_invalid_input");
  const auto slash=value.enteredName.find(L'\\');
  if(slash!=std::wstring::npos){value.domain=value.enteredName.substr(0,slash);value.username=value.enteredName.substr(slash+1);if(value.username.find(L'\\')!=std::wstring::npos||!windowsAccountText(value.domain,1,255)||!windowsAccountText(value.username,1,256))throw WindowsAccountError("rdp_credentials_invalid_input");}
  else {value.username=value.enteredName;if(value.enteredName.find(L'@')==std::wstring::npos)value.domain=L".";}
  return value;
}
using WindowsAccountValidator=std::function<DWORD(const WindowsAccount&)>;
inline DWORD validateWindowsAccount(const WindowsAccount& value) {
  HANDLE token=nullptr;
  const BOOL valid=LogonUserW(value.username.c_str(),value.domain.empty()?nullptr:value.domain.c_str(),value.password.c_str(),LOGON32_LOGON_NETWORK,LOGON32_PROVIDER_DEFAULT,&token);
  const DWORD result=valid?ERROR_SUCCESS:GetLastError();if(token)CloseHandle(token);return result;
}
inline void checkWindowsAccountResult(DWORD result) {
  if(result==ERROR_SUCCESS)return;
  if(result==ERROR_LOGON_FAILURE||result==ERROR_NO_SUCH_USER||result==ERROR_WRONG_PASSWORD)throw WindowsAccountError("rdp_credentials_invalid");
  if(result==ERROR_ACCOUNT_RESTRICTION||result==ERROR_ACCOUNT_DISABLED||result==ERROR_ACCOUNT_EXPIRED||result==ERROR_INVALID_LOGON_HOURS||result==ERROR_INVALID_WORKSTATION||result==ERROR_PASSWORD_EXPIRED||result==ERROR_PASSWORD_MUST_CHANGE||result==ERROR_ACCOUNT_LOCKED_OUT)throw WindowsAccountError("rdp_account_restricted");
  throw WindowsAccountError("rdp_credentials_validation_failed");
}
inline std::filesystem::path windowsAccountRecordPath(){return machineDirectory()/L"private"/L"rdp-credentials.dpapi";}
inline void persistWindowsAccount(const WindowsAccount& value) {
  try {
    prepareMachineDirectory();std::string password=utf8(value.password);ProtectedTextScrubber passwordScrubber{password};std::string bytes="{\"format\":1,\"username\":"+jsonQuote(utf8(value.enteredName))+",\"password\":"+jsonQuote(password)+'}';ProtectedTextScrubber bytesScrubber{bytes};DATA_BLOB source{DWORD(bytes.size()),reinterpret_cast<BYTE*>(bytes.data())},sealed{};
    winCheck(CryptProtectData(&source,L"Multitap Windows RDP account",nullptr,nullptr,nullptr,CRYPTPROTECT_LOCAL_MACHINE|CRYPTPROTECT_UI_FORBIDDEN,&sealed),"Protect Windows RDP account");
    const bool saved=writePrivateFile(windowsAccountRecordPath(),sealed.pbData,sealed.cbData);LocalFree(sealed.pbData);
    if(!saved)throw WindowsAccountError("rdp_credentials_storage_failed");
  }catch(...){throw WindowsAccountError("rdp_credentials_storage_failed");}
}
inline void saveWindowsAccount(const std::string& username,const std::string& password,const std::string& confirmation,const WindowsAccountValidator& validator=validateWindowsAccount,const std::function<void(const WindowsAccount&)>& save=persistWindowsAccount) {
  if(password!=confirmation)throw WindowsAccountError("rdp_credentials_confirmation");auto value=parseWindowsAccount(username,password);checkWindowsAccountResult(validator(value));save(value);
}
inline constexpr const char* windowsLoginClearProtocol="windows-login-clear-v1";
namespace windows_login_record {
using Trust=std::function<bool(const std::filesystem::path&)>;
using HandleTrust=std::function<bool(HANDLE,const std::filesystem::path&)>;
using Decode=std::function<std::string(const std::string&)>;
struct Access {std::filesystem::path path;Trust trusted;HandleTrust handleTrusted;Decode decode;};
inline bool sameFile(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
  return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&
    a.ftCreationTime.dwHighDateTime==b.ftCreationTime.dwHighDateTime&&a.ftCreationTime.dwLowDateTime==b.ftCreationTime.dwLowDateTime;
}
inline bool heldPath(HANDLE file,const std::filesystem::path& expected){
  std::wstring final(32768,L'\0');const DWORD length=GetFinalPathNameByHandleW(file,final.data(),DWORD(final.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  if(!length||length>=final.size())return false;final.resize(length);
  return _wcsicmp(final.c_str(),(L"\\\\?\\"+std::filesystem::absolute(expected).lexically_normal().wstring()).c_str())==0;
}
inline bool nativeHandleTrusted(HANDLE file,const std::filesystem::path& expected){
  if(!heldPath(file,expected))return false;
  PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
  if(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
  const bool valid=owner&&(IsWellKnownSid(owner,WinLocalSystemSid)||IsWellKnownSid(owner,WinBuiltinAdministratorsSid))&&privateConnectionAcl(dacl);
  LocalFree(descriptor);return valid;
}
inline std::string unprotect(const std::string& bytes){
  DATA_BLOB source{DWORD(bytes.size()),reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()))},plain{};
  if(!CryptUnprotectData(&source,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain))throw WindowsAccountError("rdp_credentials_storage_failed");
  struct Cleanup{DATA_BLOB& plain;~Cleanup(){SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);}}cleanup{plain};
  return {reinterpret_cast<char*>(plain.pbData),plain.cbData};
}
inline Access nativeAccess(){return {windowsAccountRecordPath(),trustedPrivateConnectionFile,nativeHandleTrusted,unprotect};}
struct Snapshot {WinHandle file;BY_HANDLE_FILE_INFORMATION identity{};std::string revision;WindowsAccount account;};
inline bool revisionText(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline std::optional<Snapshot> snapshot(const Access& access,bool deleting=false){
  if(!access.trusted||!access.handleTrusted||!access.decode||!access.trusted(access.path.parent_path()))throw WindowsAccountError("rdp_credentials_storage_failed");
  WinHandle file(CreateFileW(access.path.c_str(),GENERIC_READ|(deleting?DELETE:0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(file.value==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return {};throw WindowsAccountError("rdp_credentials_storage_failed");}
  Snapshot result;LARGE_INTEGER size{};
  if(!access.trusted(access.path)||!access.handleTrusted(file.value,access.path)||GetFileType(file.value)!=FILE_TYPE_DISK||
      !GetFileInformationByHandle(file.value,&result.identity)||result.identity.nNumberOfLinks!=1||
      (result.identity.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
      !GetFileSizeEx(file.value,&size)||size.QuadPart<=0||size.QuadPart>16384)throw WindowsAccountError("rdp_credentials_storage_failed");
  std::string bytes(size_t(size.QuadPart),'\0');DWORD read=0;
  if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())throw WindowsAccountError("rdp_credentials_storage_failed");
  char trailing=0;if(!ReadFile(file.value,&trailing,1,&read,nullptr)||read)throw WindowsAccountError("rdp_credentials_storage_failed");
  const auto& i=result.identity;
  std::string bound=std::string(windowsLoginClearProtocol)+":"+std::to_string(i.dwVolumeSerialNumber)+":"+std::to_string(i.nFileIndexHigh)+":"+
    std::to_string(i.nFileIndexLow)+":"+std::to_string(i.ftCreationTime.dwHighDateTime)+":"+std::to_string(i.ftCreationTime.dwLowDateTime)+":"+
    std::to_string(i.ftLastWriteTime.dwHighDateTime)+":"+std::to_string(i.ftLastWriteTime.dwLowDateTime)+":"+bytes;
  ProtectedTextScrubber boundScrubber{bound};const auto digest=connectionSha256(bound);constexpr char hex[]="0123456789abcdef";
  for(const auto byte:digest){result.revision+=hex[byte>>4];result.revision+=hex[byte&15];}
  std::string text=access.decode(bytes);ProtectedTextScrubber scrubber{text};
  auto value=JsonParser::parse(text);ConnectionJsonScrubber valueScrubber{value};
  const auto* password=std::get_if<std::string>(&value.get("password").value);
  if(value.get("format").number()!=1||!password)throw WindowsAccountError("rdp_credentials_invalid_input");
  result.account=parseWindowsAccount(value.get("username").string(),*password);result.file=std::move(file);return result;
}
enum class Clear {removed,missing,changed,unverified};
inline Clear clear(const std::string& expected,const Access& access,const std::function<void()>& afterCompare={}){
  try{
    if(!revisionText(expected))return Clear::changed;
    auto held=snapshot(access,true);if(!held)return Clear::missing;
    if(held->revision!=expected)return Clear::changed;
    if(afterCompare)afterCompare();
    FILE_DISPOSITION_INFO disposition{TRUE};
    if(!SetFileInformationByHandle(held->file.value,FileDispositionInfo,&disposition,sizeof(disposition)))return Clear::unverified;
    const auto original=held->identity;held->file=WinHandle{};
    // Readback only. A newer record after the exact unlink is never deleted.
    WinHandle current(CreateFileW(access.path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
      nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(current.value==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND?Clear::removed:Clear::unverified;
    BY_HANDLE_FILE_INFORMATION now{};
    return GetFileInformationByHandle(current.value,&now)&&!sameFile(original,now)?Clear::removed:Clear::unverified;
  }catch(...){return Clear::unverified;}
}
inline std::string rpc(const Json& request,const Access& access,const std::function<void()>& refresh={}){
  const auto* object=std::get_if<Json::Object>(&request.value);
  Clear result=Clear::changed;
  if(object&&object->size()==3&&request.get("action").string()=="clearWindowsLoginV1"&&
      request.get("protocol").string()==windowsLoginClearProtocol&&std::holds_alternative<std::string>(request.get("revision").value))
    result=clear(request.get("revision").string(),access);
  const char* outcome=result==Clear::removed?"removed":result==Clear::missing?"missing":result==Clear::changed?"changed":"unverified";
  const bool ok=result==Clear::removed;
  if(ok&&refresh)try{refresh();}catch(...){ }
  return std::string("{\"ok\":")+(ok?"true":"false")+",\"clear\":{\"protocol\":"+jsonQuote(windowsLoginClearProtocol)+",\"outcome\":"+jsonQuote(outcome)+'}'+
    (ok?"":std::string(",\"error\":")+jsonQuote(result==Clear::unverified?"windows_login_clear_unverified":"windows_login_record_changed"))+'}';
}
}
inline std::optional<WindowsAccount> loadWindowsAccount(){
  auto held=windows_login_record::snapshot(windows_login_record::nativeAccess());
  return held?std::optional<WindowsAccount>(std::move(held->account)):std::nullopt;
}
struct WindowsAccountStatus {bool configured=false;std::string username,error,clearProtocol,clearRevision;};
inline WindowsAccountStatus windowsAccountStatus(const windows_login_record::Access& access=windows_login_record::nativeAccess()) {
  try{auto held=windows_login_record::snapshot(access);return held?WindowsAccountStatus{true,utf8(held->account.enteredName),"",windowsLoginClearProtocol,held->revision}:WindowsAccountStatus{};}
  catch(const WindowsAccountError& failure){return {false,"",std::string(failure.what())=="rdp_credentials_invalid_input"?"invalid_stored_credentials":"storage_unavailable","",""};}
  catch(...){return {false,"","storage_unavailable","",""};}
}
}
