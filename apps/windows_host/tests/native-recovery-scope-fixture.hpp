#pragma once
#include "../src/native_display_recovery_identity.hpp"
namespace host::test {
inline NativeRecoveryScope currentScope(){return {17,std::string(64,'a'),1001,{}};}
inline NativeRecoveryScope recoveryScope(const NativeTopologyBaseline& baseline){
  return captureNativeRecoveryScope(baseline,[]{return NativeRecoveryScope{17,std::string(64,'a'),1000,{}};},[](const DISPLAYCONFIG_PATH_INFO& path){
    return NativeForeignBinding{path.sourceInfo.adapterId,path.targetInfo.adapterId,path.sourceInfo.id,path.targetInfo.id,3,"fixture-source-"+std::to_string(path.sourceInfo.adapterId.LowPart),"fixture-target-"+std::to_string(path.targetInfo.adapterId.LowPart),"fixture-monitor-"+std::to_string(path.targetInfo.id),true};
  });
}
}
