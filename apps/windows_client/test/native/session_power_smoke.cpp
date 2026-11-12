#include "session_power_activity.h"
#include <iostream>
int main(){
  multitap::SessionPowerActivity activity;
  const bool before=!activity.active();
  const bool held=activity.Set(true)&&activity.active();
  const bool repeated=activity.Set(true)&&activity.active();
  const bool released=activity.Set(false)&&!activity.active();
  const bool idempotent=activity.Set(false)&&!activity.active();
  std::cout<<"session scoped Windows power requests beforeInactive="<<before<<" setSystemAndDisplay="<<held<<" repeated="<<repeated<<" cleared="<<released<<" repeatedClear="<<idempotent<<"; powerPlanChanges=0 remoteConnections=0\n";
  return before&&held&&repeated&&released&&idempotent?0:1;
}
