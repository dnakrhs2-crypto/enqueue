#pragma once
#include <string>
#include <vector>

namespace gocue::remote
{
struct NetworkAddress
{
    std::string adapter, name, ip;
    bool recommended = false;
};
std::vector<NetworkAddress> networkAddresses();
std::string randomKey();
}
