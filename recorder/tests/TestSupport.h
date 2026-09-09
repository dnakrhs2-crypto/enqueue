#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>

namespace recorder_test
{
inline void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejects(Function run)
{
    bool threw = false; try { run(); } catch (const std::exception&) { threw = true; }
    require(threw, "Expected rejection");
}
struct Suite
{
    unsigned passed = 0, failed = 0;
    void test(const char* name, const std::function<void()>& run)
    {
        try { run(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    int result(const char* name) const
    {
        std::cout << name << ": " << passed << " passed, " << failed << " failed; no capture/NVENC hardware opened\n";
        return failed ? 1 : 0;
    }
};
}
