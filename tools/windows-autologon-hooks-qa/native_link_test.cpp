// Root-only explicit QA executable. No command line/native calls in main.
#include "../../apps/windows_host/src/windows_autologon_hooks.hpp"
#include <iostream>
// Export retention prevents /OPT:REF from turning a successful link into a
// headers-only test. Do NOT invoke this anchor: it is not the test entry point.
extern "C" __declspec(dllexport) int MultitapAutologonLinkOnlyNeverInvoke() {
  host::autologon::NativeAccess access;
  host::autologon::Controller controller(access);
  return controller.status().administrator ? 1 : 0;
}
int main() {
  std::cout << "{\"kind\":\"windows-autologon-native-link\",\"nativeCalls\":false,\"accountCalls\":false,\"registryCalls\":false,\"lsaCalls\":false}\n";
  return 0;
}
