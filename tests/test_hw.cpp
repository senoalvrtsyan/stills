#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stills/detail/hooks.hpp>
#include <stills/interop.hpp>
#include <string>
#include <utility>
#include <vector>

#include "support/common.hpp"

using namespace testsupport;
using stills::AssetImageGenerator;
using stills::ErrorCode;
using stills::HardwareDeviceType;
using stills::HardwarePolicy;
using stills::PixelFormat;
using stills::Time;

namespace {
stills::Options hw_options(HardwarePolicy policy, PixelFormat fmt = PixelFormat::rgba) {
  stills::Options o;
  o.hardware.policy = policy;
  o.pixel_format = fmt;
  return o;
}
}  // namespace

TEST_CASE("hw: require_hardware with an unusable device fails deterministically", "[hw]") {
  stills::Options o = hw_options(HardwarePolicy::require_hardware);
  o.hardware.device_type = HardwareDeviceType::vaapi;
  o.hardware.device = "/dev/dri/does-not-exist";
  auto g = AssetImageGenerator::open(fixture("counter.mp4").string(), o);
  REQUIRE_ERROR(g, ErrorCode::hardware_unavailable);
  CHECK_FALSE(g.error().message.empty());
}

TEST_CASE("hw: prefer_hardware with an unusable device falls back to software", "[hw]") {
  stills::Options o = hw_options(HardwarePolicy::prefer_hardware);
  o.hardware.device_type = HardwareDeviceType::vaapi;
  o.hardware.device = "/dev/dri/does-not-exist";
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4").string(), o));
  CHECK_FALSE(g.active_decoder().hardware);
  CHECK_FALSE(g.active_decoder().fallback_reason.empty());
  for (int n : {0, 29, 30, 77}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    CHECK(frame_index_of(img) == n);
  }
}

TEST_CASE("hw: automatic selection is correct whether or not hardware is present", "[hw][auto]") {
  auto hw = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4").string(),
                                                 hw_options(HardwarePolicy::prefer_hardware)));
  auto sw = open_counter(sw_options(PixelFormat::rgba));
  WARN("active decoder: " << (hw.active_decoder().hardware
                                  ? "hardware (" +
                                        std::string{to_string(*hw.active_decoder().device_type)} +
                                        ")"
                                  : "software: " + hw.active_decoder().fallback_reason));
  for (int n : {0, 1, 29, 30, 31, 60, 119, 45}) {
    auto a = REQUIRE_OK(hw.image_at(Time{n, 30}));
    auto b = REQUIRE_OK(sw.image_at(Time{n, 30}));
    CHECK(frame_index_of(a) == n);
    CHECK(a.actual_time() == Time{n, 30});
    CHECK(a.is_keyframe() == (n % 30 == 0));
    REQUIRE(a.size() == b.size());
    const auto pa = REQUIRE_OK(a.to_packed_bytes());
    const auto pb = REQUIRE_OK(b.to_packed_bytes());
    REQUIRE(pa.size() == pb.size());
    int max_diff = 0;
    for (std::size_t i = 0; i < pa.size(); ++i) {
      max_diff =
          std::max(max_diff, std::abs(std::to_integer<int>(pa[i]) - std::to_integer<int>(pb[i])));
    }
    CHECK(max_diff <= 2);
  }
  if (hw.active_decoder().hardware) {
    // Hardware frames come back through av_hwframe_transfer_data; a scaled output exercises it too.
    stills::Options o = hw_options(HardwarePolicy::prefer_hardware, PixelFormat::nv12);
    o.maximum_size = stills::Size{32, 0};
    auto scaled = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4").string(), o));
    auto img = REQUIRE_OK(scaled.image_at(Time{88, 30}));
    CHECK(img.size() == stills::Size{32, 24});
    CHECK(frame_index_of(img) == 88);
  }
}

TEST_CASE("hw: require_hardware with automatic selection", "[hw][auto]") {
  const auto available = stills::interop::available_device_types();
  auto g = AssetImageGenerator::open(fixture("counter.mp4").string(),
                                     hw_options(HardwarePolicy::require_hardware));
  if (!g) {
    INFO("error: " << g.error());
    CHECK(g.error().code == ErrorCode::hardware_unavailable);
    SKIP("no usable hardware decoder on this machine (" << available.size()
                                                        << " device types probed)");
  }
  CHECK(g->active_decoder().hardware);
  REQUIRE(g->active_decoder().device_type.has_value());
  auto img = REQUIRE_OK(g->image_at(Time{59, 30}));
  CHECK(frame_index_of(img) == 59);
}

