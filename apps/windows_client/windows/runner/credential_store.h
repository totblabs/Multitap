#ifndef RUNNER_CREDENTIAL_STORE_H_
#define RUNNER_CREDENTIAL_STORE_H_

#include <windows.h>
#include <wincred.h>
#include <shlobj.h>

#include <optional>
#include <regex>
#include <string>
#include <vector>
#include "client_transport_policy.h"

// App-specific generic credentials, scoped to this Windows user and broker.
// The UTF-8 password blob is never written to an application settings file.
namespace multitap {

struct SavedCredential {
  std::string username;
  std::string password;
};

inline std::optional<ClientTransportSettings> ReadClientTransportSettings() {
  PWSTR local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DONT_VERIFY,nullptr,&local)) || !local) return std::nullopt;
  std::wstring base(local); CoTaskMemFree(local);
  std::vector<HANDLE> held;
  struct Close { std::vector<HANDLE>& values; ~Close(){ for (auto h:values) CloseHandle(h); } } close{held};
  // No environment-selected/arbitrary file; hold and reject reparse ancestors.
  for (const auto& suffix : {std::wstring(),std::wstring(L"\\Multitap"),std::wstring(L"\\Multitap\\Client")}) {
    const auto folder=base+suffix;
    const auto h=CreateFileW(folder.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
      nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return std::nullopt;held.push_back(h);
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||
      (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return std::nullopt;
  }
  const auto file=base+L"\\Multitap\\Client\\settings.json";
  const auto h=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
    FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if(h==INVALID_HANDLE_VALUE)return std::nullopt;held.push_back(h);
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
  if(!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||
    !GetFileSizeEx(h,&size)||size.QuadPart<=0||size.QuadPart>4096)return std::nullopt;
  std::string json(static_cast<size_t>(size.QuadPart),'\0');DWORD bytes=0;
  if(!ReadFile(h,json.data(),static_cast<DWORD>(json.size()),&bytes,nullptr)||bytes!=json.size())return std::nullopt;
  return ClientTransportSettingsParse(json);
}
inline bool CredentialOriginValid(const std::string& origin) {
  // Existing HTTPS never consults optional transport metadata. HTTP must use
  // the consent actually saved by this Windows user, not an RPC boolean.
  if(origin.rfind("https://",0)==0)return ClientTransportSyntaxValid(origin);
  return ClientTransportAuthorized(origin,ReadClientTransportSettings());
}
// A password is remembered only for an HTTPS server: over plain HTTP it
// crosses the network readable, and a
// remembered one would be replayed by the silent re-login. An HTTP origin
// reads as "nothing remembered" and refuses to save; deleting still works.
inline bool CredentialPasswordOriginAllowed(const std::string& origin) {
  return origin.rfind("https://",0)==0;
}

inline std::optional<std::wstring> CredentialWide(const std::string& value) {
  if (value.empty()) return std::wstring();
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (!length) return std::nullopt;
  std::wstring converted(length, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), converted.data(),
                          length) != length) {
    return std::nullopt;
  }
  return converted;
}

inline std::optional<std::string> CredentialUtf8(const wchar_t* value) {
  if (!value) return std::nullopt;
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
  if (!length) return std::nullopt;
  std::string converted(length, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                           converted.data(), length, nullptr, nullptr)) {
    return std::nullopt;
  }
  converted.pop_back();
  return converted;
}

inline std::wstring CredentialTarget(const std::string& origin) {
  return L"Multitap/broker/" + *CredentialWide(origin);
}

struct CredentialFunctions {
  decltype(&::CredReadW) read=&::CredReadW;
  decltype(&::CredDeleteW) remove=&::CredDeleteW;
  decltype(&::CredFree) release=&::CredFree;
};

inline bool DeleteCredential(const std::string& origin, DWORD* error,
                             const CredentialFunctions& api={}) {
  // Revoked consent must not prevent forgetting that origin's own credential.
  if (!ClientTransportSyntaxValid(origin)) {
    *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  if(!api.remove(CredentialTarget(origin).c_str(),CRED_TYPE_GENERIC,0)) {
    const DWORD failed=GetLastError();
    if(failed!=ERROR_NOT_FOUND){*error=failed;return false;}
  }
  return true;
}

inline bool SaveCredential(const std::string& origin,
                           const std::string& username,
                           const std::string& password, DWORD* error) {
  const auto wide_username = CredentialWide(username);
  if (!CredentialPasswordOriginAllowed(origin) || !CredentialOriginValid(origin) || !wide_username || username.empty() ||
      wide_username->size() > 128 || username.find('\0') != std::string::npos ||
      password.empty() || password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE ||
      password.find('\0') != std::string::npos || !CredentialWide(password)) {
    *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  auto target = CredentialTarget(origin);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = target.data();
  credential.UserName = const_cast<LPWSTR>(wide_username->c_str());
  credential.CredentialBlobSize = static_cast<DWORD>(password.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(
      const_cast<char*>(password.data()));
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  if (CredWriteW(&credential, 0)) return true;
  *error = GetLastError();
  return false;
}

inline bool ReadCredentialTarget(const std::wstring& target,
                           std::optional<SavedCredential>* output,
                           DWORD* error,const CredentialFunctions& api={}) {
  output->reset();
  PCREDENTIALW credential = nullptr;
  if (!api.read(target.c_str(), CRED_TYPE_GENERIC, 0,
                 &credential)) {
    *error = GetLastError();
    return *error == ERROR_NOT_FOUND;
  }
  const auto username = CredentialUtf8(credential->UserName);
  bool valid = username && !username->empty() &&
               credential->CredentialBlobSize > 0 &&
               credential->CredentialBlobSize <= CRED_MAX_CREDENTIAL_BLOB_SIZE;
  std::string password;
  if (valid) {
    password.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                    credential->CredentialBlobSize);
    valid = password.find('\0') == std::string::npos &&
            CredentialWide(password).has_value();
  }
  if (valid) *output = SavedCredential{*username, password};
  // Wipe the Windows-allocated blob before releasing its single allocation.
  if (credential->CredentialBlob && credential->CredentialBlobSize) {
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
  }
  api.release(credential);
  if (!valid) *error = ERROR_INVALID_DATA;
  return valid;
}

inline bool ReadCredential(const std::string& origin,
                          std::optional<SavedCredential>* output,DWORD* error) {
  output->reset();
  if(!CredentialOriginValid(origin)){*error=ERROR_INVALID_PARAMETER;return false;}
  if(!CredentialPasswordOriginAllowed(origin))return true;  // never read back over HTTP
  return ReadCredentialTarget(CredentialTarget(origin),output,error);
}

}  // namespace multitap

#endif  // RUNNER_CREDENTIAL_STORE_H_
