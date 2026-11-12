#pragma once
#include "display_v2.hpp"
#include <mutex>

namespace host::signin::preparation {
// Parent-only original state. The preparer supplies bytes, never a path. A new
// original is published without replacement, then held against write/delete
// until the exact child exits. Closing this object never removes the original.
class OriginalFile {
 public:
  using Trusted=std::function<bool(const std::filesystem::path&)>;
  explicit OriginalFile(std::filesystem::path path,Trusted trusted=trustedMachineFile)
    :path_(std::move(path)),trusted_(std::move(trusted)){}
  OriginalFile(const OriginalFile&)=delete;
  OriginalFile& operator=(const OriginalFile&)=delete;

  bool absent()const{
    std::lock_guard lock(mutex_);
    if(file_.value||!trusted_||!trusted_(path_.parent_path()))return false;
    if(GetFileAttributesW(path_.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    return GetLastError()==ERROR_FILE_NOT_FOUND;
  }

  bool adopt(const std::string& expectedSha256){
    std::lock_guard lock(mutex_);
    if(file_.value||!digest(expectedSha256))return false;
    return open(expectedSha256,{});
  }
  bool create(const std::string& bytes,const std::string& session){
    std::lock_guard lock(mutex_);
    if(file_.value||!trusted_||!trusted_(path_.parent_path())||bytes.empty()||bytes.size()>65536)return false;
    try{if(session.empty()||parseDisplayV2State(bytes).session!=session)return false;}catch(...){return false;}
    const auto attributes=GetFileAttributesW(path_.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES)return false;
    const auto absent=GetLastError();
    if(absent!=ERROR_FILE_NOT_FOUND)return false;
    const std::filesystem::path temporary=path_.wstring()+temporarySuffix();
    bool created=false;
    try{
      WinHandle output(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      created=output.value!=INVALID_HANDLE_VALUE;
      DWORD written=0;BY_HANDLE_FILE_INFORMATION info{};
      if(!created||!GetFileInformationByHandle(output.value,&info)||info.nNumberOfLinks!=1||
          (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
          !WriteFile(output.value,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()||
          !FlushFileBuffers(output.value))throw std::runtime_error("Original display state was not saved");
      output=WinHandle{};
      if(!trusted_(temporary)||!trusted_(path_.parent_path())||
          !MoveFileExW(temporary.c_str(),path_.c_str(),MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Original display state was not published");
      created=false;
      return open(nativeBaselineDigest(bytes),bytes);
    }catch(...){if(created)(void)DeleteFileW(temporary.c_str());return false;}
  }
  std::optional<std::string> current()const{
    std::lock_guard lock(mutex_);
    try{
      if(!file_.value||!trusted_||!trusted_(path_.parent_path())||!trusted_(path_))return {};
      const auto bytes=readHeld(file_.value);
      if(!bytes||*bytes!=bytes_||nativeBaselineDigest(*bytes)!=sha256_)return {};
      return bytes;
    }catch(...){return {};}
  }
  // The service calls this only after positively observing its exact child
  // exit. The ordinary V2 restorer can then open/remove the retained original.
  // No peer message alone is allowed to release the held file.
  void releaseForRestore(){std::lock_guard lock(mutex_);file_=WinHandle{};}
  // Parent-only final retirement. The callback proves actual child exit and
  // completed topology restoration; it must not call back into this file.
  // Delete the exact reopened file ID/bytes, never a second path lookup.
  bool retireRestored(const std::string& expectedSha256,const std::function<bool()>& restored){
    std::lock_guard lock(mutex_);
    try{
      if(!file_.value||!restored||expectedSha256!=sha256_||!trusted_||
          !trusted_(path_.parent_path())||!trusted_(path_))return false;
      BY_HANDLE_FILE_INFORMATION before{};
      const auto original=readHeld(file_.value);
      if(!original||*original!=bytes_||nativeBaselineDigest(*original)!=expectedSha256||
          !GetFileInformationByHandle(file_.value,&before)||!restored())return false;
      file_=WinHandle{};
      WinHandle exact(CreateFileW(path_.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,
        OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      BY_HANDLE_FILE_INFORMATION after{};
      if(exact.value==INVALID_HANDLE_VALUE||!trusted_(path_)||
          !GetFileInformationByHandle(exact.value,&after)||
          before.dwVolumeSerialNumber!=after.dwVolumeSerialNumber||
          before.nFileIndexHigh!=after.nFileIndexHigh||before.nFileIndexLow!=after.nFileIndexLow||
          readHeld(exact.value)!=original||!restored())return false;
      FILE_DISPOSITION_INFO disposition{TRUE};
      if(!SetFileInformationByHandle(exact.value,FileDispositionInfo,&disposition,sizeof(disposition)))return false;
      exact=WinHandle{};
      if(GetFileAttributesW(path_.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
      return GetLastError()==ERROR_FILE_NOT_FOUND;
    }catch(...){return false;}
  }
  std::string sha256()const{std::lock_guard lock(mutex_);return sha256_;}
 private:
  static bool digest(const std::string& value){return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
  static std::optional<std::string> readHeld(HANDLE file){
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{},zero{};
    if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file,&info)||
        info.nNumberOfLinks!=1||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
        !GetFileSizeEx(file,&length)||length.QuadPart<=0||length.QuadPart>65536||
        !SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))return {};
    std::string bytes(size_t(length.QuadPart),'\0');DWORD read=0;
    if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())return {};
    char trailing=0;
    if(!ReadFile(file,&trailing,1,&read,nullptr)||read)return {};
    return bytes;
  }
  bool open(const std::string& expectedSha256,const std::string& expectedBytes){
    try{
      if(!trusted_||!trusted_(path_.parent_path()))return false;
      WinHandle held(CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      if(held.value==INVALID_HANDLE_VALUE||!trusted_(path_))return false;
      const auto bytes=readHeld(held.value);
      if(!bytes||nativeBaselineDigest(*bytes)!=expectedSha256||(!expectedBytes.empty()&&*bytes!=expectedBytes))return false;
      (void)parseDisplayV2State(*bytes);
      BY_HANDLE_FILE_INFORMATION identity{};
      if(!GetFileInformationByHandle(held.value,&identity))return false;
      // Re-acquiring the SAME original after a sharing refusal may not adopt
      // an identical-byte replacement under an old topology/exit receipt.
      if(identityKnown_&&sha256_==expectedSha256&&
          (identity_.dwVolumeSerialNumber!=identity.dwVolumeSerialNumber||
           identity_.nFileIndexHigh!=identity.nFileIndexHigh||identity_.nFileIndexLow!=identity.nFileIndexLow))return false;
      identity_=identity;identityKnown_=true;
      bytes_=*bytes;sha256_=expectedSha256;file_=std::move(held);return true;
    }catch(...){return false;}
  }
  std::filesystem::path path_;Trusted trusted_;
  mutable std::mutex mutex_;WinHandle file_;
  std::string bytes_,sha256_;
  BY_HANDLE_FILE_INFORMATION identity_{};bool identityKnown_=false;
};
}
