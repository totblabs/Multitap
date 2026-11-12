#pragma once
#include <virtual_display/driver/windows_control_client.h>
#include <algorithm>
#include <memory>
#include <string>
#include <cfgmgr32.h>
namespace multitap::vdisplay {
// SDK DEVPKEY_Device_InstanceId, scoped locally to avoid INITGUID pollution in
// translation units that already instantiate the Windows property keys.
inline constexpr DEVPROPKEY ownedInstanceIdProperty{{0x78c34fc8,0x104a,0x4aca,
  {0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57}},256};
// The path must come from an administrator-protected Multitap installation
// marker. Never pick the first interface or claim another product's device.
inline std::unique_ptr<virtual_display::driver::WindowsControlTransport> openOwnedEndpoint(const std::wstring& approvedPath) {
  namespace api=virtual_display::driver;
  if(approvedPath.empty()||approvedPath.size()>4096)return {};
  uint32_t error=0;const auto devices=api::enumerate_control_devices(&error);
  if(error||std::count_if(devices.begin(),devices.end(),[&](const auto& device){return device.device_path==approvedPath&&device.openable;})!=1)return {};
  const HANDLE handle=CreateFileW(approvedPath.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
  if(handle==INVALID_HANDLE_VALUE)return {};
  return std::make_unique<api::WindowsControlTransport>(handle);
}
inline std::unique_ptr<virtual_display::driver::WindowsControlTransport> openOwnedInstance(const std::wstring& approvedInstance){
  namespace api=virtual_display::driver;
  if(approvedInstance.empty()||approvedInstance.size()>512)return {};
  uint32_t error=0;const auto devices=api::enumerate_control_devices(&error);if(error)return {};
  std::wstring selected;size_t matches=0;
  for(const auto& device:devices){
    wchar_t instance[512]{};DEVPROPTYPE type=0;ULONG bytes=sizeof(instance);
    if(CM_Get_Device_Interface_PropertyW(device.device_path.c_str(),&ownedInstanceIdProperty,&type,reinterpret_cast<PBYTE>(instance),&bytes,0)!=CR_SUCCESS||type!=DEVPROP_TYPE_STRING||bytes<2||bytes>sizeof(instance))continue;
    instance[511]=0;if(_wcsicmp(instance,approvedInstance.c_str()))continue;
    if(!device.openable)return {};selected=device.device_path;++matches;
  }
  return matches==1?openOwnedEndpoint(selected):nullptr;
}
}
