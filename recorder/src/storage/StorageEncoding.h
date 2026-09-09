#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace gocue::recorder::storageEncoding
{
template<class T> void put(std::uint8_t* destination, T value) noexcept
{
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t i = 0; i < sizeof(T); ++i) destination[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
template<class T> T get(const std::uint8_t* source) noexcept
{
    static_assert(std::is_unsigned_v<T>);
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(source[i]) << (8 * i);
    return value;
}
inline std::uint32_t crc32(const void* data, std::size_t size) noexcept
{
    static const auto table = []
    {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < result.size(); ++i)
        {
            auto c = i;
            for (unsigned bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0u);
            result[i] = c;
        }
        return result;
    }();
    auto crc = 0xffffffffu;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
}