#if __has_include(<unistd.h>)
#include <fcntl.h>
#include <unistd.h>
TEST_CASE("hw: non-rewindable input never probes hardware", "[hw]") {
  SECTION("prefer_hardware falls back to software with the reason") {
    const int fd = ::open(fixture("counter.mp4").c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    auto g = REQUIRE_OK(AssetImageGenerator::open("pipe:" + std::to_string(fd),
                                                  hw_options(HardwarePolicy::prefer_hardware)));
    CHECK_FALSE(g.active_decoder().hardware);
    CHECK(g.active_decoder().fallback_reason.find("non-rewindable") != std::string::npos);
    auto img = REQUIRE_OK(g.image_at(Time{9, 30}));
    CHECK(frame_index_of(img) == 9);
    ::close(fd);
  }
  SECTION("require_hardware fails with hardware_unavailable") {
    const int fd = ::open(fixture("counter.mp4").c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    auto g = AssetImageGenerator::open("pipe:" + std::to_string(fd),
                                       hw_options(HardwarePolicy::require_hardware));
    REQUIRE_ERROR(g, ErrorCode::hardware_unavailable);
    CHECK(g.error().message.find("non-rewindable") != std::string::npos);
    ::close(fd);
  }
}
#endif

TEST_CASE("hw: the automatic policy uses software for a small H.264 clip", "[hw]") {
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4").string(),
                                                hw_options(HardwarePolicy::automatic)));
  CHECK_FALSE(g.active_decoder().hardware);
  CHECK(g.active_decoder().fallback_reason.find("automatic") != std::string::npos);
  auto img = REQUIRE_OK(g.image_at(Time{44, 30}));
  CHECK(frame_index_of(img) == 44);
}

TEST_CASE("hw: explicit device type that the build lacks", "[hw]") {
  stills::Options o = hw_options(HardwarePolicy::prefer_hardware);
  o.hardware.device_type = HardwareDeviceType::mediacodec;  // Android only
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter.mp4").string(), o));
  CHECK_FALSE(g.active_decoder().hardware);
  auto img = REQUIRE_OK(g.image_at(Time{12, 30}));
  CHECK(frame_index_of(img) == 12);
}

// A stream that switches to a profile the hardware decoder rejects mid-way: the hardware decoder is
// rebuilt once, then the generator falls back to software and keeps serving exact frames from both
// segments.
TEST_CASE("hw: a mid-stream profile switch falls back to software once, exactly",
          "[hw][fallback]") {
  // Software first: the h264 decoder re-initialises on the SPS change and both segments decode.
  {
    auto g = REQUIRE_OK(
        AssetImageGenerator::open(fixture("counter_profile_switch.ts").string(), sw_options()));
    for (int n : {50, 150, 239, 10, 200}) {
      auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
      INFO("software frame " << n);
      CHECK(frame_index_of(img) == n);
    }
  }
  stills::Options o = hw_options(HardwarePolicy::prefer_hardware);
  auto g = REQUIRE_OK(AssetImageGenerator::open(fixture("counter_profile_switch.ts").string(), o));
  if (!g.active_decoder().hardware)
    SKIP("no hardware decoder for the first segment on this machine ("
         << g.active_decoder().fallback_reason << ")");
  const std::string device = g.active_decoder().device_type_name;
  auto a = REQUIRE_OK(g.image_at(Time{50, 30}));
  CHECK(frame_index_of(a) == 50);
  CHECK(g.active_decoder().hardware);
  // Into the 4:4:4 segment: the hardware decoder declines it. VAAPI accepts High only.
  auto b = REQUIRE_OK(g.image_at(Time{150, 30}));
  CHECK(frame_index_of(b) == 150);
  const auto after = g.active_decoder();
  WARN("device " << device << ": after the switch hardware=" << after.hardware << " reason='"
                 << after.fallback_reason << "'");
  if (!after.hardware) CHECK_FALSE(after.fallback_reason.empty());  // rebuilt once, then software
  for (int n : {239, 10, 200, 119, 120}) {
    auto img = REQUIRE_OK(g.image_at(Time{n, 30}));
    INFO("frame " << n << " after the switch");
    CHECK(frame_index_of(img) == n);
  }
  // require_hardware: the same fault is an error, not a silent software frame.
  stills::Options strict = hw_options(HardwarePolicy::require_hardware);
  auto h =
      REQUIRE_OK(AssetImageGenerator::open(fixture("counter_profile_switch.ts").string(), strict));
  auto r = h.image_at(Time{150, 30});
  if (!r)
    CHECK(r.error().code == ErrorCode::hardware_unavailable);
  else
    CHECK(frame_index_of(*r) == 150);  // a decoder that handles 4:4:4 (CUDA does) simply succeeds
}

