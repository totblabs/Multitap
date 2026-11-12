#include "native_stream_console_identity.hpp"
#include <iostream>
using namespace host;
namespace {
int checks=0;
void require(bool condition,const char* label){if(!condition)throw std::runtime_error(label);++checks;}
LUID adapter(DWORD low){return {low,-1};}
const std::string uuid="01234567-89ab-cdef-8123-456789abcdef";
NativeConsoleModelProof proof(){return {uuid,"std"};}
NativeConsoleAdapterIdentity device(LUID luid){
  return {luid,L"\\\\?\\PCI#VEN_1234&DEV_1111#4&ABCD&0&0010#{GUID}",L"PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02\\4&ABCD&0&0010",L"PCI",
    {L"PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02",L"PCI\\VEN_1234&DEV_1111&CC_030000"},nativeConsoleDisplayClass,true,true};
}
DISPLAYCONFIG_PATH_INFO path(LUID luid,UINT32 source,UINT32 target){
  DISPLAYCONFIG_PATH_INFO result{};result.flags=DISPLAYCONFIG_PATH_ACTIVE;result.sourceInfo.adapterId=result.targetInfo.adapterId=luid;
  result.sourceInfo.id=source;result.targetInfo.id=target;result.targetInfo.targetAvailable=TRUE;return result;
}
struct Fixture {
  NativeConsoleModelProof model=proof();std::string current=uuid;bool remote=false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths={path(adapter(11),7,8),path(adapter(22),0,100),path(adapter(22),1,101)};
  std::vector<NativeDisplayTarget> owned={{"0",adapter(22),100,1920,1080},{"1",adapter(22),101,1280,720}};
  std::map<DWORD,NativeConsoleAdapterIdentity> devices={{11,device(adapter(11))}};
  int calls=0;
  NativeConsoleIdentityResult identify(){return identifyNativeStreamConsole(model,current,remote,paths,owned,[&](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{
    ++calls;const auto found=devices.find(luid.LowPart);if(found==devices.end())return {};return found->second;
  });}
  NativeQemuConsoleObservation observe(const std::optional<NativeConsoleOutputKey>& expected={}){
    return observeNativeQemuConsoleOutput(model,current,remote,paths,owned,[&](LUID luid)->std::optional<NativeConsoleAdapterIdentity>{
      ++calls;const auto found=devices.find(luid.LowPart);if(found==devices.end())return {};return found->second;
    },expected);
  }
};
template<class Change>void rejected(Change change,const char* message,const char* reason=nullptr){
  Fixture fixture;change(fixture);const auto result=fixture.identify();require(!result.binding,message);if(reason)require(result.reason==reason,"Wrong fixed failure reason");
}
void outputObservationCases(){
  {Fixture f;const auto before=f.paths;const auto result=f.observe();
    require(result.output&&result.output->active&&result.output->target==8&&result.reason=="console_output_active","Active QEMU output was not observed");
    require(f.calls==1&&std::memcmp(f.paths.data(),before.data(),f.paths.size()*sizeof(DISPLAYCONFIG_PATH_INFO))==0,"Read-only observation changed CCD or resolved owned adapters");}
  {Fixture f;f.paths[0].flags=0;const auto result=f.observe();
    require(result.output&&!result.output->active&&result.reason=="console_output_inactive","Inactive available QEMU output was mistaken for active or unidentified");
    require(!f.identify().binding,"New observation weakened the existing active-only streaming identity gate");}
  {Fixture f;f.paths[0].flags=0;auto alternative=f.paths[0];alternative.sourceInfo.id=19;f.paths.push_back(alternative);
    const auto result=f.observe();require(result.output&&!result.output->active&&result.output->target==8&&f.calls==1,"Inactive source alternatives fabricated a second output or selected a source");}
  {Fixture f;auto alternative=f.paths[0];alternative.flags=0;alternative.sourceInfo.id=19;f.paths.push_back(alternative);
    const auto result=f.observe();require(result.output&&result.output->active,"Inactive source alternatives hid the single actual active path");}
  {Fixture f;f.owned.clear();f.paths.resize(1);const auto result=f.observe();require(result.output&&result.output->active,"Read-only QEMU observation incorrectly required a Multitap virtual output");}
  {Fixture f;auto foreign=device(adapter(33));foreign.instanceId=L"PCI\\VEN_10DE&DEV_0001\\1";foreign.hardwareIds={L"PCI\\VEN_10DE&DEV_0001"};f.devices.emplace(33,foreign);f.paths.push_back(path(adapter(33),1,9));
    const auto before=f.paths;const auto result=f.observe();require(result.output&&result.output->target==8&&result.output->active&&std::memcmp(f.paths.data(),before.data(),f.paths.size()*sizeof(DISPLAYCONFIG_PATH_INFO))==0,"An unrelated display changed or became the QEMU output");}
  {Fixture f;require(f.observe(NativeConsoleOutputKey{adapter(11),8}).output.has_value(),"Exact saved output key rejected");
    require(!f.observe(NativeConsoleOutputKey{adapter(11),9}).output&&f.observe(NativeConsoleOutputKey{adapter(11),9}).reason=="console_identity_binding_changed","A different saved target was silently rebound");
    require(!f.observe(NativeConsoleOutputKey{adapter(12),8}).output,"A stale saved adapter was silently rebound");}
  const auto reject=[](auto change,const char* reason){Fixture f;change(f);const auto result=f.observe();require(!result.output&&result.reason==reason,"Unsafe QEMU observation accepted or fixed failure reason lost");};
  reject([](auto& f){f.remote=true;},"console_identity_remote_session");
  reject([](auto& f){f.current="fedcba98-7654-4321-8123-456789abcdef";},"console_identity_model_unverified");
  reject([](auto& f){f.model.model="qxl";},"console_identity_model_unverified");
  reject([](auto& f){f.paths[0].targetInfo.targetAvailable=FALSE;},"console_identity_not_found");
  reject([](auto& f){f.paths[0].sourceInfo.adapterId=adapter(33);},"console_identity_cross_adapter");
  reject([](auto& f){f.paths[0].sourceInfo.id=UINT32_MAX;},"console_identity_topology_invalid");
  reject([](auto& f){f.devices.at(11).started=false;},"console_identity_unresolved");
  reject([](auto& f){f.devices.at(11).present=false;},"console_identity_unresolved");
  reject([](auto& f){f.devices.at(11).classGuid={};},"console_identity_unresolved");
  reject([](auto& f){f.devices.at(11).hardwareIds={L"PCI\\VEN_1234&DEV_11110"};},"console_identity_not_found");
  reject([](auto& f){f.devices.clear();},"console_identity_unresolved");
  reject([](auto& f){f.paths.push_back(f.paths[0]);},"console_identity_ambiguous");
  reject([](auto& f){auto duplicate=f.paths[0];duplicate.flags=f.paths[0].flags=0;duplicate.targetInfo.id=9;f.paths.push_back(duplicate);},"console_identity_ambiguous");
  reject([](auto& f){f.paths.push_back(path(adapter(33),0,9));f.devices.emplace(33,device(adapter(33)));},"console_identity_ambiguous");
  reject([](auto& f){f.owned.push_back(f.owned[0]);},"console_identity_topology_invalid");
  reject([](auto& f){f.owned[0].adapter=adapter(11);},"console_identity_owned_adapter");
  reject([](auto& f){f.owned[0].id="foreign";},"console_identity_topology_invalid");
  reject([](auto& f){f.paths.resize(4097,f.paths[1]);},"console_identity_topology_invalid");
  reject([](auto& f){f.paths.resize(65,f.paths[1]);},"console_identity_topology_invalid");
  {Fixture f;f.model.model="qxl";require(!f.observe().output&&f.calls==0,"Unsupported model queried live devices");}
  {Fixture f;const auto result=observeNativeQemuConsoleOutput(f.model,f.current,false,f.paths,f.owned,[](LUID)->std::optional<NativeConsoleAdapterIdentity>{throw std::runtime_error("PRIVATE");});require(!result.output&&result.reason=="console_identity_unresolved","Resolver exception escaped or became positive output proof");}
}
}
int main(){try{
  Fixture original;const auto selected=original.identify();require(bool(selected.binding),"Exact std console rejected");
  require(selected.reason=="console_identity_verified"&&original.calls==1,"Owned adapters were resolved or success reason changed");
  require(selected.binding->path.source==7&&selected.binding->path.target==8&&nativeConsoleLuidEqual(selected.binding->path.sourceAdapter,adapter(11)),"Exact private path binding was lost");
  require(selected.binding->device.instanceId==device(adapter(11)).instanceId,"Stable instance identity lost");
  {Fixture fixture;fixture.model.machineUuid="01234567-89AB-CDEF-8123-456789ABCDEF";auto& value=fixture.devices.at(11);
   for(auto& ch:value.instanceId)if(ch>=L'A'&&ch<=L'Z')ch=wchar_t(ch-L'A'+L'a');std::reverse(value.hardwareIds.begin(),value.hardwareIds.end());
   const auto result=fixture.identify();require(result.binding&&sameNativeConsoleBinding(*selected.binding,*result.binding),"Case/order canonicalization changed binding");}
  rejected([](auto& f){f.remote=true;},"RDP session accepted","console_identity_remote_session");
  for(const auto* model:{"","qxl","virtio","vmware","standard","STD","std "}){
    rejected([&](auto& f){f.model.model=model;},"Unproven model accepted");
  }
  rejected([](auto& f){f.model.model=std::string("std\0junk",8);},"Embedded model NUL accepted");
  for(const std::string bad:{"", "00000000-0000-0000-0000-000000000000","ffffffff-ffff-ffff-ffff-ffffffffffff","{01234567-89ab-cdef-8123-456789abcdef}","01234567-89ab-cdef-8123-456789abcdeg","0123456789abcdef8123456789abcdef"}){
    rejected([&](auto& f){f.current=bad;},"Invalid current UUID accepted");rejected([&](auto& f){f.model.machineUuid=bad;},"Invalid model UUID accepted");
  }
  rejected([](auto& f){f.current[0]='f';},"Different VM identity accepted","console_identity_model_unverified");
  rejected([](auto& f){f.paths.clear();},"Missing topology accepted");
  rejected([](auto& f){f.paths.resize(4097);},"Unbounded topology accepted");
  rejected([](auto& f){f.owned.clear();},"No verified owned lease accepted");
  rejected([](auto& f){f.owned.push_back(f.owned[0]);},"Oversized owned set accepted");
  rejected([](auto& f){f.paths[0].targetInfo.targetAvailable=FALSE;},"Unavailable target accepted");
  rejected([](auto& f){f.paths[0].flags=0;},"Inactive console accepted","console_identity_not_found");
  rejected([](auto& f){f.paths.push_back(f.paths[0]);},"Duplicate active target accepted","console_identity_ambiguous");
  rejected([](auto& f){f.paths.push_back(path(adapter(11),9,9));},"Multiple active targets accepted","console_identity_ambiguous");
  rejected([](auto& f){f.paths.push_back(path(adapter(33),3,4));f.devices.emplace(33,device(adapter(33)));},"Two std devices accepted","console_identity_ambiguous");
  rejected([](auto& f){f.paths[0].sourceInfo.adapterId=adapter(33);},"Cross-adapter console accepted","console_identity_cross_adapter");
  rejected([](auto& f){f.devices.at(11).adapter=adapter(33);},"Wrong resolved LUID accepted","console_identity_unresolved");
  rejected([](auto& f){f.devices.clear();},"Unresolved adapter accepted","console_identity_unresolved");
  rejected([](auto& f){f.paths.push_back(path(adapter(33),3,4));},"Unresolved additional candidate ignored","console_identity_unresolved");
  rejected([](auto& f){f.owned[0].adapter=adapter(11);},"Owned adapter sibling suppressed","console_identity_owned_adapter");
  rejected([](auto& f){f.owned[0].adapter=adapter(11);f.owned[0].target=8;},"Owned exact target suppressed","console_identity_unresolved");
  {Fixture fixture;auto inactive=path(adapter(99),1,2);inactive.flags=0;fixture.paths.push_back(inactive);require(bool(fixture.identify().binding),"Unresolved inactive target prevented exact active selection");}
  {Fixture fixture;auto other=device(adapter(33));other.hardwareIds={L"PCI\\VEN_10DE&DEV_2684"};other.instanceId=L"PCI\\VEN_10DE&DEV_2684\\4&0000&0&0001";
   fixture.devices.emplace(33,other);fixture.paths.push_back(path(adapter(33),2,3));require(bool(fixture.identify().binding),"Unrelated resolved display was not preserved");}
  rejected([](auto& f){auto other=device(adapter(33));other.hardwareIds={L"PCI\\VEN_10DE&DEV_2684"};f.devices.emplace(33,other);auto extra=path(adapter(33),2,3);extra.sourceInfo.adapterId=adapter(11);f.paths.push_back(extra);},"Additional source on console adapter ignored","console_identity_ambiguous");
  for(const auto* wrong:{L"ROOT",L"RDP",L"PCI-EXTRA",L" PCI"})rejected([&](auto& f){f.devices.at(11).enumerator=wrong;},"Non-PCI enumerator accepted");
  rejected([](auto& f){f.devices.at(11).present=false;},"Absent device accepted");
  rejected([](auto& f){f.devices.at(11).started=false;},"Stopped device accepted");
  rejected([](auto& f){f.devices.at(11).classGuid.Data1^=1;},"Non-display class accepted");
  rejected([](auto& f){f.devices.at(11).hardwareIds.clear();},"Missing hardware IDs accepted");
  rejected([](auto& f){f.devices.at(11).hardwareIds.resize(17,L"PCI\\VEN_1234&DEV_1111");},"Oversized hardware IDs accepted");
  rejected([](auto& f){f.devices.at(11).hardwareIds.push_back(f.devices.at(11).hardwareIds[0]);},"Duplicate hardware ID accepted");
  for(const std::wstring wrong:{L"XPCI\\VEN_1234&DEV_1111",L"PCI\\VEN_1234&DEV_11110",L"PCI\\VEN_12345&DEV_1111",L"PCI\\VEN_1234&DEV_1111&FOO_02",L"PCI\\VEN_1234&DEV_1111&REV_02&REV_03",L"PCI\\VEN_1234&DEV_1111&CC_03000",L"PCI\\VEN_1234&DEV_1111&SUBSYS_1",L"PCI\\VEN_1234&DEV_1111&DEV_9999",L"PCI\\VEN_1234&DEV_1111&",L"PCI\\VEN_1234&DEV_1111SUFFIX",L"ROOT\\VEN_1234&DEV_1111",L"PCI\\VEN_1234&DEV_111１",L"PCI\\VEN_1234&DEV_1111&REV_G2"})
    rejected([&](auto& f){f.devices.at(11).hardwareIds={wrong};},"Hardware ID substring/lookalike accepted");
  for(const std::wstring wrong:{L"ROOT\\DISPLAY\\0000",L"PCI\\VEN_1234&DEV_1111",L"PCI\\VEN_1234&DEV_1111\\",L"PCI\\VEN_1234&DEV_1111\\4&0\\EXTRA",L"PCI\\VEN_1234&DEV_11110\\4&0",L"PCI\\VEN_1234&DEV_1111\\A/B"})
    rejected([&](auto& f){f.devices.at(11).instanceId=wrong;},"Invalid instance identity accepted");
  rejected([](auto& f){f.devices.at(11).instanceId.resize(201,L'A');},"Unbounded instance ID accepted");
  rejected([](auto& f){f.devices.at(11).hardwareIds[0].resize(201,L'A');},"Unbounded hardware ID accepted");
  rejected([](auto& f){f.devices.at(11).enumerator.resize(33,L'A');},"Unbounded enumerator accepted");
  rejected([](auto& f){f.devices.at(11).hardwareIds[0].push_back(L'\0');},"Embedded hardware NUL accepted");
  rejected([](auto& f){f.devices.at(11).interfacePath=L"friendly QEMU VGA";},"Friendly name accepted as interface identity");
  rejected([](auto& f){f.devices.at(11).interfacePath=L"\\\\?\\";},"Empty opaque interface accepted");
  rejected([](auto& f){f.devices.at(11).interfacePath.resize(128,L'A');},"Unbounded interface accepted");
  rejected([](auto& f){f.devices.at(11).interfacePath.push_back(L'\n');},"Interface control character accepted");
  rejected([](auto& f){f.paths[0].sourceInfo.id=UINT32_MAX;},"Invalid source key accepted");
  {Fixture f;const auto result=identifyNativeStreamConsole(f.model,f.current,false,f.paths,f.owned,[](LUID)->std::optional<NativeConsoleAdapterIdentity>{throw std::runtime_error("PRIVATE");});require(!result.binding&&result.reason=="console_identity_unresolved","Resolver exception leaked or accepted");}
  {Fixture f;require(!identifyNativeStreamConsole(f.model,f.current,false,f.paths,f.owned,{}).binding,"Missing resolver accepted");}
  {Fixture f;f.paths.resize(65,f.paths[1]);require(!f.identify().binding,"Oversized active topology accepted");}
  {auto altered=*selected.binding;altered.device.instanceId.back()=L'1';require(!sameNativeConsoleBinding(*selected.binding,altered),"Different private instance considered same");}
  {auto altered=*selected.binding;altered.path.target++;require(!sameNativeConsoleBinding(*selected.binding,altered),"Different target considered same");}
  {auto altered=*selected.binding;altered.path.source++;require(!sameNativeConsoleBinding(*selected.binding,altered),"Different source considered same");}
  {auto altered=*selected.binding;altered.proof.machineUuid[0]='f';require(!sameNativeConsoleBinding(*selected.binding,altered),"Different proof machine considered same");}
  {auto altered=*selected.binding;altered.device.hardwareIds={L"PCI\\VEN_1234&DEV_1111"};require(!sameNativeConsoleBinding(*selected.binding,altered),"Different hardware proof considered same");}
  {auto altered=*selected.binding;altered.device.interfacePath+=L"X";require(!sameNativeConsoleBinding(*selected.binding,altered),"Different interface considered same");}
  {auto altered=*selected.binding;altered.device.started=false;require(!sameNativeConsoleBinding(*selected.binding,altered),"Stopped binding considered same");}
  {auto altered=*selected.binding;altered.proof.model="qxl";require(!canonicalNativeConsoleBinding(altered),"Journal binding accepted unsupported model");}
  outputObservationCases();
  std::cout<<"Native console identity: "<<checks<<" checks passed. Pure injected fixtures only; live SetupAPI resolver was not invoked.\n";return 0;
}catch(const std::exception& error){std::cerr<<"Native console identity failed: "<<error.what()<<'\n';return 1;}}
