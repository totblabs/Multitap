// Real ShellLink/filesystem and isolated HKCU test key only. Never installs,
// stops or removes a real product, service, driver, account or user profile.
#include "setup_payload_selftest.hpp"
#include "setup_uninstall.hpp"
#include <iostream>
using namespace multitap::payload;
namespace qa=multitap::payload::qa;
namespace install=multitap::installation;
static void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static void marker(const std::wstring& directory,const std::wstring& file,const std::string& product,const std::string& version){
  const auto text="{\"product\":\""+product+"\",\"version\":\""+version+"\"}";check(qa::writeBytes(directory+L"\\"+file,{text.begin(),text.end()}),"write marker");
}
template<class F> static void refused(F run,const char* why){bool failed=false;try{run();}catch(const Failure&){failed=true;}check(failed,why);}
int main(){unsigned checks=0;try{
  // The payload-v1 placement, shortcut and request rules (throwaway trees under %TEMP%).
  check(qa::Run(),"payload contract");++checks;
  qa::Tree tree;check(tree.ready(),"tree");Roots roots;roots.qa=true;roots.programFiles=tree/L"ProgramFiles";roots.localAppData=tree/L"LocalAppData";roots.desktop=tree/L"Desktop";roots.source=tree/L"Source";
  for(const auto& path:{roots.programFiles,roots.localAppData,roots.desktop,roots.source})check(qa::makeDirectory(path),"root");
  // One fixed folder: Client\Multitap.exe with its marker; Client\1.0.5 is a leftover of an earlier layout.
  const auto current=roots.programFiles+L"\\Multitap\\Client",leftover=current+L"\\1.0.5";
  check(qa::makeDirectory(current)&&qa::writeBytes(current+L"\\Multitap.exe",qa::pattern(100,1)),"current");marker(current,L"multitap-client.json","Multitap","1.0.6");
  check(qa::makeDirectory(leftover)&&qa::writeBytes(leftover+L"\\Multitap.exe",qa::pattern(100,4)),"leftover");marker(leftover,L"multitap-client.json","Multitap","1.0.5");
  check(ownedClientTarget(current+L"\\Multitap.exe",clientShortcutRoots(roots))&&ownedClientTarget(leftover+L"\\Multitap.exe",clientShortcutRoots(roots))&&!ownedClientTarget(current+L"\\Other.exe",clientShortcutRoots(roots)),"fixed and earlier layouts owned");++checks;
  // A desktop shortcut that still starts the earlier layout's Client\1.0.5.
  const auto oldTarget=leftover+L"\\Multitap.exe";
  Shortcut old;old.path=roots.desktop+L"\\Multitap.lnk";old.boundary=current;old.target=oldTarget;old.icon=oldTarget;old.directory=leftover;
  check(WriteShortcut(old).ok,"old shortcut");const auto original=*qa::readBytes(old.path);
  check(clientShortcutPreflight(roots).empty(),"earlier marked layout preflight");++checks;
  Server channel(roots,"1.0.6");check(qa::feed(channel,"-6|shortcut-preflight-v1|client|1.0.6\n")==std::vector<std::string>{"payload-v1|ok\n"},"native preflight protocol");++checks;
  check(qa::feed(channel,"-6|shortcut-v1|client|1.0.6\n")==std::vector<std::string>{"payload-v1|ok\n"},"earlier shortcut upgrade");check(sameText(qa::readLink(old.path)->target,current+L"\\Multitap.exe"),"new target");++checks;
  check(qa::feed(channel,"-6|shortcut-restore-v1|client\n")==std::vector<std::string>{"payload-v1|ok\n"}&&*qa::readBytes(old.path)==original,"exact rollback");++checks;
  marker(leftover,L"multitap-client.json","OtherProduct","1.0.5");check(!clientShortcutPreflight(roots).empty()&&*qa::readBytes(old.path)==original,"foreign marker refused");++checks;
  marker(leftover,L"multitap-client.json","Multitap","1.0.4");check(!clientShortcutPreflight(roots).empty(),"wrong version refused");++checks;
  marker(leftover,L"multitap-client.json","Multitap","1.0.5");
  check(!clientOwnershipText("garbage {\"product\":\"Multitap\",\"version\":\"1.0.5\"}","Multitap","1.0.5")&&!clientOwnershipText("{\"product\":\"Multitap\",\"product\":\"Other\",\"version\":\"1.0.5\"}","Multitap","1.0.5"),"ambiguous marker refused");++checks;
  const auto hardlink=leftover+L"\\alias.exe";check(CreateHardLinkW(hardlink.c_str(),oldTarget.c_str(),nullptr)!=FALSE,"hardlink");check(!clientShortcutPreflight(roots).empty(),"hardlinked old executable refused");check(DeleteFileW(hardlink.c_str()),"remove fixture hardlink");++checks;
  const auto perUser=roots.localAppData+L"\\Multitap\\Client",outside=tree/L"Outside",linked=perUser+L"\\1.0.4";check(qa::makeDirectory(perUser)&&qa::makeDirectory(outside)&&qa::writeBytes(outside+L"\\Multitap.exe",qa::pattern(100,3)),"outside");marker(outside,L"multitap-client.json","Multitap","1.0.4");check(qa::makeJunction(linked,outside),"junction");check(!ownedClientTarget(linked+L"\\Multitap.exe",clientShortcutRoots(roots)),"junction target refused");++checks;
  const auto alias=roots.desktop+L"\\alias.lnk";check(CreateHardLinkW(alias.c_str(),old.path.c_str(),nullptr),"link hardlink");check(!clientShortcutPreflight(roots).empty(),"hardlinked shortcut refused");check(DeleteFileW(alias.c_str()),"remove link hardlink");++checks;
  check(DeleteFileW(old.path.c_str()),"remove fixture shortcut");
  const auto setup=roots.source+L"\\Setup.exe";check(qa::writeBytes(setup,qa::pattern(200,7)),"setup");
  {auto actual=install::verifiedFile(install::modulePath(),false);install::verifyMappedModule(actual.get());auto fake=install::verifiedFile(setup,false);refused([&]{install::verifyMappedModule(fake.get());},"on-disk replacement cannot become the native uninstaller");++checks;}
  refused([&]{install::verifyRemovalWorker(roots,GetCurrentProcess(),nullptr);},"untrusted child path/parent cannot trigger removal");++checks;
  install::Registration reg{HKEY_CURRENT_USER,L"SOFTWARE\\Multitap-QA-"+randomTag(),true};
  struct Cleanup{install::Registration& reg;~Cleanup(){RegDeleteTreeW(reg.hive,reg.key.c_str());}}cleanup{reg};
  {const auto foreign=install::rootPath(roots)+L"\\Uninstall.exe";const auto bytes=qa::pattern(20,73);check(qa::writeBytes(foreign,bytes),"foreign remover fixture");refused([&]{install::registerInstalled(roots,"1.0.6",setup,reg);},"unregistered existing maintenance executable refused");check(*qa::readBytes(foreign)==bytes&&DeleteFileW(foreign.c_str()),"foreign remover preserved");++checks;}
  install::registerInstalled(roots,"1.0.6",setup,reg);
  auto plan=install::preflight(roots,reg,"1.0.6");check(plan.versions.size()==1&&plan.versions[0]==L"1.0.5","registered fixed install and the leftover version folder");++checks;
  {install::Key key;check(RegOpenKeyExW(reg.hive,reg.key.c_str(),0,KEY_READ|KEY_WRITE,&key.value)==ERROR_SUCCESS,"open key");check(install::registryText(key.value,L"DisplayName")==L"Multitap"&&install::registryText(key.value,L"UninstallString")==L"\""+install::rootPath(roots)+L"\\Uninstall.exe\" --uninstall","ARP display and native command");install::registryText(key.value,L"MultitapProduct",L"foreign");}
  refused([&]{install::registerInstalled(roots,"1.0.6",setup,reg);},"foreign ARP replacement refused");++checks;
  {install::Key key;check(RegOpenKeyExW(reg.hive,reg.key.c_str(),0,KEY_READ|KEY_WRITE,&key.value)==ERROR_SUCCESS,"reopen key");install::registryText(key.value,L"MultitapProduct",L"client");}
  refused([&]{install::preflight(roots,reg,"1.0.5");},"stale uninstaller version refused");++checks;
  {const auto uninstaller=install::rootPath(roots)+L"\\Uninstall.exe";const auto originalBytes=*qa::readBytes(uninstaller);check(qa::writeBytes(setup,qa::pattern(300,71)),"next setup");refused([&]{install::registerInstalled(roots,"1.0.6",setup,reg,[]{throw Failure{"injected_registry_failure"};});},"registration failure refused");check(*qa::readBytes(uninstaller)==originalBytes&&install::preflight(roots,reg,"1.0.6").uninstaller==plan.uninstaller,"registration failure restores exact prior binary and ARP");++checks;}
  const auto personal=perUser+L"\\settings.json",unknown=roots.programFiles+L"\\Multitap\\UserNotes";check(qa::writeBytes(personal,qa::pattern(20,8))&&qa::makeDirectory(unknown)&&qa::writeBytes(unknown+L"\\keep.txt",qa::pattern(10,9)),"preserved fixtures");const auto profile=*qa::readBytes(personal);
  {Handle busy(CreateFileW((current+L"\\Multitap.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));check(bool(busy),"busy file fixture");refused([&]{install::removeInstalled(roots,reg,plan);},"locked file fails before any version deletion");check(plainFile(current+L"\\multitap-client.json")&&plainFile(current+L"\\Multitap.exe"),"busy refusal preserves installation");++checks;}
  install::removeInstalled(roots,reg,plan);check(!plainFile(current+L"\\Multitap.exe")&&!plainFile(current+L"\\multitap-client.json")&&!plainFile(leftover+L"\\Multitap.exe")&&*qa::readBytes(personal)==profile&&plainFile(unknown+L"\\keep.txt"),"only the owned Client removed; profile and unknown folders preserved");++checks;
  install::Key absent;check(RegOpenKeyExW(reg.hive,reg.key.c_str(),0,KEY_READ,&absent.value)==ERROR_FILE_NOT_FOUND,"ARP removed");++checks;
  {const auto directory=install::rootPath(roots);check(qa::writeBytes(directory+L"\\Multitap.exe",qa::pattern(100,83)),"unregister failure fixture");marker(directory,L"multitap-client.json","Multitap","1.0.7");install::registerInstalled(roots,"1.0.7",setup,reg);const auto retryPlan=install::preflight(roots,reg,"1.0.7");refused([&]{install::removeInstalled(roots,reg,retryPlan,[]{throw Failure{"injected_registry_delete_failure"};});},"unregister failure is explicit partial removal");check(install::fileDigest(install::rootPath(roots)+L"\\Uninstall.exe",false)==retryPlan.uninstaller,"failed unregister restores exact verified native remover");install::checkRegistration(reg,roots,true);check(*qa::readBytes(personal)==profile,"unregister failure preserves profile");++checks;}
  std::cout<<"{\"kind\":\"native-client-installation\",\"checks\":"<<checks<<",\"realProductPathsTouched\":false,\"realServicesTouched\":false,\"isolatedHKCUOnly\":true}\n";return 0;
}catch(const Failure& f){std::cerr<<f.code<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
