#include "settled_selection.hpp"
#include <iostream>
#include <string>
namespace clip=multitap::clipboard;
unsigned checks=0;
void require(bool ok,const char* reason="settled_selection_qa_failed"){if(!ok)throw std::runtime_error(reason);++checks;}
template<class F> void rejects(F f,const char* code){bool rejected=false;try{f();}catch(const std::runtime_error& e){rejected=std::string(e.what())==code;}require(rejected);}
int main(){try{
  require(clip::settledQuietRemaining(100,100)==250);
  require(clip::settledQuietRemaining(100,349)==1);
  require(clip::settledQuietRemaining(100,350)==0);
  require(clip::settledQuietRemaining(100,99)==250);
  require(clip::settledTextImageHint(true,true,false,8));
  require(clip::settledTextImageHint(true,true,false,40));
  require(clip::settledTextImageHint(true,false,true,16));
  // Native read() supports HTML only as a CF_UNICODETEXT companion. It cannot
  // consume HTML-only32; adding HTML must not turn an image into allowed text.
  require(!clip::settledTextImageHint(true,true,false,32),"html_only_advertised_as_plain_text");
  require(!clip::settledTextImageHint(true,false,true,32));
  require(!clip::settledTextImageHint(true,true,true,32));
  require(!clip::settledTextImageHint(true,false,false,32));
  require(clip::settledTextImageHint(true,true,false,40));
  require(!clip::settledTextImageHint(true,false,true,40));
  require(!clip::settledTextImageHint(true,false,false,40));
  require(!clip::settledTextImageHint(true,true,false,48),"image_html_advertised_with_text_only_permission");
  require(clip::settledTextImageHint(true,false,true,48));
  require(clip::settledTextImageHint(true,true,true,48));
  require(!clip::settledTextImageHint(true,false,false,48));
  require(!clip::settledTextImageHint(false,true,true,48));
  require(!clip::settledTextImageHint(true,true,false,16));
  require(!clip::settledTextImageHint(false,true,true,56));
  require(!clip::settledTextImageHint(true,true,true,0));
  for(const auto files:{1u,2u,4u,9u,18u,36u})require(!clip::settledTextImageHint(true,true,true,files));
  clip::validateSettledSelection(0,0,8);require(true);
  clip::validateSettledSelection(0xffffffffu,0xffffffffu,16);require(true);
  rejects([]{clip::validateSettledSelection(41,42,8);},"clipboard_changed");
  rejects([]{clip::validateSettledSelection(41,41,9);},"unsupported_clipboard_format");
  rejects([]{clip::validateSettledSelection(41,41,4);},"unsupported_clipboard_format");
  rejects([]{clip::validateSettledSelection(41,41,0);},"unsupported_clipboard_format");
  rejects([]{clip::validateSettledSelection(41,41,32);},"unsupported_clipboard_format");
  clip::validateSettledSelection(41,41,40);require(true);
  clip::validateSettledSelection(41,41,48);require(true);
  rejects([]{clip::validateSettledSelection(41,42,48);},"clipboard_changed");
  for(const auto file: {1u,2u,4u})
    rejects([&]{clip::validateSettledSelection(41,41,48u|file);},"unsupported_clipboard_format");
  std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"memoryOnly\":true,\"osClipboard\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
