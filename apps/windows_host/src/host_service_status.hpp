#pragma once
#include "service.hpp"
#include "service_configuration_change.hpp"
// The installed MultitapHost service as another process sees it: its state for the tray, Host settings
// and --diagnostics, and the two control codes that ask it to reload its configuration or to publish
// its capabilities again.
namespace host {
inline void reloadHostService() { requireAdmin(); ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)); ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_USER_DEFINED_CONTROL)); SERVICE_STATUS status{}; winCheck(ControlService(service.value,hostServiceReloadControl,&status),"Reload host service"); }
inline bool requestHostCapabilitiesRefresh() noexcept {
  try { requireAdmin(); ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)); ServiceHandle service(OpenServiceW(manager.value,L"MultitapHost",SERVICE_USER_DEFINED_CONTROL)); SERVICE_STATUS status{}; return ControlService(service.value,hostServiceCapabilitiesControl,&status)!=FALSE; }
  catch (...) { return false; }
}
struct HostServiceSnapshot {
  bool presenceKnown=false, runningKnown=false, installed=false, running=false;
  DWORD state=0, error=0;
  const char* key()const {
    if(!presenceKnown||!runningKnown)return "unknown";
    if(!installed)return "not-installed";
    switch(state){case SERVICE_RUNNING:return "running";case SERVICE_STOPPED:return "stopped";case SERVICE_START_PENDING:return "starting";case SERVICE_STOP_PENDING:return "stopping";case SERVICE_PAUSED:return "paused";case SERVICE_PAUSE_PENDING:return "pausing";case SERVICE_CONTINUE_PENDING:return "continuing";default:return "unknown";}
  }
};
inline HostServiceSnapshot readHostServiceSnapshot(const wchar_t* name=L"MultitapHost") {
  HostServiceSnapshot value;const auto managerHandle=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
  if(!managerHandle){value.error=GetLastError();return value;}ServiceHandle manager(managerHandle);
  const auto serviceHandle=OpenServiceW(manager.value,name,SERVICE_QUERY_STATUS);
  if(!serviceHandle){value.error=GetLastError();if(value.error==ERROR_SERVICE_DOES_NOT_EXIST){value.presenceKnown=value.runningKnown=true;value.error=0;}return value;}ServiceHandle service(serviceHandle);
  value.presenceKnown=value.installed=true;SERVICE_STATUS_PROCESS status{};DWORD bytes=0;
  if(!QueryServiceStatusEx(service.value,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&bytes)){value.error=GetLastError();return value;}
  value.runningKnown=true;value.state=status.dwCurrentState;value.running=status.dwCurrentState==SERVICE_RUNNING;return value;
}
// The service state in words: the tray shows it in the person's language;
// --diagnostics keeps the Korean words it always printed (Language::korean).
inline std::wstring serviceLabel(const HostServiceSnapshot& value,ui_text::Language language) {
  using ui_text::Id;const auto say=[language](Id id){return std::wstring(ui_text::get(id,language));};
  if(!value.presenceKnown||!value.runningKnown)return say(Id::ServiceUnknown);
  if(!value.installed)return say(Id::ServiceNotInstalled);
  switch(value.state){case SERVICE_RUNNING:return say(Id::ServiceRunning);case SERVICE_STOPPED:return say(Id::ServiceStopped);case SERVICE_START_PENDING:return say(Id::ServiceStarting);case SERVICE_STOP_PENDING:return say(Id::ServiceStopping);case SERVICE_PAUSED:return say(Id::ServicePaused);case SERVICE_PAUSE_PENDING:case SERVICE_CONTINUE_PENDING:return say(Id::ServiceChanging);default:return say(Id::ServiceUnknown);}
}
inline std::wstring serviceLabel(const HostServiceSnapshot& value){return serviceLabel(value,multitap::ui::processLanguage());}
inline std::wstring serviceLabel(){return serviceLabel(readHostServiceSnapshot());}
}
