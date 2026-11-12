#pragma once
// native-display-trust-v1: which entries of the virtual display driver's
// certificate this product's own install added (the one accepted
// trust-store change).
//
// Removing the Host takes the certificate away again only where Multitap put it
// (host_removal.hpp). A certificate that was trusted before Multitap, e.g. by
// Sunshine itself, is never recorded and therefore never removed.
//
// The record: HKLM\SOFTWARE\Multitap\Host\NativeDisplayTrust, one REG_SZ value
// per store ("Root", "TrustedPublisher") holding the certificate's SHA-1
// thumbprint. Written right after the entry was added (host_prerequisites.hpp),
// deleted when that entry is removed. It is read only from a key that only
// SYSTEM and Administrators can change.
//
// An earlier Host recorded an added certificate only in the operational
// journal (hostAction 276 stage 1, `changed`), which rotates away. That record
// is copied here once per machine while the journal still has it
// (`JournalChecked`). The journal does not name the stores: an install that
// added the certificate is taken to have added both entries that are present
// now. `JournalChecked` stays after a removal so that an old journal line can
// never claim a certificate somebody else added later.
#include "support.hpp"
#include <wincrypt.h>
#include <functional>
#include <mutex>
#pragma comment(lib,"crypt32.lib")

namespace host {
inline constexpr const wchar_t* nativeDisplayTrustKey=L"SOFTWARE\\Multitap\\Host\\NativeDisplayTrust";
inline constexpr const wchar_t* nativeDisplayTrustJournalChecked=L"JournalChecked";
inline constexpr const char* nativeDisplayTrustStores[]={"Root","TrustedPublisher"};
inline const wchar_t* nativeDisplayTrustStoreName(const std::string& store){return store=="Root"?L"Root":store=="TrustedPublisher"?L"TrustedPublisher":nullptr;}
inline bool nativeDisplayThumbprint(const std::string& value){
  return value.size()==40&&std::all_of(value.begin(),value.end(),[](char ch){return (ch>='0'&&ch<='9')||(ch>='A'&&ch<='F');});
}

// ---- decisions (no Windows call) --------------------------------------------
// One journal line: a finished virtual display install whose run added the
// certificate (settings_rpc.hpp hostAction 276 stage 1, ok and changed).
inline bool journalLineAddedTrust(const std::string& line){
  try{
    if(line.empty()||line.size()>2048)return false;
    const auto row=JsonParser::parse(line);const auto& values=row.get("values");
    const auto* ok=std::get_if<bool>(&values.get("ok").value);const auto* changed=std::get_if<bool>(&values.get("changed").value);
    return row.get("event").string()=="hostAction"&&values.get("action").number(-1)==276&&values.get("stage").number(-1)==1&&ok&&*ok&&changed&&*changed;
  }catch(...){return false;}
}
struct NativeDisplayTrustBackfill {
  std::function<bool()> checked;                              // the journal was looked at before on this machine
  std::function<std::vector<std::string>()> recorded;        // stores already in the record
  std::function<std::optional<std::vector<std::string>>()> journal;  // every journal line, oldest first; nullopt: unreadable
  std::function<bool(const std::string&)> present;           // the pinned certificate is in this store now
  std::function<bool(const std::string&)> record;
  std::function<bool()> markChecked;
};
// Returns the stores it recorded. The journal is looked at once per machine; an
// unreadable journal is looked at again next time. A record that exists is
// never extended from the journal (the install that wrote it knew the stores).
inline std::vector<std::string> backfillNativeDisplayTrust(const NativeDisplayTrustBackfill& access){
  std::vector<std::string> added;
  if(!access.checked||!access.recorded||!access.journal||!access.present||!access.record||!access.markChecked)return added;
  if(access.checked())return added;
  if(!access.recorded().empty()){(void)access.markChecked();return added;}
  const auto lines=access.journal();
  if(!lines)return added;
  const bool claimed=std::any_of(lines->begin(),lines->end(),journalLineAddedTrust);
  if(claimed)for(const char* store:nativeDisplayTrustStores)if(access.present(store)&&access.record(store))added.emplace_back(store);
  (void)access.markChecked();
  return added;
}

// ---- Windows ----------------------------------------------------------------
// Only SYSTEM and Administrators can change this registry key.
inline bool machineRegistryKeyTrusted(HKEY key){
  PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
  if(GetSecurityInfo(key,SE_REGISTRY_KEY,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
  const auto administrator=[](PSID sid){return IsWellKnownSid(sid,WinLocalSystemSid)||IsWellKnownSid(sid,WinBuiltinAdministratorsSid);};
  bool safe=owner&&dacl&&administrator(owner);
  constexpr ACCESS_MASK writes=KEY_SET_VALUE|KEY_CREATE_SUB_KEY|KEY_CREATE_LINK|DELETE|WRITE_DAC|WRITE_OWNER|GENERIC_WRITE|GENERIC_ALL;
  for(DWORD index=0;safe&&index<dacl->AceCount;++index){
    void* raw=nullptr;if(!GetAce(dacl,index,&raw)){safe=false;break;}
    const auto* header=static_cast<ACE_HEADER*>(raw);
    if((header->AceFlags&INHERIT_ONLY_ACE)||header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
    if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){safe=false;break;}
    const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if((ace->Mask&writes)&&!administrator(const_cast<DWORD*>(&ace->SidStart)))safe=false;
  }
  LocalFree(descriptor);return safe;
}
struct NativeDisplayTrustRegistry {
  HKEY key=nullptr;
  explicit NativeDisplayTrustRegistry(REGSAM access,bool create=false){
    if(create){if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,nativeDisplayTrustKey,0,nullptr,0,access|KEY_WOW64_64KEY,nullptr,&key,nullptr)!=ERROR_SUCCESS)key=nullptr;}
    else if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,nativeDisplayTrustKey,0,access|KEY_WOW64_64KEY,&key)!=ERROR_SUCCESS)key=nullptr;
  }
  ~NativeDisplayTrustRegistry(){if(key)RegCloseKey(key);}
  NativeDisplayTrustRegistry(const NativeDisplayTrustRegistry&)=delete;NativeDisplayTrustRegistry& operator=(const NativeDisplayTrustRegistry&)=delete;
};
inline bool recordNativeDisplayTrust(const std::string& store,const std::string& thumbprint){
  const auto* name=nativeDisplayTrustStoreName(store);if(!name||!nativeDisplayThumbprint(thumbprint))return false;
  NativeDisplayTrustRegistry registry(KEY_SET_VALUE|READ_CONTROL,true);
  if(!registry.key||!machineRegistryKeyTrusted(registry.key))return false;
  const auto text=wide(thumbprint);
  return RegSetValueExW(registry.key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text.c_str()),DWORD((text.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS&&RegFlushKey(registry.key)==ERROR_SUCCESS;
}
inline void forgetNativeDisplayTrust(const std::string& store){
  const auto* name=nativeDisplayTrustStoreName(store);if(!name)return;
  NativeDisplayTrustRegistry registry(KEY_SET_VALUE);
  if(registry.key)RegDeleteValueW(registry.key,name);
}
// The stores recorded for exactly this certificate.
inline std::vector<std::string> readNativeDisplayTrust(const std::string& thumbprint){
  std::vector<std::string> out;
  if(!nativeDisplayThumbprint(thumbprint))return out;
  NativeDisplayTrustRegistry registry(KEY_QUERY_VALUE|READ_CONTROL);
  if(!registry.key||!machineRegistryKeyTrusted(registry.key))return out;
  for(const char* store:nativeDisplayTrustStores){
    wchar_t value[64]{};DWORD bytes=sizeof(value)-sizeof(wchar_t);
    if(RegGetValueW(registry.key,nullptr,nativeDisplayTrustStoreName(store),RRF_RT_REG_SZ,nullptr,value,&bytes)==ERROR_SUCCESS&&_wcsicmp(value,wide(thumbprint).c_str())==0)out.emplace_back(store);
  }
  return out;
}
inline bool nativeDisplayTrustJournalWasChecked(){
  NativeDisplayTrustRegistry registry(KEY_QUERY_VALUE|READ_CONTROL);
  if(!registry.key||!machineRegistryKeyTrusted(registry.key))return false;
  DWORD value=0,bytes=sizeof(value);
  return RegGetValueW(registry.key,nullptr,nativeDisplayTrustJournalChecked,RRF_RT_REG_DWORD,nullptr,&value,&bytes)==ERROR_SUCCESS&&value==1;
}
inline bool markNativeDisplayTrustJournalChecked(){
  NativeDisplayTrustRegistry registry(KEY_SET_VALUE|READ_CONTROL,true);
  if(!registry.key||!machineRegistryKeyTrusted(registry.key))return false;
  const DWORD one=1;
  return RegSetValueExW(registry.key,nativeDisplayTrustJournalChecked,0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one))==ERROR_SUCCESS;
}
// The certificate with this SHA-1 thumbprint is in the local-machine store (read only).
inline bool certificateInMachineStore(const std::string& store,const std::string& thumbprint){
  const auto* name=nativeDisplayTrustStoreName(store);if(!name||!nativeDisplayThumbprint(thumbprint))return false;
  BYTE digest[20]{};for(size_t index=0;index<sizeof(digest);++index)digest[index]=BYTE(std::stoul(thumbprint.substr(index*2,2),nullptr,16));
  const HCERTSTORE opened=CertOpenStore(CERT_STORE_PROV_SYSTEM_W,0,0,CERT_SYSTEM_STORE_LOCAL_MACHINE|CERT_STORE_OPEN_EXISTING_FLAG|CERT_STORE_READONLY_FLAG,name);
  if(!opened)return false;
  CRYPT_HASH_BLOB hash{sizeof(digest),digest};
  const auto found=CertFindCertificateInStore(opened,X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,0,CERT_FIND_SHA1_HASH,&hash,nullptr);
  if(found)CertFreeCertificateContext(found);
  CertCloseStore(opened,0);return found!=nullptr;
}
// The operational journal's lines, oldest first (operational_log.hpp writes
// events.jsonl and keeps one rotated file).
inline std::optional<std::vector<std::string>> readHostJournalLines(){
  try{
    const auto directory=machineDirectory();
    if(GetFileAttributesW(directory.c_str())==INVALID_FILE_ATTRIBUTES)return std::vector<std::string>{};
    if(!trustedMachineFile(directory))return std::nullopt;
    std::vector<std::string> lines;
    for(const auto* name:{L"events.previous.jsonl",L"events.jsonl"}){
      const auto path=directory/name;const DWORD attributes=GetFileAttributesW(path.c_str());
      if(attributes==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)continue;return std::nullopt;}
      if((attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!trustedMachineFile(path)||std::filesystem::file_size(path)>262144)return std::nullopt;
      std::ifstream file(path,std::ios::binary);std::string line;
      while(std::getline(file,line))if(!line.empty()&&line.size()<=2048)lines.push_back(line);
    }
    return lines;
  }catch(...){return std::nullopt;}
}
inline std::vector<std::string> backfillNativeDisplayTrustFromJournal(const std::string& thumbprint) noexcept {
  try{
    NativeDisplayTrustBackfill access;
    access.checked=nativeDisplayTrustJournalWasChecked;
    access.recorded=[&]{return readNativeDisplayTrust(thumbprint);};
    access.journal=readHostJournalLines;
    access.present=[&](const std::string& store){return certificateInMachineStore(store,thumbprint);};
    access.record=[&](const std::string& store){return recordNativeDisplayTrust(store,thumbprint);};
    access.markChecked=markNativeDisplayTrustJournalChecked;
    return backfillNativeDisplayTrust(access);
  }catch(...){return {};}
}
// The Host service calls this while it runs; the work happens once per process.
inline void backfillNativeDisplayTrustOnce(const std::string& thumbprint) noexcept {
  static std::once_flag once;
  try{std::call_once(once,[&]{(void)backfillNativeDisplayTrustFromJournal(thumbprint);});}catch(...){ }
}
}  // namespace host
