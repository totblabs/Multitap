#pragma once
#include "native_display_journal.hpp"
#ifdef MULTITAP_NATIVE_DISPLAY
#include <virtual_display/driver/control_protocol.h>

namespace host {
// Exact v1 DPAPI plaintext layout, compatible with released hosts.
struct NativeDisplayIdentity {char magic[8];uint64_t lease,displays[2];virtual_display::driver::OwnerCapability owner;};
static_assert(sizeof(NativeDisplayIdentity)==64);
struct NativeDisplayIdentitySnapshot {
  std::string sealed;NativeDisplayIdentity value{};
  ~NativeDisplayIdentitySnapshot(){SecureZeroMemory(&value,sizeof(value));}
};
inline bool validNativeDisplayIdentity(const NativeDisplayIdentity& v){return !std::memcmp(v.magic,"TOTBVDD1",8)&&v.lease>=0x6000000000000000ull&&v.displays[0]&&v.displays[1]&&v.displays[0]!=v.displays[1]&&std::any_of(v.owner.bytes.begin(),v.owner.bytes.end(),[](uint8_t b){return b!=0;});}
inline bool sameNativeDisplayOwner(const NativeDisplayIdentity& a,const NativeDisplayIdentity& b){
  uint8_t difference=0;for(size_t i=0;i<a.owner.bytes.size();++i)difference|=uint8_t(a.owner.bytes[i]^b.owner.bytes[i]);
  return !difference&&!std::memcmp(a.magic,b.magic,8)&&a.displays[0]==b.displays[0]&&a.displays[1]==b.displays[1];
}
inline NativeDisplayIdentitySnapshot unsealNativeDisplayIdentity(const std::string& bytes){
  if(bytes.empty()||bytes.size()>8192)throw std::runtime_error("native_display_identity_invalid");
  NativeDisplayIdentitySnapshot result;result.sealed=bytes;DATA_BLOB input{DWORD(bytes.size()),reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()))},plain{};
  winCheck(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain),"Unprotect native display owner");
  const bool length=plain.cbData==sizeof(result.value);if(length)std::memcpy(&result.value,plain.pbData,sizeof(result.value));SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
  if(!length||!validNativeDisplayIdentity(result.value))throw std::runtime_error("native_display_identity_invalid");return result;
}
inline std::string sealNativeDisplayIdentity(const NativeDisplayIdentity& value){
  if(!validNativeDisplayIdentity(value))throw std::runtime_error("native_display_identity_invalid");
  DATA_BLOB source{sizeof(value),reinterpret_cast<BYTE*>(const_cast<NativeDisplayIdentity*>(&value))},sealed{};
  winCheck(CryptProtectData(&source,L"Multitap native display ownership",nullptr,nullptr,nullptr,CRYPTPROTECT_LOCAL_MACHINE|CRYPTPROTECT_UI_FORBIDDEN,&sealed),"Protect native display owner");
  struct Free{BYTE* bytes;~Free(){LocalFree(bytes);}}free{sealed.pbData};
  if(!sealed.cbData||sealed.cbData>8192)throw std::runtime_error("native_display_identity_invalid");return {reinterpret_cast<char*>(sealed.pbData),sealed.cbData};
}
inline NativeRuntimeJournalAccess nativeDisplayIdentityFileAccess(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  if(path.filename()!=L"native-display.dpapi")throw std::runtime_error("native_display_identity_path_invalid");
  return nativeJournalFileAccess(path,8192,[](const std::string& bytes){(void)unsealNativeDisplayIdentity(bytes);},std::move(trusted));
}
// Writers hold the service's exclusive recovery reservation. Writes use exact
// ciphertext CAS, durable rename and fresh verified readback.
class NativeDisplayIdentityStore {
 public:
  explicit NativeDisplayIdentityStore(NativeRuntimeJournalAccess access):access_(std::move(access)){}
  NativeDisplayIdentitySnapshot existing()const{const auto bytes=access_.load();if(!bytes)throw std::runtime_error("native_display_identity_missing");return unsealNativeDisplayIdentity(*bytes);}
  NativeDisplayIdentitySnapshot getOrCreate(){
    const auto bytes=access_.load();if(bytes)return unsealNativeDisplayIdentity(*bytes);
    NativeDisplayIdentitySnapshot created;winCheck(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&created.value),sizeof(created.value),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Create native display owner");
    std::memcpy(created.value.magic,"TOTBVDD1",8);created.value.lease|=0x6000000000000000ull;created.sealed=sealNativeDisplayIdentity(created.value);
    if(!access_.save({},created.sealed)||access_.load()!=created.sealed)throw std::runtime_error("native_display_identity_save_failed");return created;
  }
  bool rotate(const NativeDisplayIdentitySnapshot& before,uint64_t replacement,const std::function<bool()>& authorized)noexcept{
    try{
      if(!validNativeDisplayIdentity(before.value)||replacement<0x6000000000000000ull||replacement==before.value.lease||!authorized||!authorized())return false;
      const auto sealedBefore=unsealNativeDisplayIdentity(before.sealed);
      if(!sameNativeDisplayOwner(before.value,sealedBefore.value)||before.value.lease!=sealedBefore.value.lease)return false;
      const auto current=existing();if(!sameNativeDisplayOwner(before.value,current.value))return false;
      if(current.value.lease==replacement)return authorized()&&access_.load()==current.sealed;
      if(current.sealed!=before.sealed||current.value.lease!=before.value.lease)return false;
      auto next=before;next.value.lease=replacement;next.sealed=sealNativeDisplayIdentity(next.value);
      if(!authorized()||access_.load()!=before.sealed)return false;
      // False can be a lost write ACK. Read back; never issue a second write
      // or recreate ownership after missing or ambiguous state.
      (void)access_.save(before.sealed,next.sealed);const auto observed=existing();
      return observed.sealed==next.sealed&&observed.value.lease==replacement&&sameNativeDisplayOwner(before.value,observed.value)&&authorized()&&access_.load()==observed.sealed;
    }catch(...){return false;}
  }
 private:NativeRuntimeJournalAccess access_;
};
}
#endif
