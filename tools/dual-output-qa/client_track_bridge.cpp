#include "remote_media_session.h"
#include "remote_track_configuration.h"
#include "support.hpp"
#include <fcntl.h>
#include <io.h>
#include <iostream>

// QA-only pipe adapter to the actual packaged media owner and CPU decoder.
// No Flutter window, OS input hook, display change or network credential lives
// in this process. The substitute registrar consumes actual decoded pixels.
namespace {
using V=flutter::EncodableValue;
struct Registrar final: flutter::TextureRegistrar {
  std::mutex mutex;
  std::map<int64_t,flutter::TextureVariant*> textures;
  int64_t next=100;
  int64_t RegisterTexture(flutter::TextureVariant* texture)override{
    std::lock_guard<std::mutex> lock(mutex);const auto id=++next;textures.emplace(id,texture);return id;
  }
  bool MarkTextureFrameAvailable(int64_t id)override{
    std::lock_guard<std::mutex> lock(mutex);const auto found=textures.find(id);if(found==textures.end())return false;
    const auto pixels=std::get<flutter::PixelBufferTexture>(*found->second).CopyPixelBuffer(0,0);
    if(!pixels)return false;const bool valid=pixels->buffer&&pixels->width&&pixels->height;
    pixels->release_callback(pixels->release_context);return valid;
  }
  void UnregisterTexture(int64_t id,std::function<void()> done)override{
    {std::lock_guard<std::mutex> lock(mutex);if(textures.erase(id)!=1)throw std::runtime_error("Unknown QA texture");}done();
  }
  bool UnregisterTexture(int64_t id)override{UnregisterTexture(id,[]{});return true;}
};
V native(const host::Json& value){
  if(std::holds_alternative<std::nullptr_t>(value.value))return V();
  if(const auto v=std::get_if<bool>(&value.value))return V(*v);
  if(const auto v=std::get_if<double>(&value.value)){
    if(std::abs(*v)<=9007199254740991.0&&std::trunc(*v)==*v)return V(int64_t(*v));return V(*v);
  }
  if(const auto v=std::get_if<std::string>(&value.value))return V(*v);
  if(const auto v=std::get_if<host::Json::Array>(&value.value)){flutter::EncodableList result;for(const auto& item:*v)result.push_back(native(item));return V(result);}
  flutter::EncodableMap result;for(const auto& [key,item]:std::get<host::Json::Object>(value.value))result.emplace(V(key),native(item));return V(result);
}
std::string json(const V& value){
  if(std::holds_alternative<std::monostate>(value))return "null";
  if(const auto v=std::get_if<bool>(&value))return *v?"true":"false";
  if(const auto v=std::get_if<int32_t>(&value))return std::to_string(*v);
  if(const auto v=std::get_if<int64_t>(&value))return std::to_string(*v);
  if(const auto v=std::get_if<double>(&value)){if(!std::isfinite(*v))throw std::runtime_error("Nonfinite QA statistic");return std::to_string(*v);}
  if(const auto v=std::get_if<std::string>(&value))return host::jsonQuote(*v);
  if(const auto v=std::get_if<flutter::EncodableList>(&value)){std::string result="[";for(const auto& item:*v){if(result.size()>1)result+=',';result+=json(item);}return result+']';}
  if(const auto v=std::get_if<flutter::EncodableMap>(&value)){std::string result="{";for(const auto& [key,item]:*v){const auto name=std::get_if<std::string>(&key);if(!name)throw std::runtime_error("Invalid QA statistic key");if(result.size()>1)result+=',';result+=host::jsonQuote(*name)+':'+json(item);}return result+'}';}
  throw std::runtime_error("Unsupported QA value");
}
bool read(uint8_t* bytes,size_t length){
  std::cin.read(reinterpret_cast<char*>(bytes),std::streamsize(length));return size_t(std::cin.gcount())==length;
}
void reply(const V& value){
  const auto text=json(value);if(text.size()>65536)throw std::runtime_error("QA response exceeds bound");
  std::array<uint8_t,4> header{};multitap::tracks::write32(header.data(),uint32_t(text.size()));
  std::cout.write(reinterpret_cast<const char*>(header.data()),4);std::cout.write(text.data(),std::streamsize(text.size()));std::cout.flush();
  if(!std::cout)throw std::runtime_error("QA response pipe closed");
}
}
int main(){
  try{
    if(_setmode(_fileno(stdin),_O_BINARY)==-1||_setmode(_fileno(stdout),_O_BINARY)==-1)throw std::runtime_error("QA binary pipe unavailable");
    Registrar registrar;multitap::RemoteMediaSession owner(&registrar);int64_t lease=0;
    for(;;){
      std::array<uint8_t,5> header{};if(!read(header.data(),header.size())){if(std::cin.eof()&&std::cin.gcount()==0)break;throw std::runtime_error("Partial QA request header");}
      const auto size=multitap::tracks::read32(header.data()+1);if(size>4*1024*1024)throw std::runtime_error("QA request exceeds bound");
      std::vector<uint8_t> body(size);if(size&&!read(body.data(),size))throw std::runtime_error("Partial QA request");
      switch(header[0]){
        case 1:{
          const auto parsed=host::JsonParser::parse(std::string(body.begin(),body.end()));const auto value=native(parsed);
          multitap::tracks::Configuration configuration;if(!multitap::TrackConfiguration(&value,configuration))throw std::runtime_error("Invalid QA configuration");
          std::map<uint8_t,int64_t> textures;const auto hr=owner.StartTracks(++lease,configuration,textures);if(hr!=S_OK)throw std::runtime_error("QA native startup failed");
          flutter::EncodableMap response;for(const auto& [id,texture]:textures)response.emplace(V(std::to_string(id)),V(texture));reply(V(response));break;
        }
        case 2:if(owner.Push(lease,body)!=S_OK)throw std::runtime_error("QA native queue rejected media");reply(V(true));break;
        case 3:{if(size)throw std::runtime_error("Invalid QA statistics request");flutter::EncodableMap stats;if(owner.Stats(lease,stats)!=S_OK)throw std::runtime_error("QA native statistics unavailable");reply(V(stats));break;}
        case 4:if(size!=1||body[0]>1)throw std::runtime_error("Invalid QA capture request");reply(V(false));break;
        case 5:if(size)throw std::runtime_error("Invalid QA stop request");owner.Stop(lease);reply(V(true));break;
        default:throw std::runtime_error("Unknown QA operation");
      }
    }
    owner.Close();if(!registrar.textures.empty())throw std::runtime_error("QA native textures survived shutdown");return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
