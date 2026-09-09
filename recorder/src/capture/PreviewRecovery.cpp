#include "PreviewRecovery.h"
#include <algorithm>
#include <cctype>

namespace gocue::recorder
{
bool PreviewRecovery::deviceLost(const std::string& message)
{
    auto text = message; std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text.find("887a0005") != std::string::npos || text.find("887a0007") != std::string::npos
        || text.find("887a0006") != std::string::npos || text.find("device_removed") != std::string::npos;
}
void PreviewRecovery::lost(const std::string& error, std::int64_t now, const std::function<void()>& destroy)
{
    destroy(); ++generation;
    pending = deviceLost(error) && attempts < 3; next = now + 500;
}
bool PreviewRecovery::retry(std::int64_t now, const std::function<bool()>& create)
{
    if (!pending || now < next) return false;
    ++attempts;
    if (create()) { pending = false; ++generation; return true; }
    pending = attempts < 3; next = now + 500; return false;
}
}
