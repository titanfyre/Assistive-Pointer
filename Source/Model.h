#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <array>
#include <string>
namespace Pointer
{
inline bool Matches(const std::string &chosen, const std::string &actual)
{
    return !chosen.empty() && chosen == actual;
}
struct Settings
{
    double gain = 1;
    unsigned lock = 0, threshold = 0, debounce = 0;
};
struct State
{
    double residualX = 0, residualY = 0;
    std::int64_t heldX = 0, heldY = 0;
    std::array<std::uint64_t, 5> lastDown{};
    std::array<bool, 5> suppressed{};
    std::array<bool, 5> acceptedHeld{};
    void Clear()
    {
        *this = {};
    }
    static std::int32_t Scale(std::int32_t raw, double gain, double &residual)
    {
        const double value = static_cast<double>(raw) * gain + residual;
        const double bounded = std::clamp(value, static_cast<double>(INT32_MIN), static_cast<double>(INT32_MAX));
        const auto result = static_cast<std::int32_t>(bounded);
        residual = bounded - static_cast<double>(result);
        return result;
    }
    static Settings Validate(Settings s)
    {
        if (!std::isfinite(s.gain))
            s.gain = 1;
        s.gain = std::clamp(s.gain, 0.05, 4.0);
        s.lock = std::min(s.lock, 2u);
        s.threshold = std::min(s.threshold, 8u);
        s.debounce = std::min(s.debounce, 250u);
        return s;
    }
    void Apply(std::int32_t &x, std::int32_t &y, std::uint16_t &buttons, std::uint16_t flags, std::uint64_t now,
               Settings settings)
    {
        const auto s = Validate(settings);
        if (flags & 1u)
            return;
        auto update = [&](std::int32_t raw, double &residual, std::int64_t &held, bool locked) {
            if (locked)
            {
                residual = 0;
                held = 0;
                return std::int32_t{0};
            }
            const auto value = Scale(raw, s.gain, residual);
            held += value;
            if (!s.threshold || std::abs(held) >= static_cast<std::int64_t>(s.threshold))
            {
                const auto result = static_cast<std::int32_t>(
                    std::clamp(held, static_cast<std::int64_t>(INT32_MIN), static_cast<std::int64_t>(INT32_MAX)));
                held -= result;
                return result;
            }
            return std::int32_t{0};
        };
        x = update(x, residualX, heldX, s.lock == 2);
        y = update(y, residualY, heldY, s.lock == 1);
        for (unsigned i = 0; i < 5; ++i)
        {
            const auto down = static_cast<std::uint16_t>(1u << (i * 2));
            const auto up = static_cast<std::uint16_t>(down << 1);
            if (buttons & down)
            {
                const bool tooSoon =
                    s.debounce && lastDown[i] && now >= lastDown[i] && now - lastDown[i] < s.debounce;
                if (acceptedHeld[i])
                {
                    if(s.debounce)buttons=static_cast<std::uint16_t>(buttons&~down);
                }
                else if(suppressed[i])
                {
                    buttons=static_cast<std::uint16_t>(buttons&~down);
                }
                else if (tooSoon)
                {
                    buttons = static_cast<std::uint16_t>(buttons & ~down);
                    suppressed[i] = true;
                }
                else
                {
                    lastDown[i] = now ? now : 1;
                    suppressed[i] = false;
                    acceptedHeld[i]=true;
                }
            }
            if (buttons & up)
            {
                if (suppressed[i])
                    buttons = static_cast<std::uint16_t>(buttons & ~up);
                suppressed[i] = false;
                acceptedHeld[i]=false;
            }
        }
    }
};
} // namespace Pointer
