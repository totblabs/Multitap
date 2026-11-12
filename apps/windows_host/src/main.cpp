#include "settings_ui.hpp"
#include <iostream>
#include "tray.hpp"
#include "settings_rpc.hpp"
#include "windows_autologon_hooks.hpp"
#include "host_removal_native.hpp"
#include "signin_worker_command.hpp"
#include "signin_preparation.hpp"
#include "signin_restoration.hpp"
#include "signin_diagnostic_client.hpp"
#include "secure_desktop_entry.hpp"
#include "system_input_entry.hpp"
#include "ime_state_native.hpp"
namespace host::drive::fs { int driveFsHelperEntry(); }
using namespace host;
namespace {
int run(int argc,wchar_t** argv){
  try {
    // Fixed private child entry. This path never opens settings or constructs
    // the ordinary Runtime before native service/desktop authorization.
    if(argc>=2&&std::wstring(argv[1])==L"--signin-worker-v1")return signin::entry::workerCommand(argc,argv);
    if(argc>=2&&std::wstring(argv[1])==L"--signin-preparer-v1")return signin::preparation::workerCommand(argc,argv);
    if(argc>=2&&std::wstring(argv[1])==L"--signin-restorer-v1")return signin::restoration::workerCommand(argc,argv);
    // Role::secure: the SYSTEM helper that streams a UAC prompt of this unlocked
    // session (secure_desktop_entry.hpp). Launched by the service on winsta0\Winlogon.
    if(argc>=2&&std::wstring(argv[1])==L"--secure-desktop-helper-v1")return secure::entry::command(argc,argv);
    // System input: the SYSTEM injector of a stream whose worker cannot reach elevated windows
    // (system_input.hpp). Launched by the service on winsta0\Default.
    if(argc>=2&&std::wstring(argv[1])==L"--system-input-v1")return sysinput::entry::command(argc,argv);
    if(argc>=2&&std::wstring(argv[1])==L"--signin-diagnostic-prepare-v1"){
      const auto request=signin::diagnostic::client::parseCommand(argc,argv);
      if(!request)return ERROR_INVALID_PARAMETER;
      const auto result=signin::diagnostic::client::requestPreparation(request->width,request->height);
      const bool ok=result.observedResult&&result.code==signin::diagnostic::client::Code::prepared;
      std::cout<<"{\"type\":\"signin.diagnostic.tool-result\",\"protocol\":\"signin-diagnostic-inlet-v1\",\"ok\":"<<(ok?"true":"false")
        <<",\"code\":"<<uint32_t(result.code)<<",\"confirmationAttempted\":"<<(result.confirmationAttempted?"true":"false")
        <<",\"observedResult\":"<<(result.observedResult?"true":"false")<<",\"runtimeSigninQualified\":false}\n";
      return ok?0:ERROR_NOT_READY;
    }
    if(argc==2&&std::wstring(argv[1])==L"--settings-rpc-stdin")return settingsRpc();
    if(argc==2&&std::wstring(argv[1])==L"--windows-autologon-startup-recovery")return autologon::startupRecoveryCommand();
    if(argc==2&&std::wstring(argv[1])==L"--windows-autologon-update-journal-check")return autologon::updateJournalCheckCommand();
    // Windows Installer runs this while it removes the Host (host_removal.hpp).
    if(argc>=2&&std::wstring(argv[1])==L"--product-removal-cleanup")return removal::command(argc,argv);
    if(argc==2&&std::wstring(argv[1])==L"--windows-autologon-uninstall-cleanup")return autologon::uninstallCleanupCommand(false);
    if(argc==3&&std::wstring(argv[1])==L"--windows-autologon-uninstall-cleanup"&&std::wstring(argv[2])==L"--confirmed")return autologon::uninstallCleanupCommand(true);
    // drive-v1: started by the service with the session user's own token.
    if(argc==2&&std::wstring(argv[1])==L"--drive-mount")return drive::driveMountHelper();
    // WinFsp backend: the user-session file system helper (stdin/stdout are pipes to the service).
    if(argc==2&&std::wstring(argv[1])==L"--drive-fs")return drive::fs::driveFsHelperEntry();
    // print-v1 default printer: started by the service with the session user's own token.
    if(argc==2&&std::wstring(argv[1])==L"--print-default")return printDefaultHelper();
    // ime-state-v1: the console session's 한/영 mode on the service's question, started with the session user's own token.
    if(argc==2&&std::wstring(argv[1])==L"--ime-state")return ime::helperEntry();
    if(argc>=2&&std::wstring(argv[1])==L"--service"){std::wstring name=L"MultitapHost";bool qa=false;for(int i=2;i<argc;i++){if(std::wstring(argv[i])==L"--name"&&i+1<argc)name=argv[++i];else if(std::wstring(argv[i])==L"--qa")qa=true;else throw std::runtime_error("Invalid service argument");}return runService(name,qa);}
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // thumbnail-capture-v1: one launcher picture, started by the service with the session user's own token.
    if(argc==2&&std::wstring(argv[1])==L"--thumbnail-capture")return thumbnailCaptureHelper();
    if(argc==6&&std::wstring(argv[1])==L"--worker"&&std::wstring(argv[2])==L"--pipe"&&std::wstring(argv[4])==L"--service-pid"){Runtime runtime;return interactiveWorker(argv[3],DWORD(_wtol(argv[5])));}
    if(argc==3&&(std::wstring(argv[1])==L"--encode-selftest"||std::wstring(argv[1])==L"--capture-selftest")){int count=encodeSelfTest(argv[2],std::wstring(argv[1])==L"--capture-selftest");std::cout<<"Encoded "<<count<<" actual software H264 frames.\n";return 0;}
    if(argc==2&&(std::wstring(argv[1])==L"--tray"||std::wstring(argv[1])==L"--tray-selftest")){HostTray tray;return tray.run(std::wstring(argv[1])==L"--tray-selftest");}
    Runtime runtime;
    if((argc==3||argc==4)&&std::wstring(argv[1])==L"--settings-selftest"){const double scale=argc==4?_wtof(argv[3]):0;if(scale<0||scale>2.5)throw std::runtime_error("Invalid QA layout scale");SettingsUi ui;const int result=ui.run(argv[2],scale);std::cout<<"{\"kind\":\"real-settings-window\",\"testPassedThenEditInvalidated\":"<<(result==0?"true":"false")<<",\"layoutScale\":"<<scale<<",\"registrationAttempted\":false}\n";return result;}
    if(argc==2&&std::wstring(argv[1])==L"--enroll-stdin"){requireAdmin();std::string bytes((std::istreambuf_iterator<char>(std::cin)),{});const auto request=JsonParser::parse(bytes);SecureZeroMemory(bytes.data(),bytes.size());const auto origin=wide(request.get("serverOrigin").string());const auto consent=wide(request.get("httpConsentOrigin").string());testBroker(origin,consent);std::string code=request.get("code").string();auto value=enroll(origin,code,consent);SecureZeroMemory(code.data(),code.size());savePublicEnrollment(value);reloadHostService();std::cout<<"Enrolled VM; machine credential protected for the Windows host service.\n";return 0;}
    if(argc==2&&std::wstring(argv[1])==L"--diagnostics"){const auto settings=readPublicConfig();const auto display=virtualDisplayStatus();std::cout<<"{\"version\":\"" MULTITAP_HOST_VERSION_UTF8 "\",\"machineRegistrationConfigured\":"<<(!settings.get("desktopId").string().empty()?"true":"false")<<",\"service\":"<<jsonQuote(utf8(serviceLabel(readHostServiceSnapshot(),ui_text::Language::korean)))<<",\"interactiveSession\":"<<(sessionUnlocked(interactiveSession())?"true":"false")<<",\"virtualDisplayReady\":"<<(display.ready()?"true":"false")<<"}\n";return 0;}
    if(argc==3&&std::wstring(argv[1])==L"--test-server"){std::cout<<"Compatible Multitap broker "<<testBroker(argv[2])<<"; TLS verified.\n";return 0;}
#ifdef HOST_CONSOLE_ENTRY
    std::cout<<"Multitap Host " MULTITAP_HOST_VERSION_UTF8 ": --diagnostics | --test-server https://... | --enroll-stdin | --service [--name NAME] [--qa] | --encode-selftest path | --capture-selftest path\n";return argc>1?1:0;
#else
    if(argc>1&&std::wstring(argv[1])!=L"--configure")throw std::runtime_error("Unknown host argument");
    const auto directory=std::filesystem::path(executablePath()).parent_path();const auto settings=directory/L"MultitapHostSettings.exe";
    if(!std::filesystem::is_regular_file(settings))throw std::runtime_error("Host settings executable is missing. Reinstall the Multitap Host package.");
    SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_FLAG_NO_UI;launch.lpVerb=L"runas";launch.lpFile=settings.c_str();launch.lpParameters=L"--host-settings";launch.lpDirectory=directory.c_str();launch.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&launch)){const DWORD error=GetLastError();if(error==ERROR_CANCELLED)return ERROR_CANCELLED;SetLastError(error);winCheck(FALSE,"Launch administrator host settings");}return 0;
#endif
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';
#ifndef HOST_CONSOLE_ENTRY
    MessageBoxW(nullptr,wide(error.what()).c_str(),L"Multitap Host",MB_OK|MB_ICONERROR);
#endif
    return 1;
  }
}
}
#ifdef HOST_CONSOLE_ENTRY
int wmain(int argc,wchar_t** argv){return run(argc,argv);}
#else
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);int result=run(argc,argv);LocalFree(argv);return result;}
#endif
