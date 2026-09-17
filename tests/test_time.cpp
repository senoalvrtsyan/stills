#include <chrono>
#include <limits>
#include <random>
#include <ratio>
#include <stills/interop.hpp>
#include <type_traits>

#include "support/common.hpp"

using namespace std::chrono_literals;
using stills::Rational;
using stills::Time;
using stills::TimeRounding;

TEST_CASE("Time: construction and chrono conversion are exact", "[time]") {
  constexpr Time a = 1500ms;
  STATIC_REQUIRE(a.value() == 1500);
  STATIC_REQUIRE(a.timescale() == 1000);
  STATIC_REQUIRE(Time::seconds(1.5) == a);
  STATIC_REQUIRE(Time{} == Time::zero());
  STATIC_REQUIRE(Time{}.is_finite());
  STATIC_REQUIRE(Time{1, 2} == Time{2, 4});
  STATIC_REQUIRE(Time{7, 2}.to_duration<std::chrono::milliseconds>() == 3500ms);
  STATIC_REQUIRE(Time{1, 0}.kind() == Time::Kind::invalid);
  REQUIRE(Time::from_duration(2min) == Time{120, 1});
}

TEST_CASE("Time: arithmetic and ordering", "[time]") {
  STATIC_REQUIRE(Time{1, 3} + Time{1, 6} == Time{1, 2});
  STATIC_REQUIRE(Time{1, 2} - Time{1, 3} == Time{1, 6});
  STATIC_REQUIRE(Time{3, 4} * 2 == Time{3, 2});
  STATIC_REQUIRE((-Time{1, 4}).value() == -1);
  STATIC_REQUIRE(Time::invalid() < Time::negative_infinity());
  STATIC_REQUIRE(Time::negative_infinity() < Time{-1'000'000, 1});
  STATIC_REQUIRE(Time{1, 1} < Time::positive_infinity());
  STATIC_REQUIRE((Time::positive_infinity() + Time{1, 1}).is_positive_infinity());
  STATIC_REQUIRE(!(Time::positive_infinity() + Time::negative_infinity()).is_valid());
  STATIC_REQUIRE(!(Time::invalid() + Time{1, 1}).is_valid());
  STATIC_REQUIRE(Time{1, 1} <= Time{1000, 1000});
}