// ---- the mid-stream hardware fault: rebuild once, then fall back -----------------------------
// A driver that loses a session, or a surface download that fails with EIO, is usually transient,
// so the library re-creates the same hardware decoder once before giving up on it and rebuilding
// in software. No driver here fails on demand, so the fault is injected at the one libav call that
// reports it — av_hwframe_transfer_data, replaced through detail::DecoderHooks (see
// include/stills/detail/hooks.hpp for why that call and not a per-packet one). Everything else in
// the path is the shipped code.
namespace {

int g_transfers_to_fail = 0;  ///< how many of the next transfers the hook fails
int g_transfers_failed = 0;

int failing_transfer(AVFrame* dst, const AVFrame* src, int flags) {
  if (g_transfers_to_fail != 0) {
    if (g_transfers_to_fail > 0) --g_transfers_to_fail;
    ++g_transfers_failed;
    return AVERROR(EIO);  // what a lost VAAPI/NVDEC surface actually reports
  }
  return av_hwframe_transfer_data(dst, src, flags);
}

/// A generator on a real hardware decoder, or nullopt when this machine has none.
std::optional<AssetImageGenerator> open_hw(
    HardwarePolicy policy = HardwarePolicy::prefer_hardware) {
  auto g = AssetImageGenerator::open(fixture("counter.mp4").string(), hw_options(policy));
  if (!g || !g->active_decoder().hardware) return std::nullopt;
  return std::move(*g);
}

}  // namespace

TEST_CASE("hw: a transient download failure rebuilds the decoder once and still returns the frame",
          "[hw][fallback]") {
  if (!open_hw()) SKIP("no hardware decoder on this machine");
  g_transfers_to_fail = 0;
  g_transfers_failed = 0;

  SECTION("one failure: rebuilt once, still hardware, right frame") {
    auto g = open_hw();
    REQUIRE(g.has_value());
    {
      stills::detail::ScopedHook hook{{&failing_transfer}};
      g_transfers_to_fail = 1;  // the next transfer only
      auto img = g->image_at(Time{47, 30});
      INFO("error: " << (img ? std::string{"none"} : to_string(img.error())));
      REQUIRE(img.has_value());
      CHECK(frame_index_of(*img) == 47);
    }
    CHECK(g_transfers_failed == 1);  // one download failed; the retry after the rebuild did not
    CHECK(g->active_decoder().hardware);  // the fault was transient: hardware is kept
    auto again = REQUIRE_OK(g->image_at(Time{11, 30}));
    CHECK(frame_index_of(again) == 11);
    CHECK(g_transfers_failed == 1);  // no further fault, so no further rebuild
  }

  SECTION("every transfer fails: rebuilt once, then software, and the frame still arrives") {
    auto g = open_hw(HardwarePolicy::prefer_hardware);
    REQUIRE(g.has_value());
    stills::Image img = [&] {
      stills::detail::ScopedHook hook{{&failing_transfer}};
      g_transfers_to_fail = -1;  // forever
      auto r = g->image_at(Time{52, 30});
      g_transfers_to_fail = 0;
      INFO("error: " << (r ? std::string{"none"} : to_string(r.error())));
      REQUIRE(r.has_value());
      return std::move(*r);
    }();
    CHECK(frame_index_of(img) == 52);  // the consumer gets their frame, from software
    // Two downloads were attempted and failed: the original and the one after the single hardware
    // rebuild. A third would mean the decoder was rebuilt more than once before falling back.
    CHECK(g_transfers_failed == 2);
    CHECK_FALSE(g->active_decoder().hardware);
    INFO("fallback_reason: " << g->active_decoder().fallback_reason);
    CHECK(g->active_decoder().fallback_reason.find("hardware decoder failed during decoding") !=
          std::string::npos);
    // The generator stays usable on the software decoder.
    auto later = REQUIRE_OK(g->image_at(Time{7, 30}));
    CHECK(frame_index_of(later) == 7);
  }

  SECTION("require_hardware: no software fallback, a clear error, and the generator survives") {
    auto probe = AssetImageGenerator::open(fixture("counter.mp4").string(),
                                           hw_options(HardwarePolicy::require_hardware));
    if (!probe) SKIP("require_hardware cannot be satisfied here");
    {
      stills::detail::ScopedHook hook{{&failing_transfer}};
      g_transfers_to_fail = -1;
      auto r = probe->image_at(Time{30, 30});
      g_transfers_to_fail = 0;
      REQUIRE_FALSE(r.has_value());
      INFO("error: " << r.error());
      CHECK(r.error().code == ErrorCode::hardware_unavailable);
    }
    CHECK(g_transfers_failed == 2);          // original + one after the single rebuild
    CHECK(probe->active_decoder().hardware);  // never silently downgraded
    auto ok = REQUIRE_OK(probe->image_at(Time{30, 30}));  // works again once the driver does
    CHECK(frame_index_of(ok) == 30);
  }

  g_transfers_to_fail = 0;
}
