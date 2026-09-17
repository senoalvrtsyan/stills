#pragma once
// stills/stills_Time.h — CMTime-like rational time. No FFmpeg dependency.

#include <charconv>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <version>

#include "stills/detail/stills_Config.h"

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
#endif

namespace stills
{

/// Rounding used when converting between timescales (Time::toTimestamp, Time::converted).
enum class TimeRounding : std::uint8_t
{
    nearest, ///< round half away from zero (libav AV_ROUND_NEAR_INF)
    down,    ///< toward negative infinity
    up,      ///< toward positive infinity
    zero,    ///< toward zero
};

namespace detail
{

__extension__ using int128 = __int128;

/// Computes a * num / den with the requested rounding. Requires den > 0.
/// Returns nullopt when the result does not fit in int64_t.
[[nodiscard]] constexpr std::optional<std::int64_t> rescale (std::int64_t a, std::int64_t num, std::int64_t den,
                                                             TimeRounding rounding) noexcept
{
    if (den <= 0) return std::nullopt;
    const int128 p = static_cast<int128> (a) * static_cast<int128> (num);
    const int128 d = static_cast<int128> (den);
    int128 q = p / d; // truncates toward zero
    const int128 rem = p % d;

    if (rem != 0)
    {
        switch (rounding)
        {
        case TimeRounding::zero:
            break;
        case TimeRounding::down:
            if (p < 0) --q;
            break;
        case TimeRounding::up:
            if (p > 0) ++q;
            break;
        case TimeRounding::nearest:
        {
            const int128 twice = (rem < 0 ? -rem : rem) * 2;

            if (twice >= d) q += (p < 0) ? -1 : 1;
            break;
        }
        }
    }

    if (q > static_cast<int128> (std::numeric_limits<std::int64_t>::max())
        || q < static_cast<int128> (std::numeric_limits<std::int64_t>::min()))
    {
        return std::nullopt;
    }

    return static_cast<std::int64_t> (q);
}

/// Compares a/b with c/d (b, d > 0) without overflow. Returns <0, 0, >0.
[[nodiscard]] constexpr int compareRatios (std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d) noexcept
{
    const int128 lhs = static_cast<int128> (a) * static_cast<int128> (d);
    const int128 rhs = static_cast<int128> (c) * static_cast<int128> (b);
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

[[nodiscard]] constexpr std::int64_t gcd64 (std::int64_t a, std::int64_t b) noexcept
{
    while (b != 0)
    {
        const std::int64_t t = a % b;
        a = b;
        b = t;
    }

    return a;
}

} // namespace detail

/// A positive rational, mirroring AVRational without exposing it.
struct Rational
{
    std::int32_t num{ 0 };
    std::int32_t den{ 1 };

    /// Zero when the denominator is zero: an invalid Rational has no value, and this is the one
    /// accessor that cannot say so. Check isValid() first where it matters.
    [[nodiscard]] constexpr double toDouble() const noexcept
    {
        return den == 0 ? 0.0 : static_cast<double> (num) / static_cast<double> (den);
    }

    [[nodiscard]] constexpr bool isValid() const noexcept { return den > 0 && num >= 0; }

    /// Cross-multiplied, so 1/2 == 2/4. A non-positive denominator carries no value: such Rationals
    /// sort before every real one and compare field by field, so {1,0} and {2,0} stay distinct.
    friend constexpr std::strong_ordering operator<=> (Rational a, Rational b) noexcept
    {
        const bool aNoValue = a.den <= 0;
        const bool bNoValue = b.den <= 0;

        if (aNoValue || bNoValue)
        {
            if (aNoValue != bNoValue) return aNoValue ? std::strong_ordering::less : std::strong_ordering::greater;
            if (auto c = a.num <=> b.num; std::is_neq (c)) return c;
            return a.den <=> b.den;
        }

        return static_cast<std::int64_t> (a.num) * b.den <=> static_cast<std::int64_t> (b.num) * a.den;
    }

    friend constexpr bool operator== (Rational a, Rational b) noexcept { return std::is_eq (a <=> b); }
};

[[nodiscard]] inline std::string toString (Rational r)
{
    return std::to_string (r.num) + "/" + std::to_string (r.den);
}

inline std::ostream& operator<< (std::ostream& os, Rational r)
{
    return os << toString (r);
}

/// A point in (or duration of) media time: `value / timescale` seconds, plus a kind that models
/// the non-finite states needed for tolerances and arithmetic sinks. Rational rather than double so
/// 1/30 s survives round-trips through stream timebases; only exact conversions (integral
/// std::chrono durations) are implicit, `Time::seconds(1.5)` names the lossy one. `Time{}` is zero,
/// unlike CMTime. Comparison is mathematical (1/2 == 2/4) under a total order.
class Time
{
public:
    enum class Kind : std::uint8_t
    {
        invalid,
        finite,
        positiveInfinity,
        negativeInfinity
    };

