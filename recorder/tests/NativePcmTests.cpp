#include "audio/NativePcmConverter.h"
#include "audio/RawAudioTap.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace gocue::recorder;
namespace
{
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejects(Function f)
{
    bool rejected = false; try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid prepare must reject");
}
void writeWord(std::uint8_t* p, std::uint32_t word, NativeFormat format)
{
    for (std::uint32_t b = 0; b < format.containerBytes; ++b)
    { const auto index = format.byteOrder == NativeByteOrder::little ? b : format.containerBytes - 1 - b; p[index] = static_cast<std::uint8_t>(word); word >>= 8; }
}
std::array<std::uint8_t, 3> expected24(std::int32_t value)
{
    const auto bits = static_cast<std::uint32_t>(value);
    return {static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8), static_cast<std::uint8_t>(bits >> 16)};
}
void checkSamples(NativeFormat f, const std::vector<std::int64_t>& values, const std::vector<std::int32_t>& expected)
{
    std::vector<std::uint8_t> native(values.size() * f.strideBytes, 0xa5), packed(values.size() * 3, 0xa5), oracle;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        auto word = static_cast<std::uint32_t>(values[i]);
        if (f.alignment == NativeAlignment::mostSignificant) word <<= f.containerBytes * 8 - f.validBits;
        writeWord(native.data() + i * f.strideBytes, word, f);
        const auto bytes = expected24(expected[i]); oracle.insert(oracle.end(), bytes.begin(), bytes.end());
    }
    const auto result = NativePcmConverter::pack(f, native.data(), native.size(), static_cast<std::uint32_t>(values.size()), packed.data(), packed.size());
    require(result && result.samplesWritten == values.size() && packed == oracle, "native fixture bytes equal PCM24 oracle");
}
void checkFloat(NativeFormat f, const std::vector<float>& values, const std::vector<std::int32_t>& expected)
{
    std::vector<std::int64_t> words;
    for (auto v : values) { std::uint32_t word = 0; std::memcpy(&word, &v, sizeof(word)); words.push_back(word); }
    checkSamples(f, words, expected);
}
std::uint32_t prbs(std::uint32_t& state) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
}
int runNativePcmTests()
{
    int passed = 0, failed = 0;
    auto test = [&](const char* name, auto fn)
    {
        try { fn(); ++passed; }
        catch (const std::exception& e) { ++failed; std::cerr << "PCM FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("explicit PCM24 zero max min 1 LSB and negative bytes, both byte orders", []
    {
        const std::vector<std::int64_t> source{0, 8388607, -8388608, 1, -1, -1234567};
        const std::vector<std::int32_t> target{0, 8388607, -8388608, 1, -1, -1234567};
        checkSamples(nativeFormatForAsio(1), source, target); checkSamples(nativeFormatForAsio(17), source, target);
        require(expected24(-1) == std::array<std::uint8_t, 3>{0xff,0xff,0xff}, "literal negative bytes");
        require(expected24(-8388608) == std::array<std::uint8_t, 3>{0,0,0x80}, "literal minimum bytes");
        require(expected24(8388607) == std::array<std::uint8_t, 3>{0xff,0xff,0x7f}, "literal maximum bytes");
    });
    test("signed16 multiplied by 256, packed and 32-bit containers", []
    {
        for (int type : {0,16,8,24}) checkSamples(nativeFormatForAsio(type), {0,32767,-32768,1,-1,-129}, {0,8388352,-8388608,256,-256,-33024});
    });
    test("32-bit integer nearest rounding ties away from zero and saturation", []
    {
        for (int type : {2,18}) checkSamples(nativeFormatForAsio(type),
            {0,2147483647,-2147483648ll,1,-1,127,128,129,-127,-128,-129,256,-256},
            {0,8388607,-8388608,0,0,0,1,1,0,-1,-1,1,-1});
    });
    test("ASIO aligned 18/20/24 values and sign extension", []
    {
        for (int type : {9,10,11,25,26,27})
        {
            const auto f = nativeFormatForAsio(type); const auto scale = 1 << (24 - f.validBits), half = 1 << (f.validBits - 1);
            checkSamples(f, {0,half-1,-half,1,-1,-17}, {0,8388608-scale,-8388608,scale,-scale,-17*scale});
        }
    });
    test("explicit left/right alignment independent from endian and padded stride", []
    {
        for (int type : {8,9,10,11,24,25,26,27})
        {
            auto f = nativeFormatForAsio(type); f.alignment = NativeAlignment::mostSignificant; f.strideBytes = 8;
            const auto scale = 1 << (24 - f.validBits); checkSamples(f, {0,1,-1,71,-71}, {0,scale,-scale,71*scale,-71*scale});
        }
    });
    test("float32 both byte orders zero edges 1 LSB tie rounding saturation", []
    {
        const float lsb = 1.0f / 8388608.0f;
        for (int type : {3,19}) checkFloat(nativeFormatForAsio(type),
            {0,-0.0f,1,-1,lsb,-lsb,0.5f*lsb,-0.5f*lsb,2,-2,std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()},
            {0,0,8388607,-8388608,1,-1,1,-1,8388607,-8388608,8388607,-8388608});
    });
    test("NaN and infinity reject entire block without partial output", []
    {
        for (int type : {3,19}) for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
        {
            auto f = nativeFormatForAsio(type); std::array<std::uint8_t, 12> native{}; std::array<std::uint8_t, 9> out; out.fill(0xa5);
            std::uint32_t word = 0; std::memcpy(&word, &bad, 4); writeWord(native.data() + 4, word, f);
            auto r = NativePcmConverter::pack(f, native.data(), native.size(), 3, out.data(), out.size());
            require(r.error == PcmConversionError::nonFinite && r.errorSample == 1 && r.samplesWritten == 0, "nonfinite reports exact index");
            require(std::all_of(out.begin(), out.end(), [](auto b) { return b == 0xa5; }), "whole destination untouched");
        }
    });
    test("unsupported, truncated, invalid stride, empty buffers", []
    {
        std::array<std::uint8_t, 12> input{}, output{}; auto f = nativeFormatForAsio(17);
        for (int type : {4,20,32,33,40,-1,999}) require(!NativePcmConverter::supports(nativeFormatForAsio(type)), "float64/DSD/unknown reject");
        require(NativePcmConverter::pack(f, input.data(), 2, 1, output.data(), 3).error == PcmConversionError::invalidBuffer, "truncated source");
        require(NativePcmConverter::pack(f, input.data(), 3, 1, output.data(), 2).error == PcmConversionError::invalidBuffer, "short destination");
        require(static_cast<bool>(NativePcmConverter::pack(f, nullptr, 0, 0, nullptr, 0)), "zero-length valid");
        f.strideBytes = 2; require(!NativePcmConverter::supports(f), "short stride");
    });
    test("8 distinct channel PRBS sequences through every supported ASIO format", []
    {
        for (int type : {0,1,2,3,8,9,10,11,16,17,18,19,24,25,26,27}) for (std::uint32_t c = 0; c < 8; ++c)
        {
            auto f = nativeFormatForAsio(type); std::uint32_t state = 0x19283741u + c * 0x67452301u;
            std::vector<std::int64_t> source; std::vector<std::int32_t> expected; std::vector<float> floats;
            for (int i = 0; i < 257; ++i)
            {
                const auto p24 = static_cast<std::int32_t>(prbs(state) & 0x00ffffffu) - 8388608;
                if (f.encoding == NativeEncoding::ieeeFloat) { floats.push_back(static_cast<float>(p24) / 8388608.0f); expected.push_back(p24); }
                else
                {
                    const auto scale = std::ldexp(1.0, static_cast<int>(f.validBits) - 24);
                    const auto value = static_cast<std::int64_t>(std::trunc(p24 * scale)); source.push_back(value);
                    expected.push_back(static_cast<std::int32_t>(std::llround(value / scale)));
                }
            }
            if (f.encoding == NativeEncoding::ieeeFloat) checkFloat(f, floats, expected); else checkSamples(f, source, expected);
        }
    });
    test("8-channel native tap owns bytes and noncontiguous physical map", []
    {
        RawAudioTap tap; const std::vector<int> mapping{1,3,4,7,8,9,12,15}; tap.prepare(mapping, 32, 2);
        std::array<std::array<std::uint8_t, 96>, 8> inputs{}; std::array<NativeInputView, 8> views{};
        for (std::size_t c = 0; c < 8; ++c)
        {
            for (std::size_t b = 0; b < 96; ++b) inputs[c][b] = static_cast<std::uint8_t>(c * 31 + b);
            views[c] = {inputs[c].data(), static_cast<int>(c), mapping[c], nativeFormatForAsio(17)};
        }
        BlockStamp stamp{}; stamp.numSamples = 32; stamp.samplePosition = 12345;
        require(tap.onAsioBlock(stamp, views.data(), 8), "first raw block");
        for (auto& input : inputs) input.fill(0); // driver reuses its memory immediately
        const auto* copied = tap.front(); require(copied && copied->stamp.samplePosition == 12345, "stamp preserved");
        for (std::size_t c = 0; c < 8; ++c)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(copied->channels[c].data);
            require(bytes != inputs[c].data() && copied->channels[c].physicalIndex == mapping[c], "owned channel mapping");
            for (std::size_t b = 0; b < 96; ++b) require(bytes[b] == static_cast<std::uint8_t>(c * 31 + b), "copied fixture bytes");
        }
        require(tap.onAsioBlock(stamp, views.data(), 8) && !tap.onAsioBlock(stamp, views.data(), 8), "raw queue upper bound");
        require(tap.overflows() == 1 && tap.highWater() == 2, "overflow reported"); tap.release(); tap.release();
        require(tap.front() == nullptr && tap.onAsioBlock(stamp, views.data(), 8), "ring reuses released slot"); tap.release();
        stamp.numSamples = 33; require(!tap.onAsioBlock(stamp, views.data(), 8), "max block fixed");
        stamp.numSamples = 32; views[3].physicalIndex = 6; require(!tap.onAsioBlock(stamp, views.data(), 8), "map changed rejected");
        require(tap.invalidBlocks() == 2, "invalid block count");
    });
    test("prepare bounds and callback queue copies padded source", []
    {
        RawAudioTap tap; rejects([&] { tap.prepare({1,1}, 32, 2); }); rejects([&] { tap.prepare({3,1}, 32, 2); });
        tap.prepare({0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, 32, 2);
        rejects([&] { tap.prepare({0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, 32, 2); }); rejects([&] { tap.prepare({0}, 0, 2); });
        rejects([&] { tap.prepare({0}, 262144, 65536); });
        tap.prepare({5}, 2, 1); auto f = nativeFormatForAsio(17); f.strideBytes = 8;
        const std::array<std::uint8_t, 11> native{1,0,0,0xaa,0xaa,0xaa,0xaa,0xaa,0xff,0xff,0xff};
        NativeInputView view{native.data(),0,5,f}; BlockStamp s{}; s.numSamples = 2;
        require(tap.onAsioBlock(s, &view, 1), "padded native stride"); std::array<std::uint8_t, 6> out{};
        const auto result = NativePcmConverter::pack(f, tap.front()->channels[0].data, 11, 2, out.data(), out.size());
        require(result && out == std::array<std::uint8_t, 6>{1,0,0,0xff,0xff,0xff}, "no read beyond last sample"); tap.release();
    });
    test("concurrent raw producer and worker preserve FIFO bytes", []
    {
        RawAudioTap tap; tap.prepare({3}, 1, 7); std::atomic<bool> done{false};
        std::uint64_t accepted = 0, consumed = 0, corrupt = 0;
        std::thread worker([&]
        {
            auto drain = [&]
            {
                while (const auto* b = tap.front())
                {
                    std::uint32_t word = 0; std::memcpy(&word, b->channels[0].data, 4);
                    if (word != b->stamp.sequence) ++corrupt;
                    ++consumed; tap.release();
                }
            };
            while (!done.load(std::memory_order_acquire)) { drain(); std::this_thread::yield(); } drain();
        });
        for (std::uint32_t i = 0; i < 30000; ++i)
        { NativeInputView view{&i,0,3,nativeFormatForAsio(18)}; BlockStamp s{}; s.numSamples = 1; s.sequence = i; accepted += tap.onAsioBlock(s, &view, 1); }
        done.store(true, std::memory_order_release); worker.join();
        require(!corrupt && consumed == accepted && accepted + tap.overflows() == 30000, "raw SPSC lifetime/accounting");
    });
    std::cout << "NativePcmTests: " << passed << " passed, " << failed << " failed; no device opened\n";
    return failed ? 1 : 0;
}
