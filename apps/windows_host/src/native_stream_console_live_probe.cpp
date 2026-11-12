#include "native_stream_console_identity.hpp"
#include <iostream>

using namespace host;
namespace {
constexpr GUID displayInterface={0x5b45201d,0xf2f2,0x4f3b,{0x85,0xbb,0x30,0xff,0x1f,0x95,0x35,0x99}};
void probe(LUID adapter,const GUID& kind,DWORD flags,const char* label){
  DISPLAYCONFIG_ADAPTER_NAME name{};name.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;name.header.size=sizeof(name);name.header.adapterId=adapter;
  const auto ccd=DisplayConfigGetDeviceInfo(&name.header);
  std::cout<<"{\"set\":\""<<label<<"\",\"ccdError\":"<<ccd;
  if(ccd!=ERROR_SUCCESS){std::cout<<"}\n";return;}
  const auto set=SetupDiGetClassDevsW(&kind,nullptr,nullptr,flags);
  if(set==INVALID_HANDLE_VALUE){std::cout<<",\"setError\":"<<GetLastError()<<"}\n";return;}
  SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);SetLastError(0);
  const bool opened=SetupDiOpenDeviceInterfaceW(set,name.adapterDevicePath,DIODI_NO_ADD,&iface)!=FALSE;
  const auto openError=GetLastError();DWORD required=0;
  std::cout<<",\"opened\":"<<opened<<",\"openError\":"<<openError<<",\"active\":"<<bool(iface.Flags&SPINT_ACTIVE);
  if(opened){
    SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&required,nullptr);
    if(required>=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)&&required<4096){
      std::vector<BYTE> buffer(required);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());detail->cbSize=sizeof(*detail);
      SP_DEVINFO_DATA info{};info.cbSize=sizeof(info);
      const bool detailed=SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,required,nullptr,&info)!=FALSE;
      std::cout<<",\"detail\":"<<detailed<<",\"displayClass\":"<<bool(detailed&&nativeConsoleGuidEqual(info.ClassGuid,nativeConsoleDisplayClass));
      if(detailed){
        wchar_t instance[201]{};DWORD length=0;
        const bool instanceOk=SetupDiGetDeviceInstanceIdW(set,&info,instance,DWORD(std::size(instance)),&length)!=FALSE;
        DWORD type=0,bytes=0;SetupDiGetDeviceRegistryPropertyW(set,&info,SPDRP_HARDWAREID,&type,nullptr,0,&bytes);
        const auto queryError=GetLastError();
        std::cout<<",\"instanceOk\":"<<instanceOk<<",\"instanceLength\":"<<length<<",\"hardwareQueryError\":"<<queryError<<",\"hardwareType\":"<<type<<",\"hardwareBytes\":"<<bytes;
        if(bytes>=4&&bytes<=8192&&bytes%2==0){
          std::vector<wchar_t> value(bytes/2);DWORD actual=0;
          const bool read=SetupDiGetDeviceRegistryPropertyW(set,&info,SPDRP_HARDWAREID,&type,reinterpret_cast<PBYTE>(value.data()),bytes,&actual)!=FALSE;
          size_t at=0,count=0;while(at<value.size()&&value[at]){const auto end=std::find(value.begin()+at,value.end(),L'\0');if(end==value.end())break;at=size_t(end-value.begin())+1;++count;}
          std::cout<<",\"hardwareRead\":"<<read<<",\"hardwareActual\":"<<actual<<",\"hardwareCount\":"<<count<<",\"hardwareEndExact\":"<<bool(at+1==value.size());
        }
      }
    }
  }
  SetupDiDestroyDeviceInfoList(set);std::cout<<"}\n";
}
}
int main(){
  std::cout<<std::boolalpha;
  UINT32 pathCount=0,modeCount=0;LONG error=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount);
  if(error||pathCount>4096||modeCount>16384){std::cout<<"{\"queryError\":"<<error<<"}\n";return 1;}
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  error=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths.data(),&modeCount,modes.data(),nullptr);
  if(error){std::cout<<"{\"queryError\":"<<error<<"}\n";return 1;}
  paths.resize(pathCount);std::vector<LUID> adapters;
  for(const auto& path:paths)if(std::none_of(adapters.begin(),adapters.end(),[&](auto v){return nativeConsoleLuidEqual(v,path.targetInfo.adapterId);}))adapters.push_back(path.targetInfo.adapterId);
  std::cout<<"{\"kind\":\"read-only-console-resolver-live-probe\",\"activePaths\":"<<pathCount<<",\"adapters\":"<<adapters.size()<<",\"displayMutations\":0,\"identifiersPrinted\":false}\n";
  for(const auto adapter:adapters){
    std::cout<<"{\"productionResolver\":"<<bool(nativeConsoleAdapterIdentity(adapter))<<"}\n";
    probe(adapter,nativeConsoleDisplayClass,DIGCF_PRESENT,"display-setup-class");
    probe(adapter,displayInterface,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE,"display-interface-class");
  }
}
