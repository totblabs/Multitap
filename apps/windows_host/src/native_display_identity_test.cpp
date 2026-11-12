#include "native_display_identity.hpp"
#include <iostream>
#include <source_location>
#ifdef MULTITAP_NATIVE_DISPLAY
using namespace host;
namespace {
int checks=0;
void require(bool ok,const std::source_location at=std::source_location::current()){if(!ok)throw std::runtime_error("identity check at "+std::to_string(at.line()));++checks;}
template<class F>void rejected(F action){bool threw=false;try{action();}catch(...){threw=true;}require(threw);}
struct Memory {
  std::optional<std::string> bytes;bool fail=false,lostAck=false,corrupt=false;int saves=0;std::function<void()> afterSave;
  NativeRuntimeJournalAccess access(){return {[this]{return bytes;},[this](const auto& expected,const auto& desired){++saves;if(fail||expected!=bytes)return false;bytes=corrupt?"bad readback":desired;if(afterSave)afterSave();return !lostAck;}};}
};
NativeDisplayIdentitySnapshot fixture(){
  NativeDisplayIdentitySnapshot r;std::memcpy(r.value.magic,"TOTBVDD1",8);r.value.lease=0xfedcba9876543210ull;r.value.displays[0]=0xabcdef1234567890ull;r.value.displays[1]=0x123456abcdef7890ull;r.value.owner.bytes.fill(17);r.sealed=sealNativeDisplayIdentity(r.value);return r;
}
void compatible(){
  const auto value=fixture();const auto decoded=unsealNativeDisplayIdentity(value.sealed);require(value.value.lease==decoded.value.lease&&sameNativeDisplayOwner(value.value,decoded.value));
  // The old 64-byte plaintext and LOCAL_MACHINE flag, independently sealed
  // through Win32, must remain readable. No existing product state is read.
  DATA_BLOB plain{sizeof(value.value),reinterpret_cast<BYTE*>(const_cast<NativeDisplayIdentity*>(&value.value))},legacy{};require(CryptProtectData(&plain,L"Multitap native display ownership",nullptr,nullptr,nullptr,CRYPTPROTECT_LOCAL_MACHINE|CRYPTPROTECT_UI_FORBIDDEN,&legacy)!=FALSE);
  const std::string bytes(reinterpret_cast<char*>(legacy.pbData),legacy.cbData);LocalFree(legacy.pbData);const auto old=unsealNativeDisplayIdentity(bytes);require(old.value.lease==value.value.lease&&sameNativeDisplayOwner(old.value,value.value));
  for(const auto& bad:{std::string(),std::string("not a DPAPI blob"),std::string(8193,'x')})rejected([&]{(void)unsealNativeDisplayIdentity(bad);});
  for(int scenario=0;scenario<5;++scenario){auto bad=value;switch(scenario){case 0:bad.value.magic[0]='X';break;case 1:bad.value.lease=1;break;case 2:bad.value.displays[0]=0;break;case 3:bad.value.displays[1]=bad.value.displays[0];break;case 4:bad.value.owner.bytes.fill(0);break;}rejected([&]{(void)sealNativeDisplayIdentity(bad.value);});}
}
void transactions(){
  {Memory m;NativeDisplayIdentityStore store(m.access());rejected([&]{(void)store.existing();});const auto created=store.getOrCreate();require(validNativeDisplayIdentity(created.value)&&m.saves==1&&m.bytes==created.sealed);const auto again=store.getOrCreate();require(again.sealed==created.sealed&&sameNativeDisplayOwner(again.value,created.value)&&m.saves==1);}
  {Memory m;m.fail=true;NativeDisplayIdentityStore store(m.access());rejected([&]{(void)store.getOrCreate();});require(!m.bytes&&m.saves==1);}
  {Memory m;m.lostAck=true;NativeDisplayIdentityStore store(m.access());rejected([&]{(void)store.getOrCreate();});const auto first=m.bytes;m.lostAck=false;require(store.getOrCreate().sealed==first&&m.saves==1);}
  {Memory m;m.corrupt=true;NativeDisplayIdentityStore store(m.access());rejected([&]{(void)store.getOrCreate();});m.corrupt=false;rejected([&]{(void)store.getOrCreate();});require(m.saves==1);}
  for(int scenario=0;scenario<6;++scenario){Memory m;const auto before=fixture();m.bytes=before.sealed;NativeDisplayIdentityStore store(m.access());bool permission=true;
    if(scenario==1)m.lostAck=true;if(scenario==2)m.fail=true;if(scenario==3)m.corrupt=true;if(scenario==4)permission=false;if(scenario==5)m.afterSave=[&]{permission=false;};
    const auto allowed=[&]{return permission;};const bool changed=store.rotate(before,before.value.lease+1,allowed);require(changed==(scenario<=1));
    if(scenario==2||scenario==4)require(m.bytes==before.sealed);
    if(scenario<=1||scenario==5){permission=true;const auto current=store.existing();require(current.value.lease==before.value.lease+1&&sameNativeDisplayOwner(before.value,current.value));const auto writes=m.saves;require(store.rotate(before,before.value.lease+1,allowed)&&m.saves==writes);}
  }
  for(int scenario=0;scenario<7;++scenario){Memory m;const auto before=fixture();m.bytes=before.sealed;NativeDisplayIdentityStore store(m.access());auto wrong=before;
    if(scenario==0)wrong.value.owner.bytes[0]++;if(scenario==1)wrong.value.displays[1]++;if(scenario==2)wrong.value.lease++;
    if(scenario==3)m.bytes.reset();if(scenario==4){auto changed=before;changed.value.owner.bytes[0]++;m.bytes=sealNativeDisplayIdentity(changed.value);}
    if(scenario==5)m.bytes=sealNativeDisplayIdentity(before.value);if(scenario==6)m.bytes="corrupt";
    const auto observed=m.bytes;require(!store.rotate(wrong,before.value.lease+1,[]{return true;})&&m.bytes==observed&&!m.saves);
  }
}
void filePersistence(){
  const auto directory=std::filesystem::current_path()/(L"native-identity-qa-"+randomName());require(std::filesystem::create_directory(directory));const auto path=directory/L"native-display.dpapi";
  struct Cleanup{std::filesystem::path file,dir;~Cleanup(){std::error_code e;std::filesystem::remove(file,e);std::filesystem::remove(dir,e);}}cleanup{path,directory};
  const auto trusted=[&](const auto& p){return p==directory||p.parent_path()==directory;};const auto access=nativeDisplayIdentityFileAccess(path,trusted);NativeDisplayIdentityStore store(access);const auto before=store.getOrCreate();require(access.load()==before.sealed);
  require(store.rotate(before,before.value.lease+1,[]{return true;}));NativeDisplayIdentityStore reopened(access);const auto after=reopened.existing();require(after.value.lease==before.value.lease+1&&sameNativeDisplayOwner(after.value,before.value));require(!access.save(before.sealed,before.sealed)&&access.load()==after.sealed);
  NativeDisplayIdentityStore denied(nativeDisplayIdentityFileAccess(path,[](const auto&){return false;}));rejected([&]{(void)denied.existing();});require(!denied.rotate(before,before.value.lease+2,[]{return true;}));
  rejected([&]{(void)nativeDisplayIdentityFileAccess(directory/L"different.dpapi",trusted);});require(!access.save(after.sealed,"bad DPAPI")&&access.load()==after.sealed);
}
}
int main(){try{compatible();transactions();filePersistence();std::cout<<"Native identity storage: "<<checks<<" checks passed; actual Windows machine-DPAPI and workspace file CAS/flush/readback; production identity, service and driver unchanged.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
#else
int main(){std::cout<<"Native identity storage unavailable in SDK-less build; no machine identity access.\n";return 0;}
#endif
