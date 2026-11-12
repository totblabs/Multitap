#pragma once
#include "operational_log.hpp"
#include <atomic>

namespace host {
// At most one stream per Host; pending final/30s windows remain bounded even
// when diagnostic collection or disk writes stall. No producer performs I/O.
class HostObservationLogBuffer {
 public:
  void observe(const Json& stats,uint64_t nowMs) {
    const auto type=stats.get("type").string();const bool tracks=type=="stream.tracks.stats";
    if((!tracks&&type!="stream.stats")||!hostObservationSession(stats.get("sessionId").string())||stats.get("observability").get("protocol").string()!=streamObservabilityProtocol)return;
    const auto* fields=std::get_if<Json::Object>(&stats.get("observability").get("metrics").value);if(!fields)return;
    Json::Object observed;
    for(const auto& [key,item]:*fields){const auto maximum=hostObservationMaximum(key);const auto* value=std::get_if<double>(&item.value);
      if(maximum&&value&&std::isfinite(*value)&&*value>=0&&*value<=*maximum&&std::floor(*value)==*value)observed.emplace(key,item);}
    if(observed.empty())return;
    const auto epoch=stats.get(tracks?"topologyEpoch":"epoch").number(-1);if(!std::isfinite(epoch)||epoch<0||epoch>(tracks?4294967295.0:65535.0)||std::floor(epoch)!=epoch)return;
    const auto key=stats.get("sessionId").string()+':' +(tracks?"t":"s")+':'+std::to_string(uint64_t(epoch));
    std::lock_guard lock(mutex_);
    const auto session=stats.get("sessionId").string();
    if(std::find(retired_.begin(),retired_.end(),session)!=retired_.end())return;
    if(active_==session&&activeTracks_==tracks&&uint64_t(epoch)<activeEpoch_)return;
    if(!active_.empty()&&active_!=session)retire(active_);
    active_=session;activeTracks_=tracks;activeEpoch_=uint64_t(epoch);
    if(current_&&(current_->key!=key||nowMs<current_->began)){stage(true,nowMs);current_.reset();}
    if(!current_)current_=Window{key,stats.get("sessionId").string(),tracks,uint64_t(epoch),nowMs,0,{}};
    current_->latest=std::move(observed);current_->samples=std::min(4096u,current_->samples+1);
    if(nowMs-current_->began>=30000){stage(false,nowMs);current_.reset();}
  }
  void finish(const std::string& session,uint64_t nowMs) {
    if(!hostObservationSession(session))return;
    std::lock_guard lock(mutex_);if(current_&&current_->session==session){stage(true,nowMs);current_.reset();}
    retire(session);if(active_==session)active_.clear();
  }
  bool pending()const{return pending_.load()!=0;}
  template<class Sink> void drain(Sink sink) {
    std::deque<std::string> batch;
    {std::lock_guard lock(mutex_);for(unsigned n=0;n<4&&!records_.empty();++n){batch.push_back(std::move(records_.front()));records_.pop_front();}pending_=records_.size();}
    for(const auto& row:batch)try{sink(row);}catch(...){ }
  }
  uint64_t dropped()const{std::lock_guard lock(mutex_);return dropped_;}
 private:
  struct Window{std::string key,session;bool tracks=false;uint64_t epoch=0,began=0;unsigned samples=0;Json::Object latest;};
  mutable std::mutex mutex_;std::optional<Window> current_;std::deque<std::string> records_,retired_;std::atomic<size_t> pending_{0};uint64_t dropped_=0;
  std::string active_;bool activeTracks_=false;uint64_t activeEpoch_=0;
  void retire(const std::string& session){if(std::find(retired_.begin(),retired_.end(),session)==retired_.end()){if(retired_.size()>=16)retired_.pop_front();retired_.push_back(session);}}
  void stage(bool final,uint64_t nowMs) {
    if(!current_||!current_->samples)return;
    for(const auto* source:{"host-service","host-worker"}){
      Json row;Json::Object fields;const auto number=[&](const char* key,uint64_t value){Json item;item.value=double(value);fields[key]=std::move(item);};
      const auto text=[&](const char* key,const std::string& value){Json item;item.value=value;fields[key]=std::move(item);};
      text("event","streamObservation");text("aggregation","latest-snapshot");text("time",hostLogTime());text("source",source);text("sessionId",current_->session);
      number(current_->tracks?"topologyEpoch":"epoch",current_->epoch);number("samples",current_->samples);
      number("windowMs",nowMs>=current_->began?std::min<uint64_t>(4294967295,nowMs-current_->began):0);number("droppedRecords",std::min<uint64_t>(4294967295,dropped_));
      Json ended;ended.value=final;fields["final"]=std::move(ended);Json values;values.value=current_->latest;fields["values"]=std::move(values);row.value=std::move(fields);
      const auto record=safeHostObservationLogRecord(row);if(record.empty())continue;
      if(records_.size()>=16){records_.pop_front();++dropped_;}records_.push_back(record);
    }
    pending_=records_.size();
  }
};
inline HostObservationLogBuffer& hostObservationLogs(){static HostObservationLogBuffer buffer;return buffer;}
inline void queueHostObservationLog(const std::string& stats)noexcept {
  if(!operationalLogEnabled)return;
  try{hostObservationLogs().observe(JsonParser::parse(stats),GetTickCount64());}catch(...){ }
}
inline void finishHostObservationLog(const std::string& session)noexcept {
  if(!operationalLogEnabled)return;
  try{hostObservationLogs().finish(session,GetTickCount64());}catch(...){ }
}
inline bool hostObservationLogsPending()noexcept{return operationalLogEnabled&&hostObservationLogs().pending();}
// Called only by the existing asynchronous diagnostic collector. It never
// holds a producer/buffer lock while acquiring the protected file writer.
inline void flushHostObservationLogs()noexcept {
  try{hostObservationLogs().drain([](const std::string& row){std::lock_guard lock(hostLogProcessMutex());(void)appendHostLogRecordLocked(row);});}catch(...){ }
}
}
