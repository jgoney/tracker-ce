// CoreAudioOutput logic that needs no audio hardware: channel routing and the no-hang guarantees
// (an injected device open that blocks forever).
#include <catch2/catch_test_macros.hpp>

#include "CoreAudioOutput.h"
#include "test_coreaudio_common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace ca_test;
using Status = CoreAudioOutput::Status;

TEST_CASE("test_route_stereo_pair_in_8_channels", "[coreaudio]")
{
    const float l[4] = { 1, 2, 3, 4 }, r[4] = { -1, -2, -3, -4 };
    float out[4 * 8];
    for (float& f : out)
        f = 99.f; // route() must overwrite everything
    CoreAudioOutput::route(l, r, 4, out, 8, 3, 6);
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 8; c++)
        {
            const float want = c == 3 ? l[i] : c == 6 ? r[i] : 0.f;
            REQUIRE(out[i * 8 + c] == want);
        }
}

TEST_CASE("test_route_mono_mixes_and_out_of_range_is_skipped", "[coreaudio]")
{
    const float l[2] = { 1, 0.5f }, r[2] = { 3, 0.5f };
    float mono[2];
    CoreAudioOutput::route(l, r, 2, mono, 1, 0, 0);
    REQUIRE(mono[0] == 2.f);
    REQUIRE(mono[1] == 0.5f);

    float two[4];
    CoreAudioOutput::route(l, r, 2, two, 2, 1, 5); // right channel does not exist
    REQUIRE(two[0] == 0.f);
    REQUIRE(two[1] == 1.f);
    REQUIRE(two[2] == 0.f);
    REQUIRE(two[3] == 0.5f);
}

TEST_CASE("test_open_hang_returns_and_falls_back", "[coreaudio]")
{
    constexpr AudioDeviceID X = 0xFFFF0001, D = 0xFFFF0002, B = 0xFFFF0003; // none exists
    struct Gate
    {
        std::mutex m;
        std::condition_variable cv;
        bool release = false;
        std::vector<AudioDeviceID> attempts;
    };
    auto gate = std::make_shared<Gate>();

    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    std::mutex sm;
    std::vector<std::pair<Status, std::string>> statuses;

    auto outp = std::make_unique<CoreAudioOutput>(rt);
    {
        CoreAudioOutput& out = *outp;
        out.setOnStatus(
          [&](Status s, std::string n)
          {
              std::lock_guard<std::mutex> g(sm);
              statuses.emplace_back(s, n);
          });
        CoreAudioOutputTestHooks h;
        h.beforeOpen = [gate](AudioDeviceID id)
        {
            std::unique_lock<std::mutex> g(gate->m);
            gate->attempts.push_back(id);
            if (id == X) // the "hung driver": blocks until the test releases it
                gate->cv.wait(g, [&] { return gate->release; });
        };
        h.defaultDevice = [] { return D; };
        h.builtinDevice = [] { return B; };
        h.alive = [](AudioDeviceID) { return true; };
        CoreAudioOutputTestAccess::setHooks(out, h);

        const auto t1 = std::chrono::steady_clock::now();
        const bool ok = out.open(X);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        REQUIRE(!ok);
        REQUIRE(ms > 1500);
        REQUIRE(ms < 2500); // the 2 s timeout, not forever
        REQUIRE(CoreAudioOutputTestAccess::isUnresponsive(out, X));
        REQUIRE(!out.isOpen());
        REQUIRE(!out.isRunning());

        // The fallback chain (default, then built-in) ran after the open failed; neither fake
        // device exists, so the result is Failed (with hardware it would be FellBack).
        REQUIRE(waitUntil([&] { std::lock_guard<std::mutex> g(sm); return !statuses.empty(); }, 5000ms));
        {
            std::lock_guard<std::mutex> g(sm);
            REQUIRE(statuses.back().first == Status::Failed);
        }
        {
            std::lock_guard<std::mutex> g(gate->m);
            REQUIRE(gate->attempts == std::vector<AudioDeviceID>{ X, D, B });
        }

        // An unresponsive device is skipped: no second 2 s wait.
        const auto t2 = std::chrono::steady_clock::now();
        REQUIRE(!out.open(X));
        REQUIRE(std::chrono::steady_clock::now() - t2 < 500ms);
        {
            std::lock_guard<std::mutex> g(gate->m);
            REQUIRE(std::count(gate->attempts.begin(), gate->attempts.end(), X) == 1);
        }

    }
    // The wedged worker is still blocked inside the "driver": the destructor must not wait for it.
    const auto t3 = std::chrono::steady_clock::now();
    outp.reset();
    const double destroyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t3).count();
    {
        std::lock_guard<std::mutex> g(gate->m);
        gate->release = true;
    }
    gate->cv.notify_all();
    REQUIRE(destroyMs < 2500);
}
