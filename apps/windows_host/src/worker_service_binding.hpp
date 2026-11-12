#pragma once
#include "native_stream_launch.hpp"

namespace host {
// Session0 must not depend on a user's ability to OpenProcess(LocalSystem).
// Delegate only query + wait rights to this exact suspended, owned child, then
// communicate its target-process handle over the PID-authenticated private pipe.
// No handle inheritance, process DACL change or token privilege change is used.
struct WorkerServiceBinding {
  NativeStreamProcessIdentity service;
  DWORD workerPid=0;
  uint64_t handle=0;
  std::string nonce;
};
inline WorkerServiceBinding createWorkerServiceBinding(HANDLE child) {
  WorkerServiceBinding result;result.service=nativeStreamProcessIdentity(GetCurrentProcess());
  result.workerPid=GetProcessId(child);result.nonce=nativeBaselineDigest(utf8(randomName()));
  if(!result.service.pid||!result.service.created||!result.workerPid||result.workerPid==result.service.pid||WaitForSingleObject(child,0)!=WAIT_TIMEOUT)
    throw std::runtime_error("Invalid owned worker service binding");
  HANDLE remote=nullptr;
  winCheck(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),child,&remote,
      PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0),"Delegate owned service query handle");
  result.handle=uint64_t(reinterpret_cast<uintptr_t>(remote));return result;
}
inline std::string workerServiceBindingPacket(const WorkerServiceBinding& binding) {
  return "{\"type\":\"worker.service.bind\",\"servicePid\":"+std::to_string(binding.service.pid)+
    ",\"serviceCreated\":"+jsonQuote(nativeStreamU64(binding.service.created))+
    ",\"workerPid\":"+std::to_string(binding.workerPid)+",\"handle\":"+jsonQuote(nativeStreamU64(binding.handle))+
    ",\"nonce\":"+jsonQuote(binding.nonce)+'}';
}
inline WinHandle acceptWorkerServiceBinding(const Json& value,DWORD peer,std::string& nonce) {
  nativeStreamJournalFields(value,6);
  const NativeStreamProcessIdentity service{nativeStreamJournalU32(value.get("servicePid"),1,UINT32_MAX-1),parseNativeStreamU64(value.get("serviceCreated"))};
  const auto raw=parseNativeStreamU64(value.get("handle"));const auto correlation=value.get("nonce").string();
  if(value.get("type").string()!="worker.service.bind"||service.pid!=peer||!service.created||
      nativeStreamJournalU32(value.get("workerPid"),1,UINT32_MAX-1)!=GetCurrentProcessId()||
      !nativeRecoveryHex(correlation)||!raw||raw>UINTPTR_MAX||raw==UINTPTR_MAX)
    throw std::runtime_error("Invalid owned service binding packet");
  const HANDLE handle=reinterpret_cast<HANDLE>(uintptr_t(raw));DWORD flags=0;
  // Do not adopt/close an arbitrary local handle when validation fails.
  if(!GetHandleInformation(handle,&flags)||(flags&HANDLE_FLAG_INHERIT)||
      nativeStreamProcessIdentity(handle)!=service||WaitForSingleObject(handle,0)!=WAIT_TIMEOUT)
    throw std::runtime_error("Owned service lifetime is not verified");
  nonce=correlation;return WinHandle(handle);
}
inline std::string workerServiceBindingAck(const NativeStreamProcessIdentity& service,const std::string& nonce) {
  return "{\"type\":\"worker.service.bound\",\"servicePid\":"+std::to_string(service.pid)+
    ",\"serviceCreated\":"+jsonQuote(nativeStreamU64(service.created))+",\"workerPid\":"+
    std::to_string(GetCurrentProcessId())+",\"nonce\":"+jsonQuote(nonce)+'}';
}
inline bool workerServiceBindingAckMatches(const Json& value,const WorkerServiceBinding& binding) {
  try{nativeStreamJournalFields(value,5);return value.get("type").string()=="worker.service.bound"&&
    nativeStreamJournalU32(value.get("servicePid"),1,UINT32_MAX-1)==binding.service.pid&&
    parseNativeStreamU64(value.get("serviceCreated"))==binding.service.created&&
    nativeStreamJournalU32(value.get("workerPid"),1,UINT32_MAX-1)==binding.workerPid&&
    value.get("nonce").string()==binding.nonce;}catch(...){return false;}
}
inline WinHandle duplicateWorkerServiceQueryHandle(HANDLE source) {
  HANDLE local=nullptr;winCheck(DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&local,
    PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,0),"Retain owned service query handle");return WinHandle(local);
}
}
