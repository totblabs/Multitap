#pragma once
#include "remote_pointer.hpp"
#include "track_source.hpp"
#include <cmath>
#include <optional>
#include <set>
namespace host {
// relative-mouse-tracks-v1 (docs/remote-session-protocol.md, "Relative mouse"):
// independent outputs take 게임 마우스 movement, and buttons and wheel at the
// current cursor, only in a session.start carrying both granted markers.
inline constexpr const char* relativeMouseTracksProtocol="relative-mouse-tracks-v1";
inline bool relativeMouseTrackSessionRequested(const Json& start){
  return start.get("type").string()=="session.start"&&start.get("mode").string()=="stream"&&start.get("mediaProtocol").string()=="tracks-v1"&&
         start.get("relativeMouseProtocol").string()==relativeMouseProtocol&&start.get("relativeMouseTracksProtocol").string()==relativeMouseTracksProtocol;
}
// Shared focus/input ownership for one source session. A close waits for an
// admitted injection to finish, then no old command can press a key afterward.
class TrackSourceInput {
 public:
  explicit TrackSourceInput(TrackSourcePlan plan,bool relative=false):plan_(std::move(plan)),relative_(relative){for(const auto& output:plan_.outputs)rects_.push_back(output.capture.bounds);}
  void encodedReady(){std::lock_guard lock(mutex_);if(!closed_)encoded_=true;}
  void close(){std::lock_guard lock(mutex_);closed_=true;ready_=false;}
  // capture_follow.hpp: output `id` now reads `rect`; positions on its picture
  // map onto it. `area` gets the new union of the outputs (the injector's
  // area) under the same lock, so no input maps with half of the change.
  void follow(uint8_t id,RECT rect,const std::function<void(RECT)>& area){
    std::lock_guard lock(mutex_);
    for(size_t n=0;n<plan_.outputs.size();++n)if(plan_.outputs[n].media.id==id)rects_[n]=rect;
    RECT all=rects_.front();for(const auto& r:rects_){all.left=std::min(all.left,r.left);all.top=std::min(all.top,r.top);all.right=std::max(all.right,r.right);all.bottom=std::max(all.bottom,r.bottom);}
    plan_.inputArea=all;if(area)area(all);
  }
  bool acknowledge(const Json& message){
    std::lock_guard lock(mutex_);
    if(closed_||!encoded_||message.get("type").string()!="stream.tracks.ready"||
       !trackFields(message,{"type","sessionId","topologyEpoch","tracks"})||
       message.get("sessionId").string()!=plan_.sessionId||message.get("topologyEpoch").number()!=plan_.topology)return false;
    const auto* tracks=std::get_if<Json::Array>(&message.get("tracks").value);if(!tracks||tracks->size()!=plan_.outputs.size())return false;
    unsigned mask=0;
    for(const auto& item:*tracks){
      if(!trackFields(item,{"id","epoch"}))return false;
      const auto id=item.get("id").string();const auto output=std::find_if(plan_.outputs.begin(),plan_.outputs.end(),[&](const auto& value){return id==std::to_string(value.media.id);});
      if(output==plan_.outputs.end()||item.get("epoch").number()!=output->media.epoch||(mask&(1u<<output->media.id)))return false;
      mask|=1u<<output->media.id;
    }ready_=true;return true;
  }
  bool apply(const Json& message,const std::function<void(const Json&)>& inject){
    if(!inject)return false;return applyPlaced(message,[&](const Json& converted,const std::optional<POINT>&){inject(converted);});
  }
  // System input (system_input.hpp): the same admission and conversion, and for an input that names an output the
  // desktop pixel its position is, so the service can place the client's own event there. placed runs under this
  // source's lock: an answer written from it is ordered with every follow() of the same source.
  using Placed=std::function<void(const Json& converted,const std::optional<POINT>& desktop)>;
  bool applyPlaced(const Json& message,const Placed& inject){
    std::lock_guard lock(mutex_);std::optional<POINT> desktop;if(closed_||!ready_||!inject||message.get("type").string()!="input"||
      message.get("sessionId").string()!=plan_.sessionId||message.get("topologyEpoch").number()!=plan_.topology)return false;
    const auto event=message.get("event").string();std::set<std::string> fields{"type","sessionId","topologyEpoch","event"};
    auto converted=message;
    const auto integral=[&](const char* name,double minimum,double maximum){const auto n=message.get(name).number(minimum-1);return n>=minimum&&n<=maximum&&n==std::floor(n);};
    const auto boolean=[&](const char* name){return std::holds_alternative<bool>(message.get(name).value);};
    const auto* object=std::get_if<Json::Object>(&message.value);
    if(event=="key"){
      fields.insert({"vk","scan","down","extended"});
      if(!integral("vk",1,255)||!integral("scan",0,65535)||!boolean("down")||!boolean("extended"))return false;
    }else if(relative_&&object&&(event=="moveRelative"||((event=="button"||event=="wheel")&&!object->contains("track")))){
      // 게임 마우스: movement, or a button/wheel at the current cursor; no output, no position.
      if(event=="moveRelative"){fields.insert({"dx","dy"});if(!integral("dx",-relativeMouseMaximum,relativeMouseMaximum)||!integral("dy",-relativeMouseMaximum,relativeMouseMaximum)||(!message.get("dx").number()&&!message.get("dy").number()))return false;}
      if(event=="button"){fields.insert({"button","down"});if(!boolean("down")||!remotePointerButton(message.get("button").string()))return false;}
      if(event=="wheel"){fields.insert("delta");if(!integral("delta",-12000,12000))return false;}
    }else if(event=="move"||event=="button"||event=="wheel"){
      fields.insert({"track","epoch","x","y"});
      const auto id=message.get("track").string();const auto output=std::find_if(plan_.outputs.begin(),plan_.outputs.end(),[&](const auto& value){return id==std::to_string(value.media.id);});
      const auto x=message.get("x").number(-1),y=message.get("y").number(-1);
      if(output==plan_.outputs.end()||message.get("epoch").number()!=output->media.epoch||!std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1)return false;
      if(event=="button"){fields.insert({"button","down"});const auto button=message.get("button").string();if(!boolean("down")||!remotePointerButton(button))return false;}
      if(event=="wheel"){fields.insert("delta");if(!integral("delta",-12000,12000))return false;}
      // The output's current rectangle (it moves when an application changes a mode: capture_follow.hpp).
      const auto& r=rects_[size_t(output-plan_.outputs.begin())];const auto width=plan_.inputArea.right-plan_.inputArea.left,height=plan_.inputArea.bottom-plan_.inputArea.top;
      if(width<2||height<2)return false;
      std::get<Json::Object>(converted.value)["x"].value=(r.left+x*(r.right-r.left-1)-plan_.inputArea.left)/(width-1);
      std::get<Json::Object>(converted.value)["y"].value=(r.top+y*(r.bottom-r.top-1)-plan_.inputArea.top)/(height-1);
      desktop=POINT{LONG(std::lround(r.left+x*(r.right-r.left-1))),LONG(std::lround(r.top+y*(r.bottom-r.top-1)))};
    }else return false;
    if(!object||object->size()!=fields.size())return false;for(const auto& [name,value]:*object){(void)value;if(!fields.contains(name))return false;}
    inject(converted,desktop);return true;
  }
 private:
  TrackSourcePlan plan_;bool relative_=false;std::vector<RECT> rects_;std::recursive_mutex mutex_;bool closed_=false,encoded_=false,ready_=false;
};
}
