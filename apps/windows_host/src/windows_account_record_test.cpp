#include "windows_account_record.hpp"
#include <winioctl.h>
#include <cstring>
#include <iostream>
using namespace host;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F operation,const char* code){bool failed=false;try{operation();}catch(const WindowsAccountError& error){failed=std::string(error.what())==code;}require(failed,"Wrong safe Windows account failure");}
const std::string password=R"(  한글 비밀번호 🔑 \"  )";
void namesAndUnicode(){auto local=parseWindowsAccount("QA user",password);require(local.username==L"QA user"&&local.domain==L"."&&utf8(local.password)==password,"Local account/password changed");auto qualified=parseWindowsAccount("WINDOWS-PC\\한글아이디",password);require(qualified.domain==L"WINDOWS-PC"&&qualified.username==L"한글아이디"&&qualified.enteredName==L"WINDOWS-PC\\한글아이디","Qualified account parsing failed");auto upn=parseWindowsAccount("user@example.invalid",password);require(upn.domain.empty()&&upn.username==L"user@example.invalid","UPN domain must remain unspecified");auto explicitLocal=parseWindowsAccount(".\\user",password);require(explicitLocal.domain==L"."&&explicitLocal.username==L"user","Explicit local account changed");}
void invalidFields(){for(const auto& name:{std::string(),std::string("\\user"),std::string("PC\\"),std::string("PC\\user\\extra"),std::string("user\nname"),std::string(257,'a'),std::string("bad\xff")})rejects([&]{parseWindowsAccount(name,password);},"rdp_credentials_invalid_input");for(const auto& secret:{std::string(),std::string("a\0b",3),std::string("a\nb"),std::string(257,'p'),std::string("bad\xff")})rejects([&]{parseWindowsAccount("user",secret);},"rdp_credentials_invalid_input");}
void confirmationAndWindowsValidation(){int validations=0,saves=0;const auto validate=[&](const WindowsAccount& value){validations++;require(utf8(value.password)==password&&value.username==L"user","Windows verifier received a modified password");return DWORD(ERROR_SUCCESS);};const auto save=[&](const WindowsAccount& value){saves++;require(utf8(value.password)==password,"Persistence received modified Unicode password");};rejects([&]{saveWindowsAccount("user",password,password+"x",validate,save);},"rdp_credentials_confirmation");require(!validations&&!saves,"Confirmation failure reached Windows authentication");saveWindowsAccount("user",password,password,validate,save);require(validations==1&&saves==1,"Validated account was not saved exactly once");rejects([&]{saveWindowsAccount("user",password,password,[](const auto&){return DWORD(ERROR_LOGON_FAILURE);},save);},"rdp_credentials_invalid");require(saves==1,"Rejected account replaced saved credentials");for(DWORD code:{DWORD(ERROR_ACCOUNT_DISABLED),DWORD(ERROR_PASSWORD_EXPIRED),DWORD(ERROR_ACCOUNT_LOCKED_OUT),DWORD(ERROR_PASSWORD_MUST_CHANGE)})rejects([&]{checkWindowsAccountResult(code);},"rdp_account_restricted");rejects([&]{checkWindowsAccountResult(ERROR_ACCESS_DENIED);},"rdp_credentials_validation_failed");}
size_t fileChecks=0;
bool reparseExercised=false;
void checked(bool condition,const char* reason){require(condition,reason);++fileChecks;}
struct Records {
  std::filesystem::path root;int decrypts=0;
  Records(){root=std::filesystem::current_path()/L".artifacts"/(L"windows-login-clear-"+randomName());checked(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"Fresh own record workspace required");}
  std::filesystem::path write(const wchar_t* leaf,const std::string& bytes){
    const auto path=root/leaf;WinHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"Own record fixture cannot overwrite");DWORD count=0;
    checked(WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(file.value),"Own record fixture write failed");return path;
  }
  std::string read(const std::filesystem::path& path){
    WinHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"Own record read unavailable");std::string bytes(16385,'\0');DWORD count=0;
    checked(ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&count,nullptr)!=FALSE,"Own record read failed");bytes.resize(count);return bytes;
  }
  windows_login_record::Access access(const std::filesystem::path& path){
    return {path,[this](const auto& item){const auto relative=item.lexically_relative(root);const auto flags=GetFileAttributesW(item.c_str());
        return flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT)&&(item==root||(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!=L".."));},
      [](HANDLE file,const auto& item){return windows_login_record::heldPath(file,item);},
      [this](const std::string& bytes){++decrypts;if(bytes!="sealed-A"&&bytes!="sealed-B"&&bytes!="sealed-same-user")throw WindowsAccountError("rdp_credentials_storage_failed");
        return std::string("{\"format\":1,\"username\":")+jsonQuote(bytes=="sealed-B"?"account-B":"account-A")+",\"password\":\"SYNTHETIC_ONLY\"}";}};
  }
};
void actualOwnedRecordFiles(){
  using namespace windows_login_record;
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A");const auto access=f.access(path);const auto status=windowsAccountStatus(access);
    checked(status.configured&&status.username=="account-A"&&revisionText(status.clearRevision)&&f.decrypts==1,"Displayed identity/revision did not come from one protected decode");
    checked(clear(status.clearRevision,access)==Clear::removed,"Stable exact record was not deleted");
    checked(!windowsAccountStatus(access).configured&&clear(status.clearRevision,access)==Clear::missing,"Absent target was reported as another successful deletion");
    const auto newer=f.write(L"record.dpapi","sealed-B");checked(f.read(newer)=="sealed-B","Newer record after original deletion was removed");}
  for(const auto& newer:{std::string("sealed-B"),std::string("sealed-same-user"),std::string("sealed-A")}){
    Records f;const auto path=f.write(L"record.dpapi","sealed-A");const auto access=f.access(path);const auto before=windowsAccountStatus(access);
    const auto replacement=f.write(L"replacement.dpapi",newer);
    checked(MoveFileExW(replacement.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Own replacement fixture failed");
    const auto after=windowsAccountStatus(access);
    checked(after.configured&&before.clearRevision!=after.clearRevision,"New file identity/ciphertext reused old revision");
    checked(clear(before.clearRevision,access)==Clear::changed&&f.read(path)==newer,"Stale confirmation erased replacement or identical-name record");}
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A"),other=f.write(L"replacement.dpapi","sealed-B");auto access=f.access(path);
    access.decode=[&](const std::string& bytes){checked(bytes=="sealed-A","Snapshot decrypt saw replacement bytes");
      checked(!MoveFileExW(other.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Replacement succeeded inside held status decode");
      return std::string("{\"format\":1,\"username\":\"account-A\",\"password\":\"SYNTHETIC_ONLY\"}");};
    const auto status=windowsAccountStatus(access);checked(status.username=="account-A"&&f.read(path)=="sealed-A"&&f.read(other)=="sealed-B","Status paired username fromA with revision fromB");}
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A"),other=f.write(L"replacement.dpapi","sealed-B");const auto access=f.access(path);const auto status=windowsAccountStatus(access);bool reached=false;
    checked(clear(status.clearRevision,access,[&]{reached=true;
      WinHandle writer(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
      checked(writer.value==INVALID_HANDLE_VALUE,"A writer opened after native revision comparison");
      checked(!DeleteFileW(path.c_str()),"Path deletion bypassed held target protection");
      checked(!MoveFileExW(other.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Target replaced between comparison and exact-handle deletion");
      checked(f.read(path)=="sealed-A"&&f.read(other)=="sealed-B","Rejected race changed either record");
    })==Clear::removed&&reached,"Exact compared handle was not deleted");
    checked(f.read(other)=="sealed-B","Exact-handle deletion removed the competing record");}
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A");auto access=f.access(path);const auto status=windowsAccountStatus(access);
    {WinHandle reader(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
      checked(reader.value!=INVALID_HANDLE_VALUE&&clear(status.clearRevision,access)==Clear::unverified,"Conflicting held reader was ignored");}
    checked(f.read(path)=="sealed-A"&&clear(status.clearRevision,access)==Clear::removed,"Refused handle contention changed or poisoned the record");}
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A");auto access=f.access(path);const auto status=windowsAccountStatus(access);
    access.handleTrusted=[](HANDLE,const auto&){return false;};
    checked(clear(status.clearRevision,access)==Clear::unverified&&f.read(path)=="sealed-A","Unvalidated file handle authorized deletion");
    access=f.access(path);access.trusted=[](const auto&){return false;};checked(!windowsAccountStatus(access).configured&&clear(status.clearRevision,access)==Clear::unverified,"Unsafe record/parent permissions authorized deletion");
    access=f.access(path);checked(CreateHardLinkW((f.root/L"alias.dpapi").c_str(),path.c_str(),nullptr)!=FALSE,"Own record hardlink fixture unavailable");
    checked(!windowsAccountStatus(access).configured&&clear(status.clearRevision,access)==Clear::unverified,"Multi-link protected record admitted");}
  {Records f;const auto path=f.write(L"record.dpapi","corrupt-cipher");auto access=f.access(path);const auto status=windowsAccountStatus(access);
    checked(!status.configured&&status.username.empty()&&status.clearRevision.empty()&&!status.error.empty(),"Failed decode invented a username or usable revision");
    checked(clear(std::string(64,'a'),access)==Clear::unverified&&f.read(path)=="corrupt-cipher","Corrupt protected record was removed blindly");}
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A");auto access=f.access(path);const auto status=windowsAccountStatus(access);
    HANDLE raw=nullptr;checked(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)!=FALSE,"Own ACL token unavailable");WinHandle token(raw);
    DWORD length=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&length);std::vector<BYTE> bytes(length);
    checked(length&&GetTokenInformation(token.value,TokenUser,bytes.data(),length,&length),"Own ACL SID unavailable");LPWSTR sid=nullptr;
    checked(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)!=FALSE,"Own ACL SID conversion failed");
    const auto sddl=L"D:P(A;;FA;;;"+std::wstring(sid)+L")(A;;GR;;;WD)";LocalFree(sid);PSECURITY_DESCRIPTOR descriptor=nullptr;
    checked(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Own ACL fixture invalid");
    PACL dacl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    checked(GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted)&&present&&dacl,"Own ACL fixture missing DACL");
    checked(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr,nullptr,dacl,nullptr)==ERROR_SUCCESS,"Own synthetic-file ACL update failed");
    checked(!privateConnectionAcl(dacl),"Actual world-readable DACL was accepted as private");LocalFree(descriptor);
    access.handleTrusted=nativeHandleTrusted;
    checked(clear(status.clearRevision,access)==Clear::unverified&&f.read(path)=="sealed-A","Actual native held-handle ACL refusal was bypassed");}
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A"),link=f.root/L"linked.dpapi";
    reparseExercised=CreateSymbolicLinkW(link.c_str(),path.c_str(),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)!=FALSE;
    if(reparseExercised){const auto access=f.access(link);checked(!windowsAccountStatus(access).configured&&clear(std::string(64,'a'),access)==Clear::unverified&&
      f.read(path)=="sealed-A","Actual reparse record redirected a clear operation");}}
  {Records f;const auto path=f.write(L"record.dpapi","sealed-A");const auto access=f.access(path);const auto status=windowsAccountStatus(access);int refresh=0;
    for(const auto& text:{std::string("{\"action\":\"clearRdpCredentials\"}"),std::string("{\"action\":\"clearWindowsLoginV1\",\"protocol\":\"old\",\"revision\":\"")+status.clearRevision+"\"}"})
      checked(!JsonParser::parse(rpc(JsonParser::parse(text),access,[&]{++refresh;})).get("ok").boolean(),"Old/malformed RPC deleted a record");
    checked(!refresh&&f.read(path)=="sealed-A","Refused RPC mutated state or reported refreshed success");
    const auto response=JsonParser::parse(rpc(JsonParser::parse(std::string("{\"action\":\"clearWindowsLoginV1\",\"protocol\":\"windows-login-clear-v1\",\"revision\":\"")+status.clearRevision+"\"}"),access,[&]{++refresh;}));
    checked(response.get("ok").boolean()&&response.get("clear").get("outcome").string()=="removed"&&refresh==1,"Exact RPC did not report one observed removal");}
}
}
// The Host's private folder and files: a folder that
// is not exactly the Host's is moved aside, never adopted; a correct one is kept
// with its data; a private file is written with its own SYSTEM/Administrators
// list and refused before the move when it is not private.
std::wstring ownSid(){HANDLE raw=nullptr;require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)!=FALSE,"Own token unavailable");WinHandle token(raw);
  DWORD length=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&length);std::vector<BYTE> bytes(length);
  require(length&&GetTokenInformation(token.value,TokenUser,bytes.data(),length,&length),"Own SID unavailable");LPWSTR text=nullptr;
  require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text)!=FALSE,"Own SID text");std::wstring out(text);LocalFree(text);return out;}
