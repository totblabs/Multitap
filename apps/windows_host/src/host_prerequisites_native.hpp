#pragma once
// The machine side of host-prerequisites-v1 (host_prerequisites.hpp holds every
// decision). Run only by the elevated settings RPC of the installed Host:
//  - the package is this Host's own bin\native-display-driver, administrator
//    owned, byte for byte the pinned original (setup_pnp.hpp pins());
//  - the certificate is that package's .cer, and only if its SHA-1 thumbprint is
//    the pinned publisher; it is added to LocalMachine Root and TrustedPublisher
//    and to no other store, and nothing else is ever added to a trust store;
//  - the device calls are setup_pnp.hpp SetupApi.
// No Windows policy, test mode, Defender or UAC value is read or changed, and
// Windows is never restarted.
#include "machine.hpp"
#include "host_prerequisites.hpp"
#include "host_service_restart.hpp"
#include "display_adapter_inventory.hpp"
#include "native_display_trust.hpp"
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#pragma comment(lib,"crypt32.lib")
#pragma comment(lib,"wintrust.lib")

namespace host::prerequisites {
inline std::array<BYTE,20> publisherDigest(){
  std::array<BYTE,20> out{};const std::string text=PublisherThumbprint;
  for(size_t i=0;i<out.size();++i)out[i]=BYTE(std::stoul(text.substr(i*2,2),nullptr,16));
  return out;
}
inline bool publisherCertificate(PCCERT_CONTEXT certificate){
  BYTE digest[20]{};DWORD size=sizeof(digest);const auto expected=publisherDigest();
  return certificate&&CertGetCertificateContextProperty(certificate,CERT_SHA1_HASH_PROP_ID,digest,&size)&&size==sizeof(digest)&&std::memcmp(digest,expected.data(),sizeof(digest))==0;
}
struct CertificateStore {
  HCERTSTORE value=nullptr;
  CertificateStore(const wchar_t* name,bool write):value(CertOpenStore(CERT_STORE_PROV_SYSTEM_W,0,0,CERT_SYSTEM_STORE_LOCAL_MACHINE|CERT_STORE_OPEN_EXISTING_FLAG|(write?0:CERT_STORE_READONLY_FLAG),name)){}
  ~CertificateStore(){if(value)CertCloseStore(value,0);}
  CertificateStore(const CertificateStore&)=delete;CertificateStore& operator=(const CertificateStore&)=delete;
  // The pinned publisher's entry, or null. The caller frees it.
  PCCERT_CONTEXT publisher()const{
    if(!value)return nullptr;
    auto digest=publisherDigest();CRYPT_HASH_BLOB hash{DWORD(digest.size()),digest.data()};
    return CertFindCertificateInStore(value,X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,0,CERT_FIND_SHA1_HASH,&hash,nullptr);
  }
};
inline const wchar_t* publisherStoreName(const std::string& store){return store=="Root"?L"Root":store=="TrustedPublisher"?L"TrustedPublisher":nullptr;}
// The pinned publisher in both local-machine stores (read only).
inline bool publisherTrusted(){
  for(const auto* name:{L"Root",L"TrustedPublisher"}){
    CertificateStore store(name,false);const auto found=store.publisher();
    if(!found)return false;
    CertFreeCertificateContext(found);
  }
  return true;
}

class Machine final:public Access {
 public:
  Machine():directory_((std::filesystem::path(executablePath()).parent_path()/L"native-display-driver").wstring()){}
  ~Machine()override{if(certificate_)CertFreeCertificateContext(certificate_);}
  Machine(const Machine&)=delete;Machine& operator=(const Machine&)=delete;

