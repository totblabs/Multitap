# Host transport policy QA

`fixture.cpp` calls the Host's production broker-address policy
(`apps/windows_host/src/broker_origin.hpp`) directly: no copy of the policy, no
Windows API, network, credentials or settings. Its cases cover canonical
HTTPS/HTTP origins with default and explicit ports, IPv4/IPv6, ambiguous input,
exact local consent, session relay paths, scheme and authority, the HTTP
health opt-in and type flags, and HTTPS compatibility with older settings.

Build it standalone:

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/host-transport-qa -B <build> -A x64
& $cmake --build <build> --config Release
& <build>/Release/MultitapHostTransportPolicyTest.exe
```

The Host's own CMake project has the same target, `MultitapHostTransportPolicyTest`,
and `MultitapHostTransportJsonTest` (`src/broker_transport_test.cpp`), which tests
the JSON adapter with memory inputs only. Both are excluded from the default
build; build them by name.

These fixtures check the address policy only. They do not exercise WinHTTP,
TLS, a broker channel or enrolment; never run the Host against real stored
credentials as a substitute.