    /// Timescale used by `seconds()` when none is given: microseconds (== AV_TIME_BASE).
    static constexpr std::int32_t defaultTimescale = 1'000'000;

    constexpr Time() noexcept = default;

    /// `value / timescale` seconds. A non-positive timescale yields an invalid Time.
    explicit constexpr Time (std::int64_t value, std::int32_t timescale) noexcept
      : value (value), timescale (timescale), kind (timescale > 0 ? Kind::finite : Kind::invalid)
    {
        if (kind == Kind::invalid)
        {
            value = 0;
            timescale = 1;
        }
    }

    /// Implicit, lossless conversion from integral chrono durations whose period is 1/N seconds.
    template <std::integral Rep, class Period>
        requires (Period::num == 1 && Period::den <= std::numeric_limits<std::int32_t>::max())
    constexpr Time (std::chrono::duration<Rep, Period> d) noexcept // NOLINT(google-explicit-constructor)
      : Time (static_cast<std::int64_t> (d.count()), static_cast<std::int32_t> (Period::den))
    {
    }

    /// ... and from whole-second multiples: minutes, hours, days. Exactly as lossless, and a
    /// timeline is as likely to be written `2min` as `120s`. Overflow yields an invalid Time.
    template <std::integral Rep, class Period>
        requires (Period::den == 1 && Period::num > 1)
    constexpr Time (std::chrono::duration<Rep, Period> d) noexcept // NOLINT(google-explicit-constructor)
      : Time (wholeSeconds (static_cast<std::int64_t> (d.count()), Period::num))
    {
    }

    [[nodiscard]] static constexpr Time zero() noexcept { return Time{}; }
    [[nodiscard]] static constexpr Time invalid() noexcept { return make (0, 1, Kind::invalid); }
    [[nodiscard]] static constexpr Time positiveInfinity() noexcept { return make (0, 1, Kind::positiveInfinity); }
    [[nodiscard]] static constexpr Time negativeInfinity() noexcept { return make (0, 1, Kind::negativeInfinity); }

    /// Seconds as a double, rounded to the nearest tick of `timescale`.
    [[nodiscard]] static constexpr Time seconds (double s, std::int32_t timescale = defaultTimescale) noexcept
    {
        // NaN check without <cmath>. The pragma is local so a consumer building with -Wfloat-equal
        // stays clean: the comparison is deliberate, not a value comparison.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
        if (timescale <= 0 || s != s) return invalid();
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
        const double scaled = s * static_cast<double> (timescale);
        constexpr double limit = 9.2e18; // conservative int64 bound

        if (scaled >= limit) return positiveInfinity();
        if (scaled <= -limit) return negativeInfinity();
        const double rounded = scaled >= 0 ? scaled + 0.5 : scaled - 0.5;
        return Time{ static_cast<std::int64_t> (rounded), timescale };
    }

    /// A raw timestamp in the given time base. INT64_MIN (AV_NOPTS_VALUE) yields an invalid Time.
    [[nodiscard]] static constexpr Time fromTimestamp (std::int64_t ts, Rational timeBase) noexcept
    {
        if (ts == std::numeric_limits<std::int64_t>::min() || timeBase.den <= 0 || timeBase.num <= 0)
        {
            return invalid();
        }

        if (timeBase.num == 1) return Time{ ts, timeBase.den };
        const auto v = detail::rescale (ts, timeBase.num, 1, TimeRounding::nearest);

        if (! v) return invalid();
        return Time{ *v, timeBase.den };
    }

    /// Any chrono duration, rescaled (rounding to nearest) into `timescale` ticks.
    template <class Rep, class Period>
    [[nodiscard]] static constexpr Time fromDuration (std::chrono::duration<Rep, Period> d,
                                                      std::int32_t timescale = defaultTimescale) noexcept
    {
        const auto ticks = std::chrono::duration_cast<std::chrono::duration<double>> (d).count();
        return seconds (ticks, timescale);
    }

