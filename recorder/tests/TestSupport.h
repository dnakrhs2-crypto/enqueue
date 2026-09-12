#pragma once
#include <atomic>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace juce { class String; }
namespace recorder_test
{
inline std::atomic<unsigned> unexpectedUnhandledExceptions{0};
inline thread_local const char* currentTest = nullptr;
void recordUnhandledException(const std::exception*, const juce::String&, int) noexcept;
class ExpectedUnhandledExceptions
{
public:
    using Observer = std::function<void(const std::exception*, const juce::String&, int)>;
    explicit ExpectedUnhandledExceptions(unsigned count, Observer = {});
    ~ExpectedUnhandledExceptions();
    unsigned calls() const { return observed; }
    ExpectedUnhandledExceptions(const ExpectedUnhandledExceptions&) = delete;
    ExpectedUnhandledExceptions& operator=(const ExpectedUnhandledExceptions&) = delete;
private:
    friend void recordUnhandledException(const std::exception*, const juce::String&, int) noexcept;
    static thread_local ExpectedUnhandledExceptions* active;
    ExpectedUnhandledExceptions* previous;
    unsigned expected, observed = 0;
    Observer observer;
};
inline int processResult(int result) { return result || unexpectedUnhandledExceptions.load() ? 1 : 0; }
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
        const auto before = unexpectedUnhandledExceptions.load();
        struct Context
        {
            const char* previous = currentTest;
            explicit Context(const char* name) { currentTest = name; }
            ~Context() { currentTest = previous; }
        } context(name);
        try
        {
            run();
            require(unexpectedUnhandledExceptions.load() == before, "Unexpected JUCE callback exception (see diagnostic)");
            ++passed; std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
        catch (...) { ++failed; std::cerr << "FAIL " << name << ": unknown non-standard exception\n"; }
    }
    int result(const char* name) const
    {
        std::cout << name << ": " << passed << " passed, " << failed << " failed; no capture/NVENC hardware opened\n";
        return failed ? 1 : 0;
    }
};
}
