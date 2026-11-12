#include "signin_worker_entry.hpp"
#include "signin_worker_command.hpp"
#include "signin_preparation.hpp"
#include "signin_restoration.hpp"
#include "signin_diagnostic_inlet.hpp"
#include "signin_diagnostic_client.hpp"
#include "signin_diagnostic_coordinator.hpp"
#include <iostream>

int main(){
  // Volatile address stores retain both real implementations for link closure.
  // Neither is called: no token, process, pipe, desktop, capture, Encoder or
  // SendInput operation is performed by this qualification executable.
  using Entry=decltype(&host::signin::entry::run);
  using Command=decltype(&host::signin::entry::workerCommand);
  Entry volatile entry=&host::signin::entry::run;
  Command volatile command=&host::signin::entry::workerCommand;
  using Preparation=decltype(&host::signin::preparation::workerCommand);
  Preparation volatile preparation=&host::signin::preparation::workerCommand;
  using Restoration=decltype(&host::signin::restoration::workerCommand);
  Restoration volatile restoration=&host::signin::restoration::workerCommand;
  using Inlet=decltype(&host::signin::diagnostic::Server::run);
  Inlet volatile inlet=&host::signin::diagnostic::Server::run;
  using ToolClient=decltype(&host::signin::diagnostic::client::requestPreparation);
  ToolClient volatile toolClient=&host::signin::diagnostic::client::requestPreparation;
  using Coordinator=decltype(&host::signin::diagnostic::Coordinator::prepare);
  Coordinator volatile coordinator=&host::signin::diagnostic::Coordinator::prepare;
  if(!entry||!command||!preparation||!restoration||!inlet||!toolClient||!coordinator)return 1;
  std::cout<<"{\"kind\":\"signin-entry-production-link-only\",\"entryAddressRetained\":true,\"commandAddressRetained\":true,\"preparationCommandAddressRetained\":true,\"restorationCommandAddressRetained\":true,\"diagnosticInletAddressRetained\":true,\"implementationsInvoked\":false,\"nativeSystemCaptureInputEncoderRuns\":0,\"runtimeQualified\":false}\n";
  return 0;
}
