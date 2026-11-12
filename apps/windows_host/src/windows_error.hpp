#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>
namespace host {
class WindowsOperationError:public std::runtime_error {
 public:
  WindowsOperationError(const char* operation,DWORD code)
      :std::runtime_error(std::string(operation)+" (Windows "+std::to_string(code)+")"),code_(code){}
  DWORD systemError()const noexcept{return code_;}
 private:DWORD code_;
};
inline void winCheck(BOOL result,const char* operation){
  if(!result){const DWORD code=GetLastError();throw WindowsOperationError(operation,code);}
}
}
