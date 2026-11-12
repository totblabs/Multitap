#pragma once
#include "native_stream_profile_payload.hpp"
#include "native_display_recovery_identity.hpp"
#include "native_stream_console_identity.hpp"

namespace host {
// Geometry/identity planning only. The service MUST separately prove original
// process exit and exact SDK lease/display absence under its exclusive recovery
// reservation before applying this plan. This does not recreate owned outputs,
// authorize capture, retire a journal, or call SetDisplayConfig.
struct NativeStreamExpiredConsoleContext {
  NativeStreamPrimaryProfile profile;
  NativeRecoveryScope scope;
  NativeConsoleBinding console;
  std::string ownedInstance;
};
struct NativeStreamExpiredConsoleAccess {
  NativeConsoleIdentityResolver adapter;
  std::function<NativeForeignBinding(const DISPLAYCONFIG_PATH_INFO&)> binding;
};
struct NativeStreamExpiredConsolePlan {
  NativeCcdData before,restored;
  NativeRecoveryScope scopeBefore,scopeAfter;
  NativeConsoleBinding console;
  bool alreadyRestored=false;
};
inline bool nativeExpiredForeignBindingEqual(const NativeForeignBinding& a,const NativeForeignBinding& b){
  return a.complete&&b.complete&&nativeForeignBindingKey(a)==nativeForeignBindingKey(b)&&nativeForeignBindingValue(a)==nativeForeignBindingValue(b);
}
inline NativeStreamExpiredConsolePlan planNativeStreamExpiredConsole(const NativeStreamExpiredConsoleContext& context,const NativeCcdData& fresh,
    const NativeRecoveryScope& scope,std::string_view machine,const NativeStreamExpiredConsoleAccess& access,uint64_t now){
  const auto require=[](bool value){if(!value)throw std::runtime_error("native_stream_expired_console_unverified");};
  validateNativeStreamPrimaryProfile(context.profile);
  require(context.profile.suppressedConsole.has_value()&&nativeDisplayInstanceId(context.ownedInstance)&&bool(access.adapter)&&bool(access.binding));
  require(nativeConsoleUuid(machine)==nativeConsoleUuid(context.console.proof.machineUuid)&&canonicalNativeConsoleBinding(context.console).has_value());
  require(nativeRecoveryLogonMatches(context.scope,scope)&&nativeRecoveryScopeComplete(context.scope)&&nativeRecoveryScopeComplete(scope)&&scope.tick&&scope.tick<=now&&now-scope.tick<=3000);
  require(fresh.paths.size()<=nativeCcdQueryPathLimit&&fresh.modes.size()<=nativeCcdQueryModeLimit);
  const auto& key=*context.profile.suppressedConsole;
  require(nativeSameLuid(key.sourceAdapter,context.console.path.sourceAdapter)&&nativeSameLuid(key.targetAdapter,context.console.path.targetAdapter)&&key.source==context.console.path.source&&key.target==context.console.path.target);
  validateNativeRecoveryScope(context.scope,captureNativeTopologyBaseline(context.profile.before,context.profile.targets,"0"));
  const auto original=std::find_if(context.profile.before.paths.begin(),context.profile.before.paths.end(),[&](const auto& p){return nativeStreamConsolePathMatches(p,key);});
  require(original!=context.profile.before.paths.end());
  const auto& source=nativeCcdSource(context.profile.before,*original);
  // First supported fallback: this exact console was the original primary.
  // Do not invent a replacement for a vanished original foreign/owned primary.
  require(source.position.x==0&&source.position.y==0);
  const auto consoleBinding=std::find_if(context.scope.foreign.begin(),context.scope.foreign.end(),[&](const auto& b){return nativeSameLuid(b.sourceAdapter,key.sourceAdapter)&&nativeSameLuid(b.targetAdapter,key.targetAdapter)&&b.source==key.source&&b.target==key.target;});
  require(consoleBinding!=context.scope.foreign.end()&&consoleBinding->complete);
  const auto hardware=access.adapter(key.targetAdapter);require(hardware.has_value());
  auto currentConsole=context.console;currentConsole.device=*hardware;
  require(sameNativeConsoleBinding(currentConsole,context.console));
  const auto selected=nativeConsoleAscii(wide(context.ownedInstance),200);require(selected.has_value());
  const auto active=nativeActiveCcd(fresh);
  NativeTopologyBaseline activeBaseline;activeBaseline.ccd=active;validateNativeRecoveryScope(scope,activeBaseline);
  require(active.paths.size()<=64&&active.modes.size()<=256);
  std::set<decltype(nativeStreamProfileTargetKey(DISPLAYCONFIG_PATH_INFO{}))> targets;
  for(const auto& path:active.paths){
    require(path.targetInfo.targetAvailable&&targets.insert(nativeStreamProfileTargetKey(path)).second&&!nativeOwnedPath(path,context.profile.targets));
    (void)nativeCcdSource(active,path);(void)nativeCcdTarget(active,path);
    for(const auto luid:{path.sourceInfo.adapterId,path.targetInfo.adapterId}){
      const auto device=access.adapter(luid);require(device.has_value());const auto canonical=canonicalNativeConsoleAdapter(*device);
      require(canonical.has_value()&&canonical->instanceId!=*selected);
    }
  }
  (void)nativeStreamProfileSignatures(active);
  // QDC_ALL_PATHS may contain many possible combinations. Exactly this saved
  // available source/target must exist once, with a freshly matching monitor
  // binding even while inactive; display number/name is never evidence.
  const DISPLAYCONFIG_PATH_INFO* available=nullptr;size_t count=0;
  for(const auto& path:fresh.paths)if(nativeStreamConsolePathMatches(path,key)){available=&path;++count;}
  require(count==1&&available&&available->targetInfo.targetAvailable&&nativeExpiredForeignBindingEqual(*consoleBinding,access.binding(*available)));
  auto scopeAfter=context.scope;scopeAfter.tick=scope.tick;
  const auto oldForeign=nativeStreamProfileSignatures(context.profile.before,context.profile.targets);
  if(available->flags&DISPLAYCONFIG_PATH_ACTIVE){
    require(nativeRecoveryScopeMatches(context.scope,scope)&&nativeStreamProfileSignatures(active)==oldForeign);
    return {active,active,scope,scopeAfter,currentConsole,true};
  }
  auto expectedScope=context.scope;
  expectedScope.foreign.erase(std::remove_if(expectedScope.foreign.begin(),expectedScope.foreign.end(),[&](const auto& b){return nativeForeignBindingKey(b)==nativeForeignBindingKey(*consoleBinding);}),expectedScope.foreign.end());
  require(nativeRecoveryScopeMatches(expectedScope,scope)&&nativeStreamProfileSignatures(active)==nativeStreamProfileSignatures(context.profile.applied,context.profile.targets));
  for(const auto& path:active.paths)require(nativeSourceKey(path)!=nativeSourceKey(*original)&&nativeStreamProfileTargetKey(path)!=nativeStreamProfileTargetKey(*original));
  const auto zero=std::find_if(context.profile.before.paths.begin(),context.profile.before.paths.end(),[&](const auto& p){return nativeSameTarget(p,context.profile.targets.front());});
  require(zero!=context.profile.before.paths.end());const auto& anchor=nativeCcdSource(context.profile.before,*zero);
  const auto translated=translateNativeStreamProfile(active,anchor.position.x,anchor.position.y);
  const auto restored=nativeStreamProfileWithConsole(context.profile,fresh,translated);
  require(nativePathsAvailable(restored,fresh)&&nativeStreamProfileSignatures(restored)==oldForeign);
  NativeTopologyBaseline restoredBaseline;restoredBaseline.ccd=restored;validateNativeRecoveryScope(scopeAfter,restoredBaseline);
  return {active,restored,scope,scopeAfter,currentConsole,false};
}
} // namespace host
