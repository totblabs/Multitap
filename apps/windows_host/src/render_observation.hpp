#pragma once
// Which adapter actually renders, from the Windows "GPU Engine" performance
// counters. A render request (SetRenderAdapter) or an encoder that opened proves
// nothing about this. Read-only: no device, driver or display call.
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wtsapi32.h>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>
#pragma comment(lib,"pdh.lib")
#pragma comment(lib,"wtsapi32.lib")

namespace host {
struct GpuEngineSample {uint32_t pid=0;uint64_t luid=0;uint32_t phys=0,engine=0;uint64_t running=0;};
// "pid_1824_luid_0x00000000_0x00008633_phys_0_eng_0_engtype_3D"; only 3D engines
// are render work. The LUID is printed high part first.
inline std::optional<GpuEngineSample> parseGpuEngineInstance(const wchar_t* name,uint64_t running){
  if(!name)return {};
  unsigned pid=0,high=0,low=0,phys=0,engine=0;int used=0;
  if(swscanf_s(name,L"pid_%u_luid_0x%x_0x%x_phys_%u_eng_%u_engtype_%n",&pid,&high,&low,&phys,&engine,&used)!=5||used<=0||_wcsicmp(name+used,L"3D")!=0)return {};
  return GpuEngineSample{pid,(uint64_t(high)<<32)|low,phys,engine,running};
}
struct RenderReading {std::optional<uint64_t> compositor,busiest;};
// The adapters that did the most 3D work between two samples: for the compositor
// process, and over every process (a running game shows up here). An engine with
// no earlier sample has no baseline and does not count.
inline RenderReading readRenderAdapters(std::span<const GpuEngineSample> before,std::span<const GpuEngineSample> after,uint32_t compositor){
  using Key=std::tuple<uint32_t,uint64_t,uint32_t,uint32_t>;
  std::map<Key,uint64_t> earlier;for(const auto& sample:before)earlier[{sample.pid,sample.luid,sample.phys,sample.engine}]=sample.running;
  std::map<uint64_t,uint64_t> own,all;
  for(const auto& sample:after){
    const auto found=earlier.find({sample.pid,sample.luid,sample.phys,sample.engine});
    if(found==earlier.end()||sample.running<=found->second)continue;
    const auto delta=sample.running-found->second;all[sample.luid]+=delta;if(sample.pid==compositor)own[sample.luid]+=delta;
  }
  const auto top=[](const std::map<uint64_t,uint64_t>& totals)->std::optional<uint64_t>{
    std::optional<uint64_t> best;uint64_t most=0;for(const auto& [luid,time]:totals)if(time>most){most=time;best=luid;}return best;
  };
  return {top(own),top(all)};
}
// Samples at most every [intervalMs] while a stream runs; a reading comes back only when it changed.
// Only stream time counts: without a stream only the Proxmox console's display is on, which the
// basic render adapter drives, so every idle reading and every first reading of a stream (its interval began
// before the stream) said "compositor on Microsoft Basic Render" (hostAction 211 ok:false) on a VM whose stream
// captured on its GPU. A stream now takes its own baseline when it starts and reads from there.
class RenderObservation {
 public:
  using Sampler=std::function<std::vector<GpuEngineSample>()>;
  RenderObservation(Sampler sampler,std::function<uint32_t()> compositor,uint64_t intervalMs=30000):sampler_(std::move(sampler)),compositor_(std::move(compositor)),interval_(intervalMs){}
  // A new stream records its own pairs from its own baseline: the display it runs on can move the
  // compositor, and work done before it says nothing about this stream.
  void streaming(bool on){if(on&&!streaming_){seen_.clear();reported_={};rebase_=true;}if(!on){last_.clear();rebase_=false;}streaming_=on;}
  std::optional<RenderReading> tick(uint64_t now){
    if(!streaming_)return {};
    if(rebase_){rebase_=false;last_=sampler_();next_=now+interval_;return {};}
    if(now<next_)return {};
    next_=now+interval_;
    auto samples=sampler_();
    auto reading=readRenderAdapters(last_,samples,compositor_());
    last_=std::move(samples);
    // An idle interval has no reading; it says nothing about the adapter in use.
    if(!reading.compositor)reading.compositor=reported_.compositor;
    if(!reading.busiest)reading.busiest=reported_.busiest;
    reported_=reading;
    // Each pair once: two adapters taking turns must not fill the event log.
    if((!reading.compositor&&!reading.busiest)||!seen_.insert({reading.compositor,reading.busiest}).second)return {};
    return reading;
  }
 private:
  Sampler sampler_;std::function<uint32_t()> compositor_;uint64_t interval_=0,next_=0;bool streaming_=false,rebase_=false;
  std::vector<GpuEngineSample> last_;RenderReading reported_;
  std::set<std::pair<std::optional<uint64_t>,std::optional<uint64_t>>> seen_;
};
inline std::vector<GpuEngineSample> queryGpuEngineSamples(){
  PDH_HQUERY query=nullptr;if(PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS)return {};
  struct Close{PDH_HQUERY value;~Close(){PdhCloseQuery(value);}}close{query};
  PDH_HCOUNTER counter=nullptr;
  if(PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Running Time",0,&counter)!=ERROR_SUCCESS||PdhCollectQueryData(query)!=ERROR_SUCCESS)return {};
  DWORD bytes=0,count=0;
  if(PdhGetRawCounterArrayW(counter,&bytes,&count,nullptr)!=LONG(PDH_MORE_DATA)||!bytes)return {};
  std::vector<BYTE> buffer(bytes);auto* items=reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(buffer.data());
  if(PdhGetRawCounterArrayW(counter,&bytes,&count,items)!=ERROR_SUCCESS)return {};
  std::vector<GpuEngineSample> samples;samples.reserve(count);
  for(DWORD i=0;i<count;++i)if(const auto sample=parseGpuEngineInstance(items[i].szName,uint64_t(items[i].RawValue.FirstValue)))samples.push_back(*sample);
  return samples;
}
// The console session's dwm.exe; 0 when there is none. The session list needs no
// handle to the process, which an unelevated caller could not open.
inline uint32_t consoleCompositorPid(){
  const DWORD console=WTSGetActiveConsoleSessionId();
  WTS_PROCESS_INFOW* list=nullptr;DWORD count=0;
  if(!WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE,0,1,&list,&count))return 0;
  struct Free{WTS_PROCESS_INFOW* value;~Free(){WTSFreeMemory(value);}}release{list};
  for(DWORD i=0;i<count;++i)if(list[i].SessionId==console&&list[i].pProcessName&&_wcsicmp(list[i].pProcessName,L"dwm.exe")==0)return list[i].ProcessId;
  return 0;
}
} // namespace host
