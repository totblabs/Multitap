#pragma once
#include "pending_connection.hpp"

namespace host {
// This service/RPC gate is deliberately distinct from the helper's viewport
// mutex: the child helper must be able to take its own transaction lock.
inline ConnectionGuardPtr displayMaintenanceLock() {
  return nativeConnectionLock(L"Global\\MultitapHost.DisplayMaintenance",0);
}
struct DisplaySetupAccess {
  std::function<ConnectionGuardPtr()> lock;
  std::function<DisplayHelperOutput()> probe;
  std::function<void()> configure, reload;
};
inline void configureIdleDisplay(const DisplaySetupAccess& access) {
  const auto gate=access.lock();
  if(!gate)throw std::runtime_error("viewport_busy");
  const auto probe=access.probe();const auto& value=probe.value;
  if(probe.timedOut||probe.exitCode||!std::holds_alternative<Json::Object>(value.value)||
     !value.get("readOnly").boolean()||value.get("modeCatalogRevision").number()!=1||
     value.get("requiredModeCount").number()!=18)
    throw std::runtime_error("virtual_display_status_failed");
  if(value.get("recoveryRequired").boolean()||value.get("state").get("recoveryRequired").boolean())
    throw std::runtime_error("viewport_recovery_required");
  const auto reason=value.get("reason").string();
  if(reason=="viewport_busy")throw std::runtime_error("viewport_busy");
  if(!value.get("error").string().empty()||
     (reason!=""&&reason!="mode_catalog_update_required"&&reason!="reboot_required"&&
      reason!="interactive_console_required"&&reason!="owned_virtual_output_inactive"&&
      reason!="owned_virtual_display_not_ready"))
    throw std::runtime_error("virtual_display_status_failed");
  // Keep the gate through reload: a new automatic maintenance job may not
  // enter between the read-only proof, configuration, and service refresh.
  access.configure();
  access.reload();
}
}
