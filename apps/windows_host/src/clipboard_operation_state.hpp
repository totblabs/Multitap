#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace host {
// State only: the worker holds pullMutex_ for every call. IDs stay local and
// never enter diagnostics. An operation without an actual read stays silent.
class ClipboardOperationState {
  struct Entry { uint64_t generation; bool operation=false, reading=false, requested=false; };
  std::map<std::string,Entry> offers_;
  Entry* find(const std::string& id,uint64_t generation) {
    const auto at=offers_.find(id);return at!=offers_.end()&&at->second.generation==generation?&at->second:nullptr;
  }
 public:
  void publish(const std::string& id,uint64_t generation){offers_.insert_or_assign(id,Entry{generation});}
  void retire(const std::string& id,uint64_t generation){if(find(id,generation))offers_.erase(id);}
  void clear(){offers_.clear();}
  void request(const std::string& id,uint64_t generation){if(auto* entry=find(id,generation))entry->requested=true;}
  void reading(const std::string& id,uint64_t generation,bool on){if(auto* entry=find(id,generation))entry->reading=on;}
  bool operation(const std::string& id,uint64_t generation,bool on){
    auto* entry=find(id,generation);if(!entry||entry->operation==on)return false;
    entry->operation=on;if(on)entry->requested=entry->reading;else entry->requested=false;return true;
  }
  void cancel(const std::string& id,uint64_t generation){if(auto* entry=find(id,generation)){entry->operation=false;entry->reading=false;entry->requested=false;}}
  bool waiting()const{for(const auto& [id,entry]:offers_){(void)id;if(entry.reading||(entry.operation&&entry.requested))return true;}return false;}
};
}