    /// The presentation time of frame `n` (0-based) at a constant frame rate: exactly n / fps
    /// seconds (`frames(29, {30000, 1001})` is 29029/30000). Invalid for a non-positive rate.
    [[nodiscard]] static constexpr Time frames (std::int64_t n, Rational fps) noexcept
    {
        if (fps.num <= 0 || fps.den <= 0) return invalid();
        const auto v = detail::rescale (n, fps.den, 1, TimeRounding::zero);

        if (! v) return invalid();
        return Time{ *v, fps.num };
    }

    [[nodiscard]] constexpr std::int64_t getValue() const noexcept { return value; }
    [[nodiscard]] constexpr std::int32_t getTimescale() const noexcept { return timescale; }
    [[nodiscard]] constexpr Kind getKind() const noexcept { return kind; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return kind != Kind::invalid; }
    [[nodiscard]] constexpr bool isFinite() const noexcept { return kind == Kind::finite; }
    [[nodiscard]] constexpr bool isPositiveInfinity() const noexcept { return kind == Kind::positiveInfinity; }
    [[nodiscard]] constexpr bool isNegativeInfinity() const noexcept { return kind == Kind::negativeInfinity; }
    [[nodiscard]] constexpr bool isZero() const noexcept { return isFinite() && value == 0; }
    [[nodiscard]] constexpr bool isNegative() const noexcept
    {
        return isNegativeInfinity() || (isFinite() && value < 0);
    }

    /// Seconds as double. +/-inf for the infinities, NaN for invalid.
    [[nodiscard]] constexpr double toSeconds() const noexcept
    {
        switch (kind)
        {
        case Kind::finite:
            return static_cast<double> (value) / static_cast<double> (timescale);
        case Kind::positiveInfinity:
            return std::numeric_limits<double>::infinity();
        case Kind::negativeInfinity:
            return -std::numeric_limits<double>::infinity();
        case Kind::invalid:
            break;
        }

        return std::numeric_limits<double>::quiet_NaN();
    }

    /// Finite Time as a chrono duration, rounded to nearest. Zero for a non-finite Time, and zero
    /// when the value does not fit the target duration — check isFinite() and the magnitude first
    /// where the difference matters. The denominator is formed in 128 bits, so a duration with an
    /// enormous period (ratio<INTMAX_MAX, 1>) is answered rather than overflowing.
    template <class Duration>
    [[nodiscard]] constexpr Duration toDuration() const noexcept
    {
        if (! isFinite()) return Duration{};
        const detail::int128 den =
            static_cast<detail::int128> (timescale) * static_cast<detail::int128> (Duration::period::num);

        if (den > static_cast<detail::int128> (std::numeric_limits<std::int64_t>::max())) return Duration{};
        const auto v =
            detail::rescale (value, Duration::period::den, static_cast<std::int64_t> (den), TimeRounding::nearest);
        return Duration{ static_cast<typename Duration::rep> (v.value_or (0)) };
    }

    /// Timestamp in `timeBase` units. Infinities map to INT64_MAX/INT64_MIN+1, invalid to INT64_MIN.
    [[nodiscard]] constexpr std::int64_t toTimestamp (Rational timeBase,
                                                      TimeRounding rounding = TimeRounding::nearest) const noexcept
    {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        switch (kind)
        {
        case Kind::invalid:
            return min;
        case Kind::positiveInfinity:
            return max;
        case Kind::negativeInfinity:
            return min + 1;
        case Kind::finite:
            break;
        }

        if (timeBase.num <= 0 || timeBase.den <= 0) return min;
        const auto v =
            detail::rescale (value, timeBase.den, static_cast<std::int64_t> (timescale) * timeBase.num, rounding);
        return v.value_or (value < 0 ? min + 1 : max);
    }

    /// The same instant expressed in another timescale.
    [[nodiscard]] constexpr Time converted (std::int32_t newTimescale,
                                            TimeRounding rounding = TimeRounding::nearest) const noexcept
    {
        if (! isFinite()) return *this;
        if (newTimescale <= 0) return invalid();
        if (newTimescale == timescale) return *this;
        const auto v = detail::rescale (value, newTimescale, timescale, rounding);

        if (! v) return invalid();
        return Time{ *v, newTimescale };
    }

    friend constexpr Time operator- (Time t) noexcept
    {
        switch (t.kind)
        {
        case Kind::finite:
            if (t.value == std::numeric_limits<std::int64_t>::min()) return invalid();
            return Time{ -t.value, t.timescale };
        case Kind::positiveInfinity:
            return negativeInfinity();
        case Kind::negativeInfinity:
            return positiveInfinity();
        case Kind::invalid:
            break;
        }

        return invalid();
    }

