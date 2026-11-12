#pragma once
#include "native_display_change.hpp"
namespace host {
inline constexpr DWORD nativeCountTimeoutMs=25000;
struct NativeCountRequest { DisplayOperationKey operation;int count=0; };
inline NativeCountRequest nativeCountRequest(const Json& value){return {displayOperationKey(value),displayOperationCount(value)};}
inline bool nativeSameCount(const NativeCountRequest& a,const NativeCountRequest& b){return a.operation.id==b.operation.id&&a.operation.nonce==b.operation.nonce&&a.count==b.count;}
inline std::string nativeCountFields(const NativeCountRequest& request){return displayOperationFields(request.operation)+",\"count\":"+std::to_string(request.count);}
inline bool nativeCountValid(const NativeCountRequest& request){try{return nativeSameCount(request,nativeCountRequest(JsonParser::parse('{'+nativeCountFields(request)+'}')));}catch(...){return false;}}
}
