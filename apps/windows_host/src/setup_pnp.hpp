#pragma once
// The virtual display device operations of a Host install (host_prerequisites.hpp):
// device status and inspection, creating the root node from the pinned original
// driver package, labelling it and removing it, through SetupAPI and CfgMgr.
// The caller keeps every decision (publisher trust, kept or created node,
// selection, restart flags); this header only performs the calls.
#include "setup_payload.hpp"
#include "setup_pnp_upgrade.hpp"
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"newdev.lib")
#pragma comment(lib,"cfgmgr32.lib")
#include <string>
#include <vector>

namespace multitap::pnp {

constexpr const wchar_t* HardwareId=L"Root\\SunshineVirtualDisplay";
constexpr const wchar_t* Label=L"Multitap Virtual Display Adapter";
// display-adapters-v1 ledger (display_adapter_inventory.hpp): every node this setup
// registers is recorded the moment it exists, before anything can fail, so a node
// left behind by an interrupted install is still provably Multitap's and the Host
// service can remove it. Value name = instance ID, REG_DWORD 1.
constexpr const wchar_t* OwnedNodesKey=L"SOFTWARE\\Multitap\\Host\\NativeDisplayNodes";
inline const GUID& displayClass(){static const GUID value{0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};return value;}
// ROOT\DISPLAY\ and 4-8 hex digits: the only nodes these requests touch.
inline bool validInstance(const std::wstring& id){
  const std::wstring prefix=L"ROOT\\DISPLAY\\";
  if(id.size()<prefix.size()+4||id.size()>prefix.size()+8||!payload::startsWithText(id,prefix))return false;
  for(size_t i=prefix.size();i<id.size();++i){const wchar_t ch=id[i];if(!((ch>=L'0'&&ch<=L'9')||(ch>=L'a'&&ch<=L'f')||(ch>=L'A'&&ch<=L'F')))return false;}
  return true;
}
struct Status {unsigned long flags=0,problem=0xffffffffUL;};
// Presence is separate from CM status: a registered nonpresent node is not
// permission to create another root node. Only an exact protected selection
// may be inspected; hardware identity alone never grants product ownership.
struct Inspection {std::string presence="unknown";Status status;};
class Backend {
 public:
  virtual ~Backend()=default;
  virtual Status status(const std::wstring& id)=0;
  virtual Inspection inspect(const std::wstring&){return {};}
  virtual std::string install(const std::wstring& inf,std::wstring& instance,bool& restart)=0;
  virtual std::string label(const std::wstring& id)=0;
  virtual bool remove(const std::wstring& id)=0;
};

// A node is labelled or removed only when its class and single hardware ID match.
// This compatibility check is not proof of product ownership: the installer
// supplies an exact protected selection or its own journaled created instance.
class SetupApi final:public Backend {
 public:
  Status status(const std::wstring& id)override{
    Status out;if(!validInstance(id))return out;
    DEVINST node=0;ULONG flags=0,problem=0;std::wstring text=id;
    if(CM_Locate_DevNodeW(&node,text.data(),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS||CM_Get_DevNode_Status(&flags,&problem,node,0)!=CR_SUCCESS)return out;
    out.flags=flags;out.problem=problem;return out;
  }
  Inspection inspect(const std::wstring& id)override{
    Inspection out;if(!validInstance(id))return out;
    // No DIGCF_PRESENT: phantom/disabled registrations must be reused, not
    // silently replaced. Enumerate all classes to detect an ID rebound to a
    // foreign class. An incomplete/error enumeration is unknown, not absent.
    Set set(SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES));
    if(!set.valid())return out;
    for(DWORD index=0;index<8192;++index){
      SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
      if(!SetupDiEnumDeviceInfo(set.get(),index,&device)){
        if(GetLastError()==ERROR_NO_MORE_ITEMS)out.presence="absent";
        return out;
      }
      wchar_t instance[512]{};DWORD needed=0;
      if(!SetupDiGetDeviceInstanceIdW(set.get(),&device,instance,512,&needed))return out;
      if(!payload::sameText(instance,id))continue;
      if(device.ClassGuid!=displayClass()||!owned(set.get(),id,device)){out.presence="foreign";return out;}
      out.presence="present";out.status=status(id);return out;
    }
    return out;
  }
  std::string install(const std::wstring& inf,std::wstring& instance,bool& restart)override{
    restart=false;instance.clear();
    if(inf.empty()||inf.size()>=MAX_PATH)return "pnp_inf_invalid";
    GUID guid{};DWORD needed=0;wchar_t name[128]{};
    if(!SetupDiGetINFClassW(inf.c_str(),&guid,name,128,&needed)||guid!=displayClass())return "pnp_inf_invalid";
    Set set(SetupDiCreateDeviceInfoList(&guid,nullptr));if(!set.valid())return "pnp_unavailable";
    SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);bool registered=false;std::wstring registeredInstance;
    // A failure after registration removes the node again (best effort); the
    // script still treats the outcome as uncertain, as it always did. [instance]
    // then names that node, so a caller can check whether it is really gone
    // (host_prerequisites.hpp); it stays empty when the name could not be read.
    const auto fail=[&](const char* code){if(registered){SetupDiCallClassInstaller(DIF_REMOVE,set.get(),&device);instance=registeredInstance;}return std::string(code);};
    if(!SetupDiCreateDeviceInfoW(set.get(),L"Display",&guid,Label,nullptr,DICD_GENERATE_ID,&device))return fail("pnp_create_failed");
    static const wchar_t hardware[]=L"Root\\SunshineVirtualDisplay\0";
    if(!SetupDiSetDeviceRegistryPropertyW(set.get(),&device,SPDRP_HARDWAREID,reinterpret_cast<const BYTE*>(hardware),DWORD(sizeof(hardware))))return fail("pnp_create_failed");
    if(!SetupDiSetDeviceRegistryPropertyW(set.get(),&device,SPDRP_FRIENDLYNAME,reinterpret_cast<const BYTE*>(Label),DWORD((std::wcslen(Label)+1)*sizeof(wchar_t))))return fail("pnp_create_failed");
    SP_DEVINSTALL_PARAMS_W parameters{};parameters.cbSize=sizeof(parameters);
    if(!SetupDiGetDeviceInstallParamsW(set.get(),&device,&parameters))return fail("pnp_create_failed");
    parameters.Flags|=DI_ENUMSINGLEINF;
    if(wcscpy_s(parameters.DriverPath,inf.c_str())||!SetupDiSetDeviceInstallParamsW(set.get(),&device,&parameters))return fail("pnp_create_failed");
    if(!SetupDiBuildDriverInfoList(set.get(),&device,SPDIT_COMPATDRIVER))return fail("pnp_driver_unavailable");
    SP_DRVINFO_DATA_W driver{};driver.cbSize=sizeof(driver);
    if(!SetupDiEnumDriverInfoW(set.get(),&device,SPDIT_COMPATDRIVER,0,&driver))return fail("pnp_driver_unavailable");
    if(!SetupDiCallClassInstaller(DIF_REGISTERDEVICE,set.get(),&device))return fail("pnp_register_failed");
    registered=true;
    {wchar_t created[512]{};DWORD length=0;const DWORD one=1;
     if(SetupDiGetDeviceInstanceIdW(set.get(),&device,created,512,&length)&&validInstance(created)){registeredInstance=created;RegSetKeyValueW(HKEY_LOCAL_MACHINE,OwnedNodesKey,created,REG_DWORD,&one,sizeof(one));}}
    BOOL reboot=FALSE;
    if(!DiInstallDevice(nullptr,set.get(),&device,&driver,0,&reboot))return fail("pnp_install_failed");
    wchar_t id[512]{};
    if(!SetupDiGetDeviceInstanceIdW(set.get(),&device,id,512,&needed))return fail("pnp_install_failed");
    instance=id;restart=reboot!=FALSE;return {};
  }
  std::string label(const std::wstring& id)override{
    if(!validInstance(id))return "pnp_instance_invalid";
    Set set(SetupDiCreateDeviceInfoList(&displayClass(),nullptr));if(!set.valid())return "pnp_unavailable";
    SP_DEVINFO_DATA device{};if(!owned(set.get(),id,device))return "pnp_device_unowned";
    if(!SetupDiSetDeviceRegistryPropertyW(set.get(),&device,SPDRP_FRIENDLYNAME,reinterpret_cast<const BYTE*>(Label),DWORD((std::wcslen(Label)+1)*sizeof(wchar_t))))return "pnp_label_failed";
    const auto read=text(set.get(),device,SPDRP_FRIENDLYNAME,REG_SZ);
    if(!read||*read!=std::wstring(Label)+L'\0')return "pnp_label_unverified";
    return {};
  }
  bool remove(const std::wstring& id)override{
    if(!validInstance(id))return false;
    Set set(SetupDiCreateDeviceInfoList(&displayClass(),nullptr));SP_DEVINFO_DATA device{};
    return set.valid()&&owned(set.get(),id,device)&&SetupDiCallClassInstaller(DIF_REMOVE,set.get(),&device)!=FALSE;
  }
 private:
  class Set {
   public:
    explicit Set(HDEVINFO value):value_(value){}
    Set(const Set&)=delete;
    Set& operator=(const Set&)=delete;
    ~Set(){if(valid())SetupDiDestroyDeviceInfoList(value_);}
    HDEVINFO get()const{return value_;}
    bool valid()const{return value_!=INVALID_HANDLE_VALUE;}
   private:
    HDEVINFO value_;
  };
  // A REG_SZ / REG_MULTI_SZ property with its terminating null(s).
  static std::optional<std::wstring> text(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property,DWORD expectedType){
    DWORD type=0,needed=0;std::vector<BYTE> bytes(4096);
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,bytes.data(),DWORD(bytes.size()),&needed))return std::nullopt;
    if(type!=expectedType||needed<2||needed>bytes.size()||(needed&1)||bytes[needed-1]||bytes[needed-2])return std::nullopt;
    return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data()),needed/2);
  }
  static bool owned(HDEVINFO set,const std::wstring& id,SP_DEVINFO_DATA& device){
    device={};device.cbSize=sizeof(device);
    if(!SetupDiOpenDeviceInfoW(set,id.c_str(),nullptr,0,&device)||device.ClassGuid!=displayClass())return false;
    auto hardware=text(set,device,SPDRP_HARDWAREID,REG_MULTI_SZ);
    if(!hardware||hardware->size()<2||(*hardware)[hardware->size()-1]||(*hardware)[hardware->size()-2])return false;
    hardware->resize(hardware->size()-2);
    return hardware->find(L'\0')==std::wstring::npos&&payload::sameText(*hardware,HardwareId);
  }
};

