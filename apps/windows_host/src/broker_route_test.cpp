#include "broker_route.hpp"
#include <iostream>
#include <stdexcept>
using namespace host;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
int main() {
  try {
    int passed = 0;
    for (const char* good : {"192.0.2.101", "0.0.0.0", "255.255.255.255", "198.51.100.1"})
      require(brokerRouteAddress(good).value_or(L"") == std::wstring(good, good + std::char_traits<char>::length(good)), "A valid address was refused");
    ++passed;
    for (const char* bad : {"", "vdi.example.com", "192.0.2", "192.0.2.101.1", "192.0.2.256", "192.0.2.101:443", "0192.0.2.101", "192..2.101", ".192.0.2", "192.0.2.", " 192.0.2.101", "1234.1.1.1", "::1"})
      require(!brokerRouteAddress(bad), "An address that is not a plain IPv4 literal passed");
    ++passed;
    BrokerRoute route;
    require(route.current(0).empty() && !route.offer(""), "A route existed before any offer");
    require(route.offer("192.0.2.101") && route.current(5) == L"192.0.2.101", "An offer was not taken");
    require(!route.offer("192.0.2.101"), "The same offer counted as a change");
    ++passed;
    route.failed(1000);
    require(route.current(1000).empty() && route.current(1000 + BrokerRoute::coolDownMs - 1).empty(), "A failed route was used inside the cool-down");
    require(route.current(1000 + BrokerRoute::coolDownMs) == L"192.0.2.101", "The route did not come back after the cool-down");
    ++passed;
    route.failed(2000);
    require(route.offer("192.0.2.102") && route.current(2001) == L"192.0.2.102", "A new address waited out the old one's cool-down");
    require(route.offer("not an address") && route.current(2002).empty(), "An invalid offer kept the old route");
    ++passed;
    std::cout << "Broker route checks passed: " << passed << '\n';
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
