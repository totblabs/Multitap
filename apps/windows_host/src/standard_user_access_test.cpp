// A standard (non-administrator) Windows user and the device features
// (docs/standard-user-devices.md): who may use what the Host creates for the
// clipboard, shared folders, printing and USB, asked of Windows itself.
//
// Every security descriptor the Host builds for these features is checked with
// AccessCheck against two tokens made from this process's own token:
//   standard  the same account with Administrators and every other group but
//             Everyone / Authenticated Users / Users switched off and every
//             privilege removed: what a standard user's process holds;
//   other     the same, with the account itself switched off as well: a
//             different standard account on the same machine.
// Descriptors made for an account are also built for a made-up account and
// checked with `standard`: the other direction of "another account".
// No account, printer, drive, driver or setting is created or changed; the only
// thing written is one folder under %TEMP% (the clipboard store), removed again.
#include <winsock2.h>
#include "print_redirection.hpp"
#include "usb_driver.hpp"
#include "drive_winfsp_wire.hpp"
#include "worker_token.hpp"
#include "storage.hpp"
#include <aclapi.h>
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }

const GENERIC_MAPPING fileMapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
const GENERIC_MAPPING printerMapping{PRINTER_READ, PRINTER_WRITE, PRINTER_EXECUTE, PRINTER_ALL_ACCESS};
const DWORD fileWrites = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
const wchar_t* const someoneElse = L"S-1-5-21-1111111111-2222222222-3333333333-1001";

