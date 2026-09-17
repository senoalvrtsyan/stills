#pragma once
// stills/time.hpp — CMTime-like rational time. No FFmpeg dependency.

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

#include "stills/detail/config.hpp"

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#include <format>
#endif

namespace stills {

/// Rounding used when converting between timescales (Time::to_timestamp, Time::converted).
enum class TimeRounding : std::uint8_t {
  nearest,  ///< round half away from zero (libav AV_ROUND_NEAR_INF)
  down,     ///< toward negative infinity
  up,       ///< toward positive infinity
  zero,     ///< toward zero
};

namespace detail {

__extension__ using int128 = __int128;

/// Computes a * num / den with the requested rounding. Requires den > 0.
/// Returns nullopt when the result does not fit in int64_t.
[[nodiscard]] constexpr std::optional<std::int64_t> rescale(std::int64_t a, std::int64_t num,
                                                            std::int64_t den,
                                                            TimeRounding rounding) noexcept {
  if (den <= 0) return std::nullopt;
  const int128 p = static_cast<int128>(a) * static_cast<int128>(num);
  const int128 d = static_cast<int128>(den);
  int128 q = p / d;  // truncates toward zero
  const int128 rem = p % d;
  if (rem != 0) {
    switch (rounding) {
      case TimeRounding::zero:
        break;
      case TimeRounding::down:
        if (p < 0) --q;
        break;
      case TimeRounding::up:
        if (p > 0) ++q;
        break;
      case TimeRounding::nearest: {
        const int128 twice = (rem < 0 ? -rem : rem) * 2;
        if (twice >= d) q += (p < 0) ? -1 : 1;
        break;
      }
    }
  }
  if (q > static_cast<int128>(std::numeric_limits<std::int64_t>::max()) ||
      q < static_cast<int128>(std::numeric_limits<std::int64_t>::min())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(q);
}

/// Compares a/b with c/d (b, d > 0) without overflow. Returns <0, 0, >0.
[[nodiscard]] constexpr int compare_ratios(std::int64_t a, std::int64_t b, std::int64_t c,
                                           std::int64_t d) noexcept {
  const int128 lhs = static_cast<int128>(a) * static_cast<int128>(d);
  const int128 rhs = static_cast<int128>(c) * static_cast<int128>(b);
  return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

[[nodiscard]] constexpr std::int64_t gcd64(std::int64_t a, std::int64_t b) noexcept {
  while (b != 0) {
    const std::int64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

}  // namespace detail

/// A positive rational, mirroring AVRational without exposing it.
struct Rational {
  std::int32_t num{0};
  std::int32_t den{1};

  /// Zero when the denominator is zero: an invalid Rational has no value, and this is the one
  /// accessor that cannot say so. Check is_valid() first where it matters.
  [[nodiscard]] constexpr double to_double() const noexcept {
    return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
  }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return den > 0 && num >= 0; }

  /// Cross-multiplied, so 1/2 == 2/4. A non-positive denominator carries no value: such Rationals
  /// sort before every real one and compare field by field, so {1,0} and {2,0} stay distinct.
  friend constexpr std::strong_ordering operator<=>(Rational a, Rational b) noexcept {
    const bool a_novalue = a.den <= 0;
    const bool b_novalue = b.den <= 0;
    if (a_novalue || b_novalue) {
      if (a_novalue != b_novalue)
        return a_novalue ? std::strong_ordering::less : std::strong_ordering::greater;
      if (auto c = a.num <=> b.num; std::is_neq(c)) return c;
      return a.den <=> b.den;
    }
    return static_cast<std::int64_t>(a.num) * b.den <=> static_cast<std::int64_t>(b.num) * a.den;
  }
  friend constexpr bool operator==(Rational a, Rational b) noexcept { return std::is_eq(a <=> b); }
};

[[nodiscard]] inline std::string to_string(Rational r) {
  return std::to_string(r.num) + "/" + std::to_string(r.den);
}
inline std::ostream& operator<<(std::ostream& os, Rational r) {
  return os << to_string(r);
}

/// A point in (or duration of) media time: `value / timescale` seconds, plus a kind that models
/// the non-finite states needed for tolerances and arithmetic sinks. Rational rather than double so
/// 1/30 s survives round-trips through stream timebases; only exact conversions (integral
/// std::chrono durations) are implicit, `Time::seconds(1.5)` names the lossy one. `Time{}` is zero,
/// unlike CMTime. Comparison is mathematical (1/2 == 2/4) under a total order.
class Time {
 public:
  enum class Kind : std::uint8_t { invalid, finite, positive_infinity, negative_infinity };

  /// Timescale used by `seconds()` when none is given: microseconds (== AV_TIME_BASE).
  static constexpr std::int32_t default_timescale = 1'000'000;

  constexpr Time() noexcept = default;

  /// `value / timescale` seconds. A non-positive timescale yields an invalid Time.
  explicit constexpr Time(std::int64_t value, std::int32_t timescale) noexcept
      : value_(value), timescale_(timescale), kind_(timescale > 0 ? Kind::finite : Kind::invalid) {
    if (kind_ == Kind::invalid) {
      value_ = 0;
      timescale_ = 1;
    }
  }

  /// Implicit, lossless conversion from integral chrono durations whose period is 1/N seconds.
  template <std::integral Rep, class Period>
    requires(Period::num == 1 && Period::den <= std::numeric_limits<std::int32_t>::max())
  constexpr Time(
      std::chrono::duration<Rep, Period> d) noexcept  // NOLINT(google-explicit-constructor)
      : Time(static_cast<std::int64_t>(d.count()), static_cast<std::int32_t>(Period::den)) {}

  /// ... and from whole-second multiples: minutes, hours, days. Exactly as lossless, and a
  /// timeline is as likely to be written `2min` as `120s`. Overflow yields an invalid Time.
  template <std::integral Rep, class Period>
    requires(Period::den == 1 && Period::num > 1)
  constexpr Time(
      std::chrono::duration<Rep, Period> d) noexcept  // NOLINT(google-explicit-constructor)
      : Time(whole_seconds(static_cast<std::int64_t>(d.count()), Period::num)) {}

  [[nodiscard]] static constexpr Time zero() noexcept { return Time{}; }
  [[nodiscard]] static constexpr Time invalid() noexcept { return make(0, 1, Kind::invalid); }
  [[nodiscard]] static constexpr Time positive_infinity() noexcept {
    return make(0, 1, Kind::positive_infinity);
  }
  [[nodiscard]] static constexpr Time negative_infinity() noexcept {
    return make(0, 1, Kind::negative_infinity);
  }

  /// Seconds as a double, rounded to the nearest tick of `timescale`.
  [[nodiscard]] static constexpr Time seconds(double s,
                                              std::int32_t timescale = default_timescale) noexcept {
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
    const double scaled = s * static_cast<double>(timescale);
    constexpr double limit = 9.2e18;  // conservative int64 bound
    if (scaled >= limit) return positive_infinity();
    if (scaled <= -limit) return negative_infinity();
    const double rounded = scaled >= 0 ? scaled + 0.5 : scaled - 0.5;
    return Time{static_cast<std::int64_t>(rounded), timescale};
  }

  /// A raw timestamp in the given time base. INT64_MIN (AV_NOPTS_VALUE) yields an invalid Time.
  [[nodiscard]] static constexpr Time from_timestamp(std::int64_t ts, Rational time_base) noexcept {
    if (ts == std::numeric_limits<std::int64_t>::min() || time_base.den <= 0 ||
        time_base.num <= 0) {
      return invalid();
    }
    if (time_base.num == 1) return Time{ts, time_base.den};
    const auto v = detail::rescale(ts, time_base.num, 1, TimeRounding::nearest);
    if (!v) return invalid();
    return Time{*v, time_base.den};
  }

  /// Any chrono duration, rescaled (rounding to nearest) into `timescale` ticks.
  template <class Rep, class Period>
  [[nodiscard]] static constexpr Time from_duration(
      std::chrono::duration<Rep, Period> d, std::int32_t timescale = default_timescale) noexcept {
    const auto ticks = std::chrono::duration_cast<std::chrono::duration<double>>(d).count();
    return seconds(ticks, timescale);
  }

  /// The presentation time of frame `n` (0-based) at a constant frame rate: exactly n / fps
  /// seconds (`frames(29, {30000, 1001})` is 29029/30000). Invalid for a non-positive rate.
  [[nodiscard]] static constexpr Time frames(std::int64_t n, Rational fps) noexcept {
    if (fps.num <= 0 || fps.den <= 0) return invalid();
    const auto v = detail::rescale(n, fps.den, 1, TimeRounding::zero);
    if (!v) return invalid();
    return Time{*v, fps.num};
  }

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr std::int32_t timescale() const noexcept { return timescale_; }
  [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return kind_ != Kind::invalid; }
  [[nodiscard]] constexpr bool is_finite() const noexcept { return kind_ == Kind::finite; }
  [[nodiscard]] constexpr bool is_positive_infinity() const noexcept {
    return kind_ == Kind::positive_infinity;
  }
  [[nodiscard]] constexpr bool is_negative_infinity() const noexcept {
    return kind_ == Kind::negative_infinity;
  }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return is_finite() && value_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept {
    return is_negative_infinity() || (is_finite() && value_ < 0);
  }

  /// Seconds as double. +/-inf for the infinities, NaN for invalid.
  [[nodiscard]] constexpr double to_seconds() const noexcept {
    switch (kind_) {
      case Kind::finite:
        return static_cast<double>(value_) / static_cast<double>(timescale_);
      case Kind::positive_infinity:
        return std::numeric_limits<double>::infinity();
      case Kind::negative_infinity:
        return -std::numeric_limits<double>::infinity();
      case Kind::invalid:
        break;
    }
    return std::numeric_limits<double>::quiet_NaN();
  }

  /// Finite Time as a chrono duration, rounded to nearest. Zero for a non-finite Time, and zero
  /// when the value does not fit the target duration — check is_finite() and the magnitude first
  /// where the difference matters. The denominator is formed in 128 bits, so a duration with an
  /// enormous period (ratio<INTMAX_MAX, 1>) is answered rather than overflowing.
  template <class Duration>
  [[nodiscard]] constexpr Duration to_duration() const noexcept {
    if (!is_finite()) return Duration{};
    const detail::int128 den = static_cast<detail::int128>(timescale_) *
                               static_cast<detail::int128>(Duration::period::num);
    if (den > static_cast<detail::int128>(std::numeric_limits<std::int64_t>::max()))
      return Duration{};
    const auto v = detail::rescale(value_, Duration::period::den, static_cast<std::int64_t>(den),
                                   TimeRounding::nearest);
    return Duration{static_cast<typename Duration::rep>(v.value_or(0))};
  }

  /// Timestamp in `time_base` units. Infinities map to INT64_MAX/INT64_MIN+1, invalid to INT64_MIN.
  [[nodiscard]] constexpr std::int64_t to_timestamp(
      Rational time_base, TimeRounding rounding = TimeRounding::nearest) const noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    switch (kind_) {
      case Kind::invalid:
        return min;
      case Kind::positive_infinity:
        return max;
      case Kind::negative_infinity:
        return min + 1;
      case Kind::finite:
        break;
    }
    if (time_base.num <= 0 || time_base.den <= 0) return min;
    const auto v = detail::rescale(value_, time_base.den,
                                   static_cast<std::int64_t>(timescale_) * time_base.num, rounding);
    return v.value_or(value_ < 0 ? min + 1 : max);
  }

  /// The same instant expressed in another timescale.
  [[nodiscard]] constexpr Time converted(
      std::int32_t new_timescale, TimeRounding rounding = TimeRounding::nearest) const noexcept {
    if (!is_finite()) return *this;
    if (new_timescale <= 0) return invalid();
    if (new_timescale == timescale_) return *this;
    const auto v = detail::rescale(value_, new_timescale, timescale_, rounding);
    if (!v) return invalid();
    return Time{*v, new_timescale};
  }

  friend constexpr Time operator-(Time t) noexcept {
    switch (t.kind_) {
      case Kind::finite:
        if (t.value_ == std::numeric_limits<std::int64_t>::min()) return invalid();
        return Time{-t.value_, t.timescale_};
      case Kind::positive_infinity:
        return negative_infinity();
      case Kind::negative_infinity:
        return positive_infinity();
      case Kind::invalid:
        break;
    }
    return invalid();
  }

  /// Exact when lcm(timescales) fits in int32 (always the case for the usual media timescales);
  /// otherwise the sum is expressed in the larger timescale, rounding to nearest. Overflow ->
  /// invalid.
  friend constexpr Time operator+(Time a, Time b) noexcept {
    if (!a.is_valid() || !b.is_valid()) return invalid();
    if (!a.is_finite() || !b.is_finite()) {
      if (a.is_finite()) return b;
      if (b.is_finite()) return a;
      return a.kind_ == b.kind_ ? a : invalid();  // inf + (-inf) is undefined
    }
    const std::int32_t ts = common_timescale(a.timescale_, b.timescale_);
    const Time ca = a.converted(ts);
    const Time cb = b.converted(ts);
    if (!ca.is_finite() || !cb.is_finite()) return invalid();
    const std::int64_t x = ca.value_;
    const std::int64_t y = cb.value_;
    if ((y > 0 && x > std::numeric_limits<std::int64_t>::max() - y) ||
        (y < 0 && x < std::numeric_limits<std::int64_t>::min() - y)) {
      return invalid();
    }
    return Time{x + y, ts};
  }

  friend constexpr Time operator-(Time a, Time b) noexcept { return a + (-b); }

  /// Scaling by a whole number, exactly (the product is formed in 128 bits); overflow yields an
  /// invalid Time. Any integral type works, so a 64-bit count cannot wrap on the way in. There is
  /// deliberately no floating-point overload — `t * 1.5` would truncate to `t * 1` with no warning;
  /// write `Time::seconds(t.to_seconds() * 1.5)` and own the rounding.
  template <std::integral K>
  friend constexpr Time operator*(Time a, K k) noexcept {
    const bool negative = std::cmp_less(k, 0);
    if (std::cmp_greater(k, std::numeric_limits<std::int64_t>::max())) return invalid();
    const auto k64 = static_cast<std::int64_t>(k);
    if (!a.is_finite()) {
      if (!a.is_valid() || k64 == 0) return invalid();
      return negative ? -a : a;
    }
    const auto v = detail::rescale(a.value_, k64, 1, TimeRounding::zero);
    if (!v) return invalid();
    return Time{*v, a.timescale_};
  }
  template <std::integral K>
  friend constexpr Time operator*(K k, Time a) noexcept {
    return a * k;
  }
  friend constexpr Time operator*(Time, double) = delete;
  friend constexpr Time operator*(double, Time) = delete;
  friend constexpr Time operator*(Time, long double) = delete;
  friend constexpr Time operator*(long double, Time) = delete;
  friend constexpr Time operator*(Time, float) = delete;
  friend constexpr Time operator*(float, Time) = delete;

  // Total order: invalid < -inf < finite < +inf.
  friend constexpr std::weak_ordering operator<=>(Time a, Time b) noexcept {
    const int ra = rank(a.kind_);
    const int rb = rank(b.kind_);
    if (ra != rb) return ra < rb ? std::weak_ordering::less : std::weak_ordering::greater;
    if (a.kind_ != Kind::finite) return std::weak_ordering::equivalent;
    const int c = detail::compare_ratios(a.value_, a.timescale_, b.value_, b.timescale_);
    return c < 0 ? std::weak_ordering::less
                 : (c > 0 ? std::weak_ordering::greater : std::weak_ordering::equivalent);
  }
  /// std::is_eq rather than `== 0`: comparing an ordering against a literal 0 trips
  /// -Wzero-as-null-pointer-constant in a consumer that enables it, inside our header, where they
  /// cannot suppress it.
  friend constexpr bool operator==(Time a, Time b) noexcept { return std::is_eq(a <=> b); }

  /// Hash consistent with the mathematical equality (1/2 hashes like 2/4).
  [[nodiscard]] constexpr std::size_t hash() const noexcept {
    std::int64_t v = value_;
    std::int64_t ts = timescale_;
    if (kind_ == Kind::finite && v != std::numeric_limits<std::int64_t>::min()) {
      const std::int64_t g = detail::gcd64(v < 0 ? -v : v, ts);
      if (g > 1) {
        v /= g;
        ts /= g;
      }
    } else if (kind_ != Kind::finite) {
      v = 0;
      ts = 1;
    }
    const auto h1 = static_cast<std::size_t>(static_cast<std::uint64_t>(v) * 0x9E3779B97F4A7C15ULL);
    const auto h2 =
        static_cast<std::size_t>(static_cast<std::uint64_t>(ts) * 0xC2B2AE3D27D4EB4FULL);
    return h1 ^ (h2 + 0x165667B19E3779F9ULL + (h1 << 6) + (h1 >> 2)) ^
           static_cast<std::size_t>(kind_);
  }

 private:
  /// `count` whole seconds times `per` seconds each, as a Time, or invalid on overflow.
  static constexpr Time whole_seconds(std::int64_t count, std::int64_t per) noexcept {
    std::int64_t seconds = 0;
    if (__builtin_mul_overflow(count, per, &seconds)) return invalid();
    return Time{seconds, 1};
  }

  static constexpr Time make(std::int64_t v, std::int32_t ts, Kind k) noexcept {
    Time t;
    t.value_ = v;
    t.timescale_ = ts;
    t.kind_ = k;
    return t;
  }
  static constexpr int rank(Kind k) noexcept {
    switch (k) {
      case Kind::invalid:
        return 0;
      case Kind::negative_infinity:
        return 1;
      case Kind::finite:
        return 2;
      case Kind::positive_infinity:
        return 3;
    }
    return 0;
  }
  /// lcm when it fits in int32, otherwise the larger of the two (rounding may then occur).
  static constexpr std::int32_t common_timescale(std::int32_t a, std::int32_t b) noexcept {
    if (a == b) return a;
    const std::int64_t g = detail::gcd64(a, b);
    const std::int64_t l = (a / g) * static_cast<std::int64_t>(b);
    if (l <= std::numeric_limits<std::int32_t>::max()) return static_cast<std::int32_t>(l);
    return a > b ? a : b;
  }

  std::int64_t value_{0};
  std::int32_t timescale_{1};
  Kind kind_{Kind::finite};
};

[[nodiscard]] inline std::string to_string(Time t) {
  switch (t.kind()) {
    case Time::Kind::invalid:
      return "invalid";
    case Time::Kind::positive_infinity:
      return "+inf";
    case Time::Kind::negative_infinity:
      return "-inf";
    case Time::Kind::finite:
      break;
  }
  // std::to_chars is locale-independent (no LC_NUMERIC surprises in error messages).
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, t.to_seconds(), std::chars_format::fixed, 6);
  std::string s = r.ec == std::errc{} ? std::string(buf, r.ptr) : std::to_string(t.to_seconds());
  s += "s (";
  s += std::to_string(t.value());
  s += '/';
  s += std::to_string(t.timescale());
  s += ')';
  return s;
}
inline std::ostream& operator<<(std::ostream& os, Time t) {
  return os << to_string(t);
}

/// A half-open interval [start, start + duration).
struct TimeRange {
  Time start = {};
  Time duration = {};

