#pragma once
#include "display_operation.hpp"
#include "native_stream_console_identity.hpp"

namespace host {
inline constexpr const char* qemuConsoleProtocol="qemu-console-v2";
inline constexpr uint64_t qemuConsoleHoldMillis=30000;
// The operation's existing nonce/Host/lease binding is checked by the service.
// Worker exit and the lifetime of this display hold are separate facts.
class QemuConsoleHold {
 public:
  void begin(uint64_t now){held_=true;sequence_=0;deadline_=now+qemuConsoleHoldMillis;}
  bool held()const{return held_;}
  uint64_t deadline()const{return deadline_;}
  uint32_t sequence()const{return sequence_;}
  bool expire(uint64_t now){if(!held_||now<deadline_)return false;held_=false;return true;}
  void release(){held_=false;}
  bool renew(uint32_t sequence,uint64_t now){
    if(!held_||now>=deadline_||sequence_==UINT32_MAX||sequence!=sequence_+1)return false;
    sequence_=sequence;deadline_=now+qemuConsoleHoldMillis;return true;
  }
 private:bool held_=false;uint32_t sequence_=0;uint64_t deadline_=0;
};
inline bool qemuConsoleHoldIdentityCurrent(bool sameConsoleSession,bool sameMachine,bool sameLease,bool configurationPending){
  // Renewal only suppresses idle display mutation. Windows locking does not
  // change this authority or require an interactive worker on Winlogon.
  return sameConsoleSession&&sameMachine&&sameLease&&!configurationPending;
}
inline bool qemuConsoleActivationRearmAllowed(bool successfulHold,bool workerQuiet,bool sameLease,bool sameSession,bool probeRunning){
  return successfulHold&&workerQuiet&&sameLease&&sameSession&&!probeRunning;
}
inline NativeConsoleModelProof qemuConsoleProof(const Json& value,bool worker=false){
  const auto* fields=std::get_if<Json::Object>(&value.value);
  if(!fields||fields->size()!=(worker?8u:6u)||value.get("protocol").string()!=qemuConsoleProtocol||
     value.get("type").string()!=(worker?"worker.qemu-console.prepare":"host.qemu-console.prepare"))
    throw std::runtime_error("Invalid QEMU console request");
  (void)displayOperationKey(value);
  const auto uuid=nativeConsoleUuid(value.get("machineUuid").string());
  if(!uuid||value.get("model").string()!="std")throw std::runtime_error("Invalid QEMU console proof");
  return {*uuid,"std"};
}
inline std::string safeQemuConsoleReason(const std::string& reason){
  for(const auto* value:{"qemu_console_active","qemu_console_restored","qemu_console_inactive","qemu_console_state_missing","qemu_console_state_invalid","qemu_console_identity_changed","qemu_console_foreign_changed","qemu_console_unavailable","qemu_console_cancelled","qemu_console_busy","qemu_console_timeout"})
    if(reason==value)return reason;
  return "qemu_console_unavailable";
}
inline std::string qemuConsoleResultPacket(const DisplayOperationKey& key,bool ok,bool attempted,bool quiet,const std::string& reason,const char* type="host.qemu-console.result"){
  return "{\"type\":"+jsonQuote(type)+",\"protocol\":"+jsonQuote(qemuConsoleProtocol)+','+displayOperationFields(key)+
    ",\"ok\":"+(ok?"true":"false")+",\"attempted\":"+(attempted?"true":"false")+",\"workerQuiescent\":"+(quiet?"true":"false")+",\"reason\":"+jsonQuote(safeQemuConsoleReason(reason))+'}';
}
inline uint32_t qemuConsoleRenewSequence(const Json& value){
  const auto* fields=std::get_if<Json::Object>(&value.value);const auto sequence=value.get("sequence").number();
  if(!fields||fields->size()!=5||value.get("type").string()!="host.qemu-console.renew"||value.get("protocol").string()!=qemuConsoleProtocol||
      !nativeDisplayInteger(sequence,1,UINT32_MAX))throw std::runtime_error("Invalid QEMU console renewal");
  (void)displayOperationKey(value);return uint32_t(sequence);
}
inline std::string qemuConsoleHeldPacket(const DisplayOperationKey& key,uint32_t sequence){
  return "{\"type\":\"host.qemu-console.held\",\"protocol\":"+jsonQuote(qemuConsoleProtocol)+','+displayOperationFields(key)+",\"sequence\":"+std::to_string(sequence)+",\"leaseMs\":30000}";
}
}