  std::string package()override{
    namespace payload=multitap::payload;
    try{
      payload::plainChain(directory_,L"");
      const auto folder=payload::openDirectory(directory_,READ_CONTROL);payload::protectedDirectory(folder.get());
      for(const auto* pin=multitap::pnp::pins();pin->name;++pin){
        payload::Handle file(CreateFileW((directory_+L"\\"+pin->name).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
        if(!file)return "native_display_package_missing";
        LARGE_INTEGER size{};
        if((payload::attributesOf(file.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!GetFileSizeEx(file.get(),&size)||size.QuadPart<1||size.QuadPart>(64LL<<20))return "native_display_package_mismatch";
        payload::protectedDirectory(file.get());
        const auto digest=payload::parseDigest(pin->sha256);
        if(!digest||payload::hashOf(file.get(),static_cast<unsigned long long>(size.QuadPart))!=*digest)return "native_display_package_mismatch";
        if(std::wstring(pin->name).ends_with(L".cer")&&!loadCertificate(file.get(),size.QuadPart))return "native_display_publisher_mismatch";
      }
    }catch(const payload::Failure& failure){
      const std::string reason=failure.code;
      return reason=="payload_destination_missing"?"native_display_package_missing":reason=="payload_destination_unprotected"||reason=="payload_link_refused"?"native_display_package_unprotected":"native_display_package_mismatch";
    }catch(...){return "native_display_package_mismatch";}
    if(!certificate_||!publisherCertificate(certificate_))return "native_display_publisher_mismatch";
    // Outside its validity period Windows would not accept the catalog either.
    if(CertVerifyTimeValidity(nullptr,certificate_->pCertInfo)!=0)return "native_display_certificate_expired";
    return {};
  }
  bool certificateTrusted()override{return publisherTrusted();}
  bool catalogTrusted()override{
    const auto path=directory_+L"\\SunshineVirtualDisplayDriver.cat";
    WINTRUST_FILE_INFO file{sizeof(file),path.c_str(),nullptr,nullptr};
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;
    data.dwStateAction=WTD_STATEACTION_VERIFY;data.dwProvFlags=WTD_REVOCATION_CHECK_NONE|WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status=WinVerifyTrust(nullptr,&action,&data);
    // The catalog's bytes are pinned, so its signer is fixed already; a signer
    // Windows reports that is NOT the pinned publisher still stops the install.
    bool foreign=false;
    if(auto* provider=WTHelperProvDataFromStateData(data.hWVTStateData))
      if(auto* chain=WTHelperGetProvSignerFromChain(provider,0,FALSE,0))
        if(auto* first=WTHelperGetProvCertFromChain(chain,0))foreign=!publisherCertificate(first->pCert);
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&data);
    return status==ERROR_SUCCESS&&!foreign;
  }
  bool addCertificate(std::vector<std::string>& added)override{
    if(!certificate_||!publisherCertificate(certificate_))return false;
    for(const char* name:{"Root","TrustedPublisher"}){
      CertificateStore store(publisherStoreName(name),true);
      if(!store.value)return false;
      if(const auto found=store.publisher()){CertFreeCertificateContext(found);continue;}
      if(!CertAddCertificateContextToStore(store.value,certificate_,CERT_STORE_ADD_NEW,nullptr))return false;
      added.emplace_back(name);
    }
    return true;
  }
  void removeCertificate(const std::string& name)override{
    const auto* storeName=publisherStoreName(name);if(!storeName)return;
    CertificateStore store(storeName,true);
    // CertDeleteCertificateFromStore frees the context it is given.
    if(const auto found=store.publisher())CertDeleteCertificateFromStore(found);
  }
  void recordTrust(const std::string& store)override{(void)recordNativeDisplayTrust(store,PublisherThumbprint);}
  void forgetTrust(const std::string& store)override{forgetNativeDisplayTrust(store);}
  Selection selection()override{
    Selection out;const auto path=machineDirectory()/L"native-display.json";
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){
      const DWORD error=GetLastError();
      out.present=error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND;  // unreadable is "there, not understood"
      return out;
    }
    out.present=true;
    try{
      if((attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!trustedMachineFile(path)||std::filesystem::file_size(path)>4096)return out;
      std::ifstream file(path,std::ios::binary);const auto config=JsonParser::parse(std::string((std::istreambuf_iterator<char>(file)),{}));
      const auto* enabled=std::get_if<bool>(&config.get("enabled").value);const auto instance=config.get("deviceInstanceId").string();
      if(!enabled||!*enabled||config.get("provider").string()!=Provider||!nativeDisplayInstanceId(instance))return out;
      out.instance=wide(instance);out.valid=multitap::pnp::validInstance(out.instance);
      if(const auto* manager=std::get_if<std::string>(&config.get("displayManager").value))out.displayManager=*manager;
    }catch(...){out.valid=false;}
    return out;
  }
  std::vector<std::wstring> recordedNodes()override{try{return readNativeAdapterLedger();}catch(...){return {};}}
  multitap::pnp::Backend& pnp()override{return pnp_;}
  std::wstring inf()override{return directory_+L"\\SunshineVirtualDisplayDriver.inf";}
  std::string boundInf(const std::wstring& instance)override{
    try{return multitap::pnp::ExactDeviceUpgrade::inspect(instance).infSha;}catch(...){return {};}
  }
  // A new file with its own protection (administrators own and write, users
  // read), complete and flushed before it takes the name: the selection is the
  // old one or the new one, never a part of either.
  bool writeSelection(const std::string& json)override{
    try{
      if(json.empty()||json.size()>4096)return false;
      prepareMachineDirectory();
      const auto path=machineDirectory()/L"native-display.json";const auto temporary=path.wstring()+temporarySuffix();
      PSECURITY_DESCRIPTOR descriptor=nullptr;
      if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)",SDDL_REVISION_1,&descriptor,nullptr))return false;
      SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
      const HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
      LocalFree(descriptor);
      if(file==INVALID_HANDLE_VALUE)return false;
      DWORD written=0;const bool saved=WriteFile(file,json.data(),DWORD(json.size()),&written,nullptr)&&written==json.size()&&FlushFileBuffers(file);
      CloseHandle(file);
      if(!saved||!trustedMachineFile(temporary)||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());return false;}
      return true;
    }catch(...){return false;}
  }
  // What the status reads too (native_display_status.hpp): the lease snapshot is
  // a protected file written within the last five seconds by the process that
  // SCM reports as the running Host service, and it names the selected node.
  // An active lease without a pending recovery means that service opened the
  // device and holds its outputs. No new protocol.
  bool serviceUsesDevice(const std::wstring& instance)override{
    try{
      const auto snapshot=nativeDisplayLeaseSnapshot();
      return multitap::payload::sameText(wide(snapshot.get("deviceInstanceId").string()),instance)&&snapshot.get("leaseActive").boolean()&&!snapshot.get("recoveryRequired").boolean();
    }catch(...){return false;}
  }
  void wait(unsigned long milliseconds)override{Sleep(milliseconds);}
  bool restartService()override{return service_restart::native().outcome==service_restart::Outcome::completed;}

 private:
  // The .cer is one DER certificate (its bytes are pinned by SHA-256).
  bool loadCertificate(HANDLE file,long long size){
    if(certificate_){CertFreeCertificateContext(certificate_);certificate_=nullptr;}
    if(size<1||size>16384)return false;
    std::vector<BYTE> bytes(static_cast<size_t>(size));LARGE_INTEGER start{};DWORD read=0;
    if(!SetFilePointerEx(file,start,nullptr,FILE_BEGIN)||!ReadFile(file,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())return false;
    certificate_=CertCreateCertificateContext(X509_ASN_ENCODING,bytes.data(),DWORD(bytes.size()));
    return certificate_!=nullptr;
  }
  std::wstring directory_;
  PCCERT_CONTEXT certificate_=nullptr;
  multitap::pnp::SetupApi pnp_;
};

// prerequisiteStatus: read only (nothing is installed, trusted or written).
inline std::string statusJson(bool winfspInstalled,bool winfspPackage){
  Machine machine;const auto observation=observe(machine);
  DWORD session=0;
  const bool remote=!ProcessIdToSessionId(GetCurrentProcessId(),&session)||session!=WTSGetActiveConsoleSessionId()||GetSystemMetrics(SM_REMOTESESSION)!=0;
  return observationJson(observation,elevated(),remote,winfspInstalled,winfspPackage);
}
// installNativeDisplay: one installation at a time on this machine.
inline Result installNativeDisplay(bool confirmed,bool trustConfirmed){
  const auto lock=nativeConnectionLock(L"Global\\MultitapHost.NativeDisplayInstall",0);
  if(!lock){Result busy;busy.error="native_display_install_busy";return busy;}
  Machine machine;
  return install(machine,confirmed,trustConfirmed);
}
}  // namespace host::prerequisites