  [[nodiscard]] constexpr Time end() const noexcept { return start + duration; }
  [[nodiscard]] constexpr bool is_empty() const noexcept {
    return !duration.is_valid() || duration <= Time::zero();
  }
  [[nodiscard]] constexpr bool contains(Time t) const noexcept {
    return t.is_valid() && t >= start && t < end();
  }
};

/// How far from the requested time a returned frame may be. Zero tolerances (the default) mean
/// frame-accurate. Infinite tolerances (`any()`) mean "the keyframe at or before the time" — the
/// fastest mode. Anything in between allows the generator to stop decoding early when a frame
/// within the window appears.
struct Tolerance {
  Time before = Time::zero();  ///< a frame up to this much *earlier* than requested is acceptable
  Time after = Time::zero();   ///< a frame up to this much *later* than requested is acceptable

  [[nodiscard]] static constexpr Tolerance exact() noexcept { return {}; }
  [[nodiscard]] static constexpr Tolerance any() noexcept {
    return {Time::positive_infinity(), Time::positive_infinity()};
  }
  [[nodiscard]] static constexpr Tolerance symmetric(Time t) noexcept { return {t, t}; }

  [[nodiscard]] constexpr bool is_exact() const noexcept {
    return before.is_zero() && after.is_zero();
  }
};

}  // namespace stills

template <>
struct std::hash<stills::Time> {
  [[nodiscard]] std::size_t operator()(stills::Time t) const noexcept { return t.hash(); }
};

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
template <>
struct std::formatter<stills::Time> : std::formatter<std::string> {
  template <class Ctx>
  auto format(stills::Time t, Ctx& ctx) const {
    return std::formatter<std::string>::format(stills::to_string(t), ctx);
  }
};
template <>
struct std::formatter<stills::Rational> : std::formatter<std::string> {
  template <class Ctx>
  auto format(stills::Rational r, Ctx& ctx) const {
    return std::formatter<std::string>::format(stills::to_string(r), ctx);
  }
};
#endif
