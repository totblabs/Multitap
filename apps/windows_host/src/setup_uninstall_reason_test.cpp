// setup_uninstall_reason.hpp: which sentence the removal window shows for a
// failure code. Memory only.
#include "setup_uninstall_reason.hpp"
#include <cstdio>

using namespace multitap::installation;
namespace {
unsigned checks=0,failed=0;
void check(bool value,const char* name){++checks;if(!value){++failed;std::printf("FAIL %s\n",name);}}
}

int main(){
  // The checks below pin the Korean source text; English follows at the end.
  multitap::ui::forceLanguageForTest(multitap::ui::Language::korean);
  // Another install, update or removal is running: wait, never "reinstall".
  for(const char* code:{"uninstall_installation_in_progress","uninstall_installer_gate_unavailable"}){
    check(removalFailure(code)==RemovalFailure::otherInstaller,code);
  }
  check(std::wstring(removalFailureText(RemovalFailure::otherInstaller))==L"다른 Multitap 설치나 제거가 진행 중이에요.\n끝난 뒤 다시 제거하세요.","the sentence for a running installer");
  check(std::wstring(removalFailureText(RemovalFailure::otherInstaller)).find(L"다시 설치")==std::wstring::npos,"a running installer is not answered with a reinstall");
  // A program or file of the product is in use: close it and try again.
  for(const char* code:{"uninstall_client_still_running","uninstall_files_in_use","uninstall_shortcut_busy","uninstall_maintenance_busy"}){
    check(removalFailure(code)==RemovalFailure::inUse,code);
  }
  check(removalFailure("uninstall_partial_removal")==RemovalFailure::partial,"uninstall_partial_removal");
  // Codes about this setup's own file or its records keep the general sentence:
  // they are not "the program is in use".
  for(const char* code:{"uninstall_running_image_unverified","uninstall_running_image_replaced","uninstall_running_setup_required","uninstall_remover_restore_failed","uninstall_registration_rollback_failed",
                        "uninstall_installation_unverified","uninstall_installation_changed","uninstall_version_mismatch","uninstall_failed",""}){
    check(removalFailure(code)==RemovalFailure::installation,code);
  }
  // Every sentence is two lines.
  for(const auto kind:{RemovalFailure::installation,RemovalFailure::partial,RemovalFailure::inUse,RemovalFailure::otherInstaller}){
    const std::wstring text=removalFailureText(kind);
    check(!text.empty()&&text.find(L'\n')!=std::wstring::npos&&text.find(L'\n')==text.rfind(L'\n'),"two lines");
  }
  // English (Windows display language not Korean): the same two lines, no Hangul.
  for(const auto kind:{RemovalFailure::installation,RemovalFailure::partial,RemovalFailure::inUse,RemovalFailure::otherInstaller}){
    const std::wstring text=removalFailureText(kind,multitap::ui::Language::english);
    check(!text.empty()&&text.find(L'\n')!=std::wstring::npos&&text.find(L'\n')==text.rfind(L'\n'),"two lines in English");
    check(!multitap::ui::hasHangul(text),"no Hangul in English");
  }
  multitap::ui::forceLanguageForTest(multitap::ui::Language::english);
  check(std::wstring(removalFailureText(RemovalFailure::otherInstaller))==L"Another Multitap install or removal is running.\nRemove it again when that is done.","the process language picks English");
  check(std::wstring(removalFailureText(RemovalFailure::otherInstaller)).find(L"nstall the program again")==std::wstring::npos,"a running installer is not answered with a reinstall in English");
  std::printf("checks=%u failed=%u\n",checks,failed);
  return failed?1:0;
}