    /// Exact when lcm(timescales) fits in int32 (always the case for the usual media timescales);
    /// otherwise the sum is expressed in the larger timescale, rounding to nearest. Overflow ->
    /// invalid.
    friend constexpr Time operator+ (Time a, Time b) noexcept
    {
        if (! a.isValid() || ! b.isValid()) return invalid();
        if (! a.isFinite() || ! b.isFinite())
        {
            if (a.isFinite()) return b;
            if (b.isFinite()) return a;
            return a.kind == b.kind ? a : invalid(); // inf + (-inf) is undefined
        }

        const std::int32_t ts = commonTimescale (a.timescale, b.timescale);
        const Time ca = a.converted (ts);
        const Time cb = b.converted (ts);

        if (! ca.isFinite() || ! cb.isFinite()) return invalid();
        const std::int64_t x = ca.value;
        const std::int64_t y = cb.value;

        if ((y > 0 && x > std::numeric_limits<std::int64_t>::max() - y)
            || (y < 0 && x < std::numeric_limits<std::int64_t>::min() - y))
        {
            return invalid();
        }

        return Time{ x + y, ts };
    }

    friend constexpr Time operator- (Time a, Time b) noexcept { return a + (-b); }

    /// Scaling by a whole number, exactly (the product is formed in 128 bits); overflow yields an
    /// invalid Time. Any integral type works, so a 64-bit count cannot wrap on the way in. There is
    /// deliberately no floating-point overload — `t * 1.5` would truncate to `t * 1` with no warning;
    /// write `Time::seconds(t.toSeconds() * 1.5)` and own the rounding.
    template <std::integral K>
    friend constexpr Time operator* (Time a, K k) noexcept
    {
        const bool negative = std::cmp_less (k, 0);

        if (std::cmp_greater (k, std::numeric_limits<std::int64_t>::max())) return invalid();
        const auto k64 = static_cast<std::int64_t> (k);

        if (! a.isFinite())
        {
            if (! a.isValid() || k64 == 0) return invalid();
            return negative ? -a : a;
        }

        const auto v = detail::rescale (a.value, k64, 1, TimeRounding::zero);

        if (! v) return invalid();
        return Time{ *v, a.timescale };
    }

    template <std::integral K>
    friend constexpr Time operator* (K k, Time a) noexcept
    {
        return a * k;
    }

    friend constexpr Time operator* (Time, double) = delete;
    friend constexpr Time operator* (double, Time) = delete;
    friend constexpr Time operator* (Time, long double) = delete;
    friend constexpr Time operator* (long double, Time) = delete;
    friend constexpr Time operator* (Time, float) = delete;
    friend constexpr Time operator* (float, Time) = delete;

    // Total order: invalid < -inf < finite < +inf.
    friend constexpr std::weak_ordering operator<=> (Time a, Time b) noexcept
    {
        const int ra = getRank (a.kind);
        const int rb = getRank (b.kind);

        if (ra != rb) return ra < rb ? std::weak_ordering::less : std::weak_ordering::greater;
        if (a.kind != Kind::finite) return std::weak_ordering::equivalent;
        const int c = detail::compareRatios (a.value, a.timescale, b.value, b.timescale);
        return c < 0 ? std::weak_ordering::less
                     : (c > 0 ? std::weak_ordering::greater : std::weak_ordering::equivalent);
    }

    /// std::is_eq rather than `== 0`: comparing an ordering against a literal 0 trips
    /// -Wzero-as-null-pointer-constant in a consumer that enables it, inside our header, where they
    /// cannot suppress it.
    friend constexpr bool operator== (Time a, Time b) noexcept { return std::is_eq (a <=> b); }

    /// Hash consistent with the mathematical equality (1/2 hashes like 2/4).
    [[nodiscard]] constexpr std::size_t hash() const noexcept
    {
        std::int64_t v = value;
        std::int64_t ts = timescale;

        if (kind == Kind::finite && v != std::numeric_limits<std::int64_t>::min())
        {
            const std::int64_t g = detail::gcd64 (v < 0 ? -v : v, ts);

            if (g > 1)
            {
                v /= g;
                ts /= g;
            }
        }
        else if (kind != Kind::finite)
        {
            v = 0;
            ts = 1;
        }

        const auto h1 = static_cast<std::size_t> (static_cast<std::uint64_t> (v) * 0x9E3779B97F4A7C15ULL);
        const auto h2 = static_cast<std::size_t> (static_cast<std::uint64_t> (ts) * 0xC2B2AE3D27D4EB4FULL);
        return h1 ^ (h2 + 0x165667B19E3779F9ULL + (h1 << 6) + (h1 >> 2)) ^ static_cast<std::size_t> (kind);
    }

private:
    /// `count` whole seconds times `per` seconds each, as a Time, or invalid on overflow.
    static constexpr Time wholeSeconds (std::int64_t count, std::int64_t per) noexcept
    {
        std::int64_t seconds = 0;

        if (__builtin_mul_overflow (count, per, &seconds)) return invalid();
        return Time{ seconds, 1 };
    }

