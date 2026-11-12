#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <filesystem>
#include <regex>
#include "../../apps/windows_client/windows/runner/product_update.hpp"
namespace multitap {
// Released clients already stage verified installers in this protected root.
// Recognize that handoff so their first upgrade also needs no second Install
// click. A normal download-folder launch keeps the graphical confirmation.
// The stage is ProgramData\Multitap-Updates\{GUID}: owner Administrators and
// only SYSTEM/Administrators access, which only an administrator can create.
inline bool verifiedUpdateStage(const std::wstring& filename, std::filesystem::path& verifiedStage)noexcept{
  try{
    update_detail::Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)};
    if(snapshot.value==INVALID_HANDLE_VALUE)return false;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);DWORD parent=0;
    if(Process32FirstW(snapshot.value,&entry))do{if(entry.th32ProcessID==GetCurrentProcessId()){parent=entry.th32ParentProcessID;break;}}while(Process32NextW(snapshot.value,&entry));
    if(!parent)return false;
    update_detail::Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent)};if(!process.value)return false;
    wchar_t image[32768]{};DWORD length=32768;if(!QueryFullProcessImageNameW(process.value,0,image,&length))return false;
    const std::filesystem::path path(image);if(_wcsicmp(path.filename().c_str(),filename.c_str()))return false;
    const auto stage=path.parent_path();
    if(!std::regex_match(stage.filename().wstring(),std::wregex(L"\\{[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}\\}")))return false;
    PWSTR folder=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&folder)))return false;
    const auto root=std::filesystem::path(folder)/L"Multitap-Updates";CoTaskMemFree(folder);
    if(_wcsicmp(stage.parent_path().c_str(),root.c_str()))return false;
    update_detail::Handle rootHandle{update_detail::DirectoryHandle(root.wstring())},stageHandle{update_detail::DirectoryHandle(stage.wstring())};
    update_detail::TrustedDirectory(rootHandle.value);update_detail::TrustedDirectory(stageHandle.value);
    update_detail::Handle file{CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attributes,sizeof(attributes))||(attributes.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
    verifiedStage=stage;return true;
  }catch(...){return false;}
}
inline bool verifiedUpdateParent(const std::wstring& filename)noexcept{std::filesystem::path stage;return verifiedUpdateStage(filename,stage);}

// The unelevated client no longer starts an installer from a folder it can
// write: it starts its own administrator-protected EXE, which stages the
// verified installer in the protected root above (product_update.hpp). A
// signature file beside an installer in a user-writable folder (the earlier
// "verifiedSignedUpdateParent" recognition, which identified its parent by
// image path) is therefore not accepted any more: such a start keeps the
// graphical installer with its Install click.
}
