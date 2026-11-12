#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include "video_bounds.h"

namespace multitap {
// Physical client-pixel geometry. HWNDs/rectangles come from the local native
// view, never from the remote VM. Remote track identity is a separate field.
class RemoteWindowGroup {
 public:
  struct Surface {uint8_t track=0;uint32_t epoch=0,width=0,height=0;HWND window=nullptr;RECT image{};std::vector<RECT> blocked;};
  struct Point {uint8_t track=0;uint32_t epoch=0,topology=0;double x=0,y=0;};
  bool Configure(uint32_t topology,std::vector<Surface> surfaces){
    if(!topology||surfaces.empty()||surfaces.size()>2)return false;
    unsigned ids=0;
    uint64_t bytes=0;std::vector<HWND> roots;std::vector<RECT> screenImages;
    for(const auto& s:surfaces){
      RECT client{};
      if(s.track>1||!s.epoch||ids&(1u<<s.track)||!s.window||!IsWindow(s.window)||!GetClientRect(s.window,&client)||
        s.width<128||s.height<128||!ValidVideoSize(s.width,s.height)||s.image.left<0||s.image.top<0||
        int64_t(s.image.right)-s.image.left!=s.width||int64_t(s.image.bottom)-s.image.top!=s.height||
        s.image.right>client.right||s.image.bottom>client.bottom||s.blocked.size()>8)return false;
      for(const auto& b:s.blocked)if(b.left<0||b.top<0||b.right<=b.left||b.bottom<=b.top||b.right>client.right||b.bottom>client.bottom)return false;
      const HWND root=GetAncestor(s.window,GA_ROOT);if(!root||std::find(roots.begin(),roots.end(),root)!=roots.end())return false;
      POINT origin{};if(!ClientToScreen(s.window,&origin))return false;
      const auto fits=[](int64_t n){return n>=std::numeric_limits<LONG>::min()&&n<=std::numeric_limits<LONG>::max();};
      if(!fits(int64_t(origin.x)+s.image.left)||!fits(int64_t(origin.y)+s.image.top)||!fits(int64_t(origin.x)+s.image.right)||!fits(int64_t(origin.y)+s.image.bottom))return false;
      const RECT image{LONG(int64_t(origin.x)+s.image.left),LONG(int64_t(origin.y)+s.image.top),LONG(int64_t(origin.x)+s.image.right),LONG(int64_t(origin.y)+s.image.bottom)};
      for(const auto& other:screenImages){RECT overlap{};if(IntersectRect(&overlap,&image,&other))return false;}
      if(!video::checkedAdd(bytes,uint64_t(s.width)*s.height*4,bytes)||bytes>uint64_t(std::numeric_limits<std::ptrdiff_t>::max()))return false;
      roots.push_back(root);screenImages.push_back(image);
      ids|=1u<<s.track;
    }
    topology_=topology;surfaces_=std::move(surfaces);return true;
  }
  std::optional<Point> Map(POINT screen)const{
    // A window's toolbar/dialog exclusions win before any other view mapping.
    // The actual cursor can cross an unequal-height/gapped monitor layout.
    std::optional<Point> result;
    for(const auto& s:surfaces_){
      POINT client=screen;if(!IsWindow(s.window)||!ScreenToClient(s.window,&client))return {};
      RECT bounds{};if(!GetClientRect(s.window,&bounds)||s.image.right>bounds.right||s.image.bottom>bounds.bottom)return {};
      if(client.x<s.image.left||client.x>=s.image.right||client.y<s.image.top||client.y>=s.image.bottom)continue;
      for(const auto& b:s.blocked)if(PtInRect(&b,client))return {};
      if(result)return {}; // Later window movement must not create ambiguous input.
      result=Point{s.track,s.epoch,topology_,double(client.x-s.image.left)/std::max(1u,s.width-1),double(client.y-s.image.top)/std::max(1u,s.height-1)};
    }
    return result;
  }
  std::optional<Point> MapVisible(POINT screen)const{
    const auto point=Map(screen);if(!point)return {};
    const HWND hit=WindowFromPoint(screen);if(!hit)return {};
    for(const auto& s:surfaces_)if(s.track==point->track&&GetAncestor(hit,GA_ROOT)==GetAncestor(s.window,GA_ROOT))return point;
    return {}; // Owned dialogs and other applications do not receive remote input.
  }
  bool Contains(HWND window)const{
    if(!window||!IsWindow(window))return false;
    const auto root=GetAncestor(window,GA_ROOT);
    return std::any_of(surfaces_.begin(),surfaces_.end(),[&](const auto& s){return IsWindow(s.window)&&GetAncestor(s.window,GA_ROOT)==root;});
  }
  uint32_t topology()const{return topology_;}
  void Clear(){topology_=0;surfaces_.clear();}
 private:
  uint32_t topology_=0;
  std::vector<Surface> surfaces_;
};
}