    static constexpr Time make (std::int64_t v, std::int32_t ts, Kind k) noexcept
    {
        Time t;
        t.value = v;
        t.timescale = ts;
        t.kind = k;
        return t;
    }

    static constexpr int getRank (Kind k) noexcept
    {
        switch (k)
        {
        case Kind::invalid:
            return 0;
        case Kind::negativeInfinity:
            return 1;
        case Kind::finite:
            return 2;
        case Kind::positiveInfinity:
            return 3;
        }

        return 0;
    }

    /// lcm when it fits in int32, otherwise the larger of the two (rounding may then occur).
    static constexpr std::int32_t commonTimescale (std::int32_t a, std::int32_t b) noexcept
    {
        if (a == b) return a;
        const std::int64_t g = detail::gcd64 (a, b);
        const std::int64_t l = (a / g) * static_cast<std::int64_t> (b);

        if (l <= std::numeric_limits<std::int32_t>::max()) return static_cast<std::int32_t> (l);
        return a > b ? a : b;
    }

    std::int64_t value{ 0 };
    std::int32_t timescale{ 1 };
    Kind kind{ Kind::finite };
};

[[nodiscard]] inline std::string toString (Time t)
{
    switch (t.getKind())
    {
    case Time::Kind::invalid:
        return "invalid";
    case Time::Kind::positiveInfinity:
        return "+inf";
    case Time::Kind::negativeInfinity:
        return "-inf";
    case Time::Kind::finite:
        break;
    }

    // std::to_chars is locale-independent (no LC_NUMERIC surprises in error messages).
    char buf[64];
    const auto r = std::to_chars (buf, buf + sizeof buf, t.toSeconds(), std::chars_format::fixed, 6);
    std::string s = r.ec == std::errc{} ? std::string (buf, r.ptr) : std::to_string (t.toSeconds());
    s += "s (";
    s += std::to_string (t.getValue());
    s += '/';
    s += std::to_string (t.getTimescale());
    s += ')';
    return s;
}

inline std::ostream& operator<< (std::ostream& os, Time t)
{
    return os << toString (t);
}

/// A half-open interval [start, start + duration).
struct TimeRange
{
    Time start = {};
    Time duration = {};

    [[nodiscard]] constexpr Time end() const noexcept { return start + duration; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return ! duration.isValid() || duration <= Time::zero(); }
    [[nodiscard]] constexpr bool contains (Time t) const noexcept { return t.isValid() && t >= start && t < end(); }
};

/// How far from the requested time a returned frame may be. Zero tolerances (the default) mean
/// frame-accurate. Infinite tolerances (`any()`) mean "the keyframe at or before the time" — the
/// fastest mode. Anything in between allows the generator to stop decoding early when a frame
/// within the window appears.
struct Tolerance
{
    Time before = Time::zero(); ///< a frame up to this much *earlier* than requested is acceptable
    Time after = Time::zero();  ///< a frame up to this much *later* than requested is acceptable

    [[nodiscard]] static constexpr Tolerance exact() noexcept { return {}; }
    [[nodiscard]] static constexpr Tolerance any() noexcept
    {
        return { Time::positiveInfinity(), Time::positiveInfinity() };
    }

    [[nodiscard]] static constexpr Tolerance symmetric (Time t) noexcept { return { t, t }; }

    [[nodiscard]] constexpr bool isExact() const noexcept { return before.isZero() && after.isZero(); }
};

} // namespace stills

template <>
struct std::hash<stills::Time>
{
    [[nodiscard]] std::size_t operator() (stills::Time t) const noexcept { return t.hash(); }
};

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
template <>
struct std::formatter<stills::Time> : std::formatter<std::string>
{
    template <class Ctx>
    auto format (stills::Time t, Ctx& ctx) const
    {
        return std::formatter<std::string>::format (stills::toString (t), ctx);
    }
};

template <>
struct std::formatter<stills::Rational> : std::formatter<std::string>
{
    template <class Ctx>
    auto format (stills::Rational r, Ctx& ctx) const
    {
        return std::formatter<std::string>::format (stills::toString (r), ctx);
    }
};
#endif