TEST_CASE("Time: timestamps and rescaling agree with libav", "[time]") {
  STATIC_REQUIRE(Time{29, 30}.to_timestamp({1, 15360}) == 29 * 512);
  STATIC_REQUIRE(Time::from_timestamp(512, {1, 15360}) == Time{1, 30});
  STATIC_REQUIRE(Time::from_timestamp(3, {1001, 30000}) == Time{3003, 30000});
  STATIC_REQUIRE(
      !Time::from_timestamp(std::numeric_limits<std::int64_t>::min(), {1, 1000}).is_valid());
  STATIC_REQUIRE(Time{1, 3}.converted(1000, TimeRounding::down).value() == 333);
  STATIC_REQUIRE(Time{1, 3}.converted(1000, TimeRounding::up).value() == 334);
  STATIC_REQUIRE(Time{2, 3}.converted(1000, TimeRounding::nearest).value() == 667);
  STATIC_REQUIRE(Time{-1, 3}.converted(1000, TimeRounding::zero).value() == -333);

  std::mt19937_64 rng{42};
  std::uniform_int_distribution<std::int64_t> values{-5'000'000'000LL, 5'000'000'000LL};
  std::uniform_int_distribution<std::int32_t> scales{1, 200'000};
  for (int i = 0; i < 2000; ++i) {
    const Time t{values(rng), scales(rng)};
    const Rational tb{1, scales(rng)};
    const std::int64_t ours = t.to_timestamp(tb, TimeRounding::nearest);
    const std::int64_t theirs = av_rescale_q_rnd(t.value(), AVRational{1, t.timescale()},
                                                 stills::interop::to_av(tb), AV_ROUND_NEAR_INF);
    REQUIRE(ours == theirs);
    const Time u{values(rng), scales(rng)};
    const int cmp = av_compare_ts(t.value(), AVRational{1, t.timescale()}, u.value(),
                                  AVRational{1, u.timescale()});
    const auto ord = t <=> u;
    REQUIRE((cmp < 0) == (ord < 0));
    REQUIRE((cmp > 0) == (ord > 0));
  }
}

TEST_CASE("TimeRange and Tolerance", "[time]") {
  constexpr stills::TimeRange r{Time{1, 1}, Time{2, 1}};
  STATIC_REQUIRE(r.end() == Time{3, 1});
  STATIC_REQUIRE(r.contains(Time{2, 1}));
  STATIC_REQUIRE(!r.contains(Time{3, 1}));
  STATIC_REQUIRE(stills::Tolerance::exact().is_exact());
  STATIC_REQUIRE(stills::Tolerance::any().before.is_positive_infinity());
  STATIC_REQUIRE(stills::Tolerance::symmetric(Time{1, 2}).after == Time{1, 2});
  REQUIRE(to_string(Time{3, 2}) == "1.500000s (3/2)");
  REQUIRE(to_string(Time::positive_infinity()) == "+inf");
}

// Three arithmetic gaps that returned a wrong value in silence where every other overflow path in
// this header returns Time::invalid().
TEST_CASE("time: whole-second durations convert, scaling is exact, to_duration says zero",
          "[time]") {
  using namespace std::chrono_literals;
  // Minutes and hours are exact multiples of a second, so they convert like seconds and
  // milliseconds do. They used not to: the implicit conversion required a period of 1/N.
  STATIC_REQUIRE(std::is_convertible_v<std::chrono::minutes, Time>);
  STATIC_REQUIRE(std::is_convertible_v<std::chrono::hours, Time>);
  CHECK(Time{2min} == Time{120, 1});
  CHECK(Time{1h} == Time{3600, 1});
  CHECK(Time{90s} == Time{1min} + Time{30s});
  CHECK(Time{std::chrono::hours{std::numeric_limits<std::int64_t>::max()}}.is_valid() == false);

  // Scaling: a 64-bit factor used to narrow to int32 and wrap.
  CHECK(Time::seconds(1) * std::int64_t{3000000000} == Time{3000000000, 1});
  CHECK((Time::seconds(1) * std::int64_t{3000000000}).to_seconds() > 0);
  CHECK(Time{1, 30} * 3 == Time{3, 30});
  CHECK((Time{std::numeric_limits<std::int64_t>::max(), 1} * 2).is_valid() == false);
  // `Time * 1.5` no longer compiles at all (the floating-point overloads are deleted), where it
  // used to truncate to `Time * 1` with no warning under -Wall -Wextra.

  // to_duration: zero when it does not fit, and no overflow computing the denominator.
  CHECK(Time{std::numeric_limits<std::int64_t>::max(), 1}.to_duration<std::chrono::nanoseconds>() ==
        std::chrono::nanoseconds{0});
  using Huge = std::chrono::duration<std::int64_t, std::ratio<INTMAX_MAX, 1>>;
  CHECK(Time{1, 1}.to_duration<Huge>() == Huge{0});
  CHECK(Time{1500, 1000}.to_duration<std::chrono::milliseconds>() == 1500ms);
}

// Rational compares by value, and one with no denominator is not equal to every other one.
TEST_CASE("time: Rational compares cross-multiplied and orders the invalid ones apart", "[time]") {
  CHECK(stills::Rational{1, 2} == stills::Rational{2, 4});
  CHECK(stills::Rational{1, 2} < stills::Rational{2, 3});
  CHECK_FALSE(stills::Rational{1, 0} == stills::Rational{2, 0});
  CHECK(stills::Rational{1, 0} < stills::Rational{0, 1});
  CHECK_FALSE(stills::Rational{1, 0}.is_valid());
}