std::wstring sidText(PSID sid) {
  LPWSTR text = nullptr; if (!ConvertSidToStringSidW(sid, &text)) throw std::runtime_error("SID text");
  std::wstring out(text); LocalFree(text); return out;
}
// An impersonation token for AccessCheck: this process's token with its
// powerful groups (and, for `other`, its own account) deny-only.
WinHandle stand(bool dropAccount, std::wstring* account = nullptr) {
  HANDLE raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &raw)) throw std::runtime_error("process token");
  WinHandle token(raw);
  const auto groupBytes = workerTokenInfo(token.value, TokenGroups);
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groupBytes.data());
  std::vector<SID_AND_ATTRIBUTES> off;
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    PSID sid = groups->Groups[i].Sid;
    if (IsWellKnownSid(sid, WinWorldSid) || IsWellKnownSid(sid, WinAuthenticatedUserSid) || IsWellKnownSid(sid, WinBuiltinUsersSid)) continue;
    if (groups->Groups[i].Attributes & SE_GROUP_INTEGRITY) continue;
    off.push_back({sid, 0});
  }
  const auto userBytes = workerTokenInfo(token.value, TokenUser);
  PSID user = reinterpret_cast<const TOKEN_USER*>(userBytes.data())->User.Sid;
  if (account) *account = sidText(user);
  if (dropAccount) off.push_back({user, 0});
  HANDLE restrictedRaw = nullptr;
  if (!CreateRestrictedToken(token.value, DISABLE_MAX_PRIVILEGE, DWORD(off.size()), off.data(), 0, nullptr, 0, nullptr, &restrictedRaw)) throw std::runtime_error("restricted token");
  WinHandle restricted(restrictedRaw);
  HANDLE impersonation = nullptr;
  if (!DuplicateTokenEx(restricted.value, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &impersonation)) throw std::runtime_error("impersonation token");
  return WinHandle(impersonation);
}
// Windows maps the generic rights of a descriptor when it puts it on an object;
// AccessCheck does not, so the same is done here before asking.
void mapGenericRights(PSECURITY_DESCRIPTOR descriptor, GENERIC_MAPPING mapping) {
  PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present || !dacl) throw std::runtime_error("descriptor without a list");
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* raw = nullptr; if (!GetAce(dacl, i, &raw)) throw std::runtime_error("descriptor entry");
    if (static_cast<ACE_HEADER*>(raw)->AceType != ACCESS_ALLOWED_ACE_TYPE) throw std::runtime_error("descriptor entry kind");
    MapGenericMask(&static_cast<ACCESS_ALLOWED_ACE*>(raw)->Mask, &mapping);
  }
}
// Everything `token` would be granted on an object with `descriptor` (0: nothing).
DWORD granted(PSECURITY_DESCRIPTOR descriptor, HANDLE token, GENERIC_MAPPING mapping) {
  PRIVILEGE_SET privileges{}; DWORD size = sizeof(privileges), mask = 0; BOOL allowed = FALSE;
  if (!AccessCheck(descriptor, token, MAXIMUM_ALLOWED, &mapping, &privileges, &size, &mask, &allowed)) throw std::runtime_error("AccessCheck " + std::to_string(GetLastError()));
  return allowed ? mask : 0;
}
// The same for a descriptor in text. One without an owner gets SYSTEM's (the
// service creates these objects), which neither stand-in is.
DWORD granted(const std::wstring& text, HANDLE token, const GENERIC_MAPPING& mapping) {
  const std::wstring whole = text.rfind(L"O:", 0) == 0 ? text : L"O:SYG:SY" + text;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(whole.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("descriptor text");
  struct Free { PSECURITY_DESCRIPTOR value; ~Free() { LocalFree(value); } } free{descriptor};
  mapGenericRights(descriptor, mapping);
  return granted(descriptor, token, mapping);
}
bool all(DWORD mask, DWORD wanted) { return (mask & wanted) == wanted; }

void hostDataFolders(HANDLE standard, HANDLE other) {
  // ProgramData\Multitap\Host: the lease, the provider marker and the settings a worker reads. Read only.
  for (const HANDLE token : {standard, other}) {
    const DWORD open = granted(hostDataSecurityText(true), token, fileMapping);
    require(all(open, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE), "A standard user's worker cannot read the Host's public folder");
    require(!(open & fileWrites), "A standard user can change the Host's public folder");
    // ...\private: the credential, the display state, waiting print documents. Nothing at all.
    require(granted(hostDataSecurityText(false), token, fileMapping) == 0, "A standard user can open the Host's private folder");
  }
}
void workerPipe(HANDLE standard, HANDLE other, const std::wstring& account) {
  const DWORD own = granted(workerPipeSecurityText(account), standard, fileMapping);
  require(all(own, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE), "A standard user's worker cannot open its own service pipe");
  require(!(own & (WRITE_DAC | WRITE_OWNER | DELETE)), "A worker can change its pipe's permissions");
  require(granted(workerPipeSecurityText(account), other, fileMapping) == 0, "Another account can open a worker's pipe");
  require(granted(workerPipeSecurityText(someoneElse), standard, fileMapping) == 0, "A user can open another user's worker pipe");
}
void printerQueue(HANDLE standard, HANDLE other, const std::wstring& account) {
  const auto text = printerSecurityDescriptor(account);
  const DWORD own = granted(text, standard, printerMapping);
  require(all(own, PRINTER_ACCESS_USE | READ_CONTROL), "A standard user cannot print to their own Multitap printer");
  require(!(own & (PRINTER_ACCESS_ADMINISTER | WRITE_DAC | WRITE_OWNER | DELETE)), "A standard user can manage the Multitap printer");
  require(granted(text, other, printerMapping) == 0, "Another account can use a user's Multitap printer");
  require(granted(printerSecurityDescriptor(someoneElse), standard, printerMapping) == 0, "A user can print to another user's Multitap printer");
  // Administrators manage the queue and have no Print entry (print_redirection.hpp): a program
  // started with another account's administrator rights does not print into this user's stream.
  require(text.find(L"(A;;LCSDRCWDWO;;;BA)") != std::wstring::npos && text.find(L"SW;;;BA") == std::wstring::npos, "Administrators were given Print on the queue");
}
void sharedFolderVolume(HANDLE standard, HANDLE other, const std::wstring& account) {
  const auto text = drive::fs::volumeSecurityText(account);
  require(all(granted(text, standard, fileMapping), FILE_ALL_ACCESS), "A standard user cannot use their own shared folder volume");
  require(granted(text, other, fileMapping) == 0, "Another account can open a user's shared folder volume");
  require(granted(drive::fs::volumeSecurityText(someoneElse), standard, fileMapping) == 0, "A user can open another user's shared folder volume");
  require(drive::fs::volumeSecurityText(L"").empty(), "A volume without an owner got a descriptor");
}
void usbController(HANDLE standard, HANDLE other) {
  // Attach and detach are the service's; no user process talks to the controller.
  require(granted(usb::vhciSecurity, standard, fileMapping) == 0, "A standard user can open the USB controller");
  require(granted(usb::vhciSecurity, other, fileMapping) == 0, "Another account can open the USB controller");
}
// The clipboard's file store is made by the worker itself, in the user's own
// profile. A real one is made here (under %TEMP%) and the folder Windows now
// holds is checked: the store works without administrator rights, and its
// protected list names the account, SYSTEM and Administrators only.
void clipboardStore(HANDLE standard, HANDLE other) {
  const auto root = std::filesystem::temp_directory_path() / (L"multitap-standard-user-" + std::to_wstring(GetCurrentProcessId()));
  const auto parent = root / L"ClipboardTransfers";
  struct Cleanup { std::filesystem::path parent, root; ~Cleanup() { RemoveDirectoryW(parent.c_str()); RemoveDirectoryW(root.c_str()); } } cleanup{parent, root};
  {
    multitap::clipboard::Storage store(parent);
    store.prepare(1024, {});
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(GetNamedSecurityInfoW(parent.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS && descriptor, "The clipboard store's permissions cannot be read");
    struct Free { PSECURITY_DESCRIPTOR value; ~Free() { LocalFree(value); } } free{descriptor};
    require(all(granted(descriptor, standard, fileMapping), FILE_ALL_ACCESS), "A standard user cannot use their own clipboard store");
    require(granted(descriptor, other, fileMapping) == 0, "Another account can open a user's clipboard store");
  }
}
}  // namespace

int main() {
  try {
    std::wstring account;
    const auto standard = stand(false, &account), other = stand(true);
    require(printSidText(account), "This account's identifier is not one the Host accepts");
    hostDataFolders(standard.value, other.value);
    workerPipe(standard.value, other.value, account);
    printerQueue(standard.value, other.value, account);
    sharedFolderVolume(standard.value, other.value, account);
    usbController(standard.value, other.value);
    clipboardStore(standard.value, other.value);
    // Said with the result: an elevated run proves the same lists, but only an
    // ordinary one also made the clipboard store without administrator rights.
    std::cout << "standard user access ok: " << checks << " checks, run " << (elevated() ? "elevated" : "without administrator rights") << "\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << "\n"; return 1; }
}