// Tests never touch a device: a scripted stand-in that records calls. Modes:
// "" (a present, started node), "absent", "restart", "install-fails".
class Fake final:public Backend {
 public:
  explicit Fake(std::string mode):mode_(std::move(mode)){}
  size_t statuses=0,inspections=0,installs=0,labels=0,removes=0;
  Inspection inspect(const std::wstring& id)override{
    ++inspections;Inspection out;if(!validInstance(id)||mode_=="unknown")return out;
    out.presence=mode_=="absent"?"absent":(mode_=="foreign"?"foreign":"present");
    if(out.presence=="present"&&mode_!="inactive")out.status=status(id);
    return out;
  }
  Status status(const std::wstring& id)override{
    ++statuses;Status out;
    if(!validInstance(id)||mode_=="absent")return out;
    out.flags=0x0000000AUL|(mode_=="restart"?0x100UL:0);out.problem=0;return out;
  }
  std::string install(const std::wstring&,std::wstring& instance,bool& restart)override{
    ++installs;if(mode_=="install-fails")return "pnp_install_failed";
    instance=L"ROOT\\DISPLAY\\0007";restart=mode_=="restart";mode_=mode_=="absent"?std::string():mode_;return {};
  }
  std::string label(const std::wstring& id)override{++labels;return validInstance(id)?std::string():"pnp_instance_invalid";}
  bool remove(const std::wstring& id)override{++removes;return validInstance(id);}
 private:
  std::string mode_;
};

// The pinned original package bytes (third_party/libvirtualdisplay/upstream.json).
struct Pin {const wchar_t* name;const char* sha256;};
inline const Pin* pins(){
  static const Pin values[]={
    {L"SunshineVirtualDisplayDriver.inf",Driver165InfSha},
    {L"SunshineVirtualDisplayDriver.cat","a310627e2b34bfb2a54c2fc7a697391e410d4ad6def52ab23a0e75648cd30c22"},
    {L"SunshineVirtualDisplayDriver.dll","c4238424b5ce2786b52a7d7e4e1cb6e366f91ea972835f275654955364a0a1fd"},
    {L"SunshineVirtualDisplayDriver.cer","a42a04d241438ac8bd9107bc302e11b1232796c9536bf45d99717dee256746a1"},
    {nullptr,nullptr}};
  return values;
}

}  // namespace multitap::pnp
