#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
namespace multitap {
inline uint64_t AvailableVideoSystemMemory(){
  MEMORYSTATUSEX state{};state.dwLength=sizeof(state);
  if(!GlobalMemoryStatusEx(&state))return 0;
  return std::min<uint64_t>(state.ullAvailPhys,state.ullAvailPageFile);
}
inline bool VideoAllocationFits(uint64_t bytes){const auto available=AvailableVideoSystemMemory();return available&&bytes<=available;}
}