bool exactText(const wchar_t* sddl){PSECURITY_DESCRIPTOR descriptor=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Descriptor fixture invalid");
  const bool exact=exactPrivateDescriptor(descriptor);LocalFree(descriptor);return exact;}
void privateStorage(){
  checked(exactText(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")&&exactText(L"O:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")&&exactText(L"O:BAD:P(A;;FA;;;SY)"),"The Host's own private folder security was refused");
  checked(!exactText(L"O:BUD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"),"A folder a user owns was adopted");
  checked(!exactText(L"O:S-1-5-21-1111111111-2222222222-3333333333-1001D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"),"A folder another account owns was adopted");
  checked(!exactText(L"O:BAD:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"),"An unprotected (inheriting) list was adopted");
  checked(!exactText(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;FR;;;BU)"),"A list that lets Users read was adopted");
  checked(!exactText(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIIO;FA;;;BU)"),"An inherit-only entry for Users was adopted");
  checked(!exactText(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIIO;FA;;;CO)"),"A CREATOR OWNER template was adopted");
  checked(!exactText(L"O:BAD:P(D;;FA;;;WD)(A;OICI;FA;;;SY)"),"A list with other entry kinds was adopted");
  checked(!exactText(L"O:BA"),"A descriptor without a list was adopted");
  const auto root=std::filesystem::temp_directory_path()/(L"multitap-private-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
  checked(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"Private storage workspace");
  struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}}cleanup{root};
  // The fixture's stand-in for "exactly the Host's": no file named planted.
  const auto exact=[](const std::filesystem::path& path){const DWORD a=GetFileAttributesW(path.c_str());return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT)&&!std::filesystem::exists(path/L"planted");};
  const auto own=L"D:P(A;OICI;FA;;;"+ownSid()+L")";
  const auto write=[](const std::filesystem::path& path,const char* text){std::ofstream file(path,std::ios::binary);file<<text;};
  const auto read=[](const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(file)),{});};
  const auto folder=root/L"private";
  securePrivateDirectory(folder,exact,own.c_str());
  checked(std::filesystem::is_directory(folder),"A missing private folder was not created");
  write(folder/L"credentials.dpapi","kept");
  securePrivateDirectory(folder,exact,own.c_str());
  checked(read(folder/L"credentials.dpapi")=="kept","A correct private folder lost its data");
  write(folder/L"planted","someone else's");
  securePrivateDirectory(folder,exact,own.c_str());
  checked(std::filesystem::is_directory(folder)&&!std::filesystem::exists(folder/L"planted")&&!std::filesystem::exists(folder/L"credentials.dpapi"),"An untrusted private folder was adopted");
  size_t aside=0;for(const auto& entry:std::filesystem::directory_iterator(root)){const auto name=entry.path().filename().wstring();
    if(name.rfind(L"private.untrusted-{",0)==0){++aside;checked(read(entry.path()/L"planted")=="someone else's"&&read(entry.path()/L"credentials.dpapi")=="kept","A moved-aside folder lost what was in it");}}
  checked(aside==1,"The untrusted folder was not moved aside exactly once");
  // A link where the folder should be is removed as a link; its target is untouched.
  const auto target=root/L"target";checked(CreateDirectoryW(target.c_str(),nullptr)!=FALSE,"Link target fixture");write(target/L"planted","target");
  const auto linked=root/L"linked";
  // A junction needs no privilege (a symbolic link would): a mount-point reparse buffer.
  const auto junction=[](const std::filesystem::path& link,const std::filesystem::path& to){
    if(!CreateDirectoryW(link.c_str(),nullptr))return false;
    WinHandle h(CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(h.value==INVALID_HANDLE_VALUE)return false;
    const std::wstring substitute=L"\\??\\"+to.wstring();const USHORT bytes=USHORT(substitute.size()*sizeof(wchar_t));
    std::vector<BYTE> buffer(8+8+bytes+2*sizeof(wchar_t),0);
    const auto put16=[&](size_t at,USHORT value){std::memcpy(buffer.data()+at,&value,2);};
    const ULONG tag=IO_REPARSE_TAG_MOUNT_POINT;std::memcpy(buffer.data(),&tag,4);put16(4,USHORT(buffer.size()-8));
    put16(8,0);put16(10,bytes);put16(12,USHORT(bytes+sizeof(wchar_t)));put16(14,0);
    std::memcpy(buffer.data()+16,substitute.data(),bytes);
    DWORD returned=0;return DeviceIoControl(h.value,FSCTL_SET_REPARSE_POINT,buffer.data(),DWORD(buffer.size()),nullptr,0,&returned,nullptr)!=FALSE;};
  checked(junction(linked,target),"Junction fixture unavailable");{
    checked((GetFileAttributesW(linked.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)!=0,"Junction fixture is not a reparse point");
    securePrivateDirectory(linked,exact,own.c_str());
    const DWORD a=GetFileAttributesW(linked.c_str());
    checked(a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_REPARSE_POINT)&&read(target/L"planted")=="target","A linked private folder was followed or its target changed");
  }
  // A private file whose list is not private (here: owned by this account, not SYSTEM or
  // Administrators) is refused before the move: nothing replaces the old file, no temporary stays.
  write(root/L"secret.dpapi","old");
  const std::string bytes="new";
  const bool written=writePrivateFile(root/L"secret.dpapi",bytes.data(),DWORD(bytes.size()));
  size_t temporaries=0;for(const auto& entry:std::filesystem::directory_iterator(root))if(entry.path().filename().wstring().rfind(L"secret.dpapi.",0)==0)++temporaries;
  checked(temporaries==0,"A refused private temporary file stayed behind");
  if(!elevated())checked(!written&&read(root/L"secret.dpapi")=="old","A temporary file that is not private replaced a private file");
  else{checked(written&&trustedPrivateFile(root/L"secret.dpapi"),"A private file was not written with its own protected list");}
}
int main(){try{namesAndUnicode();invalidFields();confirmationAndWindowsValidation();actualOwnedRecordFiles();privateStorage();std::cout<<"{\"kind\":\"windows-account-store-components\",\"unicodePasswordPreserved\":true,\"confirmationBeforeValidation\":true,\"mockWindowsAccountValidation\":true,\"actualWindowsAccountTested\":false,\"programDataWritten\":false,\"serviceOrPolicyChanged\":false,\"ownFileChecks\":"<<fileChecks<<",\"injectedCrypto\":true,\"actualReparseFixture\":"<<(reparseExercised?"true":"false")<<"}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
