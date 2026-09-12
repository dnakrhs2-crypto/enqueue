#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace gocue::recorder
{
// Owner-thread policy. destroy must stop/join the old presenter before create
// allocates a new device, swapchain and upload textures. CPU capture stays live.
class PreviewRecovery
{
public:
    static bool deviceLost(const std::string&);
    void lost(const std::string& error, std::int64_t now, const std::function<void()>& destroy);
    bool retry(std::int64_t now, const std::function<bool()>& create);
    void reset() { pending = false; attempts = 0; ++generation; }
    bool recovering() const noexcept { return pending; }
    bool exhausted() const noexcept { return attempts >= 3 && !pending; }
    std::uint64_t epoch() const noexcept { return generation; }
private:
    bool pending = false;
    unsigned attempts = 0;
    std::int64_t next = 0;
    std::uint64_t generation = 1;
};
}
