#include "direct_input.hpp"
#include "direct_input_validation.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace host;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{
  require(validDirectInput(JsonParser::parse(R"({"type":"input","event":"key","vk":65,"scan":30,"down":true,"extended":false})")),"ordinary key rejected");
  const auto relative=JsonParser::parse(R"({"type":"input","event":"moveRelative","dx":3,"dy":-2})");require(!validDirectInput(relative)&&validDirectInput(relative,true),"relative permission gate");
  require(!validDirectInput(JsonParser::parse(R"({"type":"input","event":"moveRelative","dx":3,"dy":-2,"x":null})"),true),"invalid relative geometry accepted");
  require(!validDirectInput(JsonParser::parse(R"({"type":"host.power.request","action":"shutdown"})")),"UDP admitted a privileged control command");
  DirectInputOrder order;std::vector<uint32_t> seen;const auto emit=[&](const DirectInputItem& item){seen.push_back(item.sequence);};const auto current=[]{return true;};
  order.accept(false,{1,"key.down",1},emit,current);require(seen.empty(),"UDP started without a WSS owner");
  order.accept(true,{1,"key.down",2},emit,current);order.accept(false,{3,"key.up",3},emit,current);require(seen==std::vector<uint32_t>{1},"UDP skipped a missing key");
  order.accept(false,{2,"mouse",4},emit,current);order.accept(true,{2,"mouse",5},emit,current);order.accept(true,{3,"key.up",6},emit,current);require(seen==std::vector<uint32_t>({1,2,3}),"duplicate or reordered injection");
  order.accept(false,{5,"key.down",7},emit,current);order.pause();order.accept(false,{4,"mouse",8},emit,current);order.accept(false,{6,"key.up",9},emit,current);require(seen.size()==3,"input after a focus/revocation barrier");
  order.accept(true,{7,"reset",10},emit,current);order.accept(false,{8,"mouse",11},emit,current);require(seen==std::vector<uint32_t>({1,2,3,7,8}),"WSS failed to advance a denied/missing UDP input");
  order.accept(true,{9,"old worker",12},emit,[]{return false;});require(seen.size()==5,"retired worker injected");
  std::vector<uint32_t> all;std::mutex captured;std::condition_variable complete;
  DirectInputDispatcher dispatcher([&](const DirectInputItem& item){std::lock_guard lock(captured);all.push_back(item.sequence);complete.notify_all();});
  for(uint32_t n=1;n<=1000;++n){dispatcher.submit(false,{n,"fast",1000000+n});dispatcher.submit(true,{n,"reliable",1000000+n});}
  {std::unique_lock lock(captured);require(complete.wait_for(lock,std::chrono::seconds(3),[&]{return all.size()==1000;}),"reliable inputs were lost");}
  // pause waits for the current injection and retires all older queued work.
  dispatcher.pause();const size_t before=all.size();dispatcher.submit(false,{1001,"late",2000000});dispatcher.stop();require(all.size()==before,"late input crossed stop");
  for(size_t n=0;n<all.size();++n)require(all[n]==n+1,"loss/reorder/duplicate across both dispatcher paths");
  std::cout<<"{\"kind\":\"direct-input-order\",\"orderedInputs\":"<<all.size()<<",\"realInput\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
