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

namespace
{
constexpr AudioDeviceID kA = 0xEE000001, kX = 0xEE000002, kY = 0xEE000003, kD = 0xEE000004, kB = 0xEE000005;

struct Fixture
{
    FakeTracker fake;
    AudioRuntime rt{ &fake, nullptr };
    std::mutex m;
    std::vector<std::pair<Status, std::string>> statuses;
    std::vector<AudioDeviceID> attempts;
    std::atomic<int> sinkCalls{ 0 };
    std::atomic<bool> killedA{ false };
    CoreAudioOutputTestHooks hooks;

    Fixture()
    {
        hooks.fakeOpen = [](AudioDeviceID id) { return id >= kA && id <= kB; };
        hooks.defaultDevice = [] { return kD; };
        hooks.builtinDevice = [] { return kB; };
        hooks.alive = [this](AudioDeviceID id) { return !(killedA && id == kA); };
        hooks.workgroupSink = [this](void*) { sinkCalls++; };
    }
    void attach(CoreAudioOutput& o)
    {
        o.setOnStatus([this](Status s, std::string n)
        {
            std::lock_guard<std::mutex> g(m);
            statuses.emplace_back(s, n);
        });
        CoreAudioOutputTestAccess::setHooks(o, hooks);
    }
    void note(AudioDeviceID id)
    {
        std::lock_guard<std::mutex> g(m);
        attempts.push_back(id);
    }
    size_t statusCount()
    {
        std::lock_guard<std::mutex> g(m);
        return statuses.size();
    }
    Status lastStatus()
    {
        std::lock_guard<std::mutex> g(m);
        return statuses.back().first;
    }
    bool attempted(AudioDeviceID id)
    {
        std::lock_guard<std::mutex> g(m);
        return std::count(attempts.begin(), attempts.end(), id) != 0;
    }
};

double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void blockUntil(const std::shared_ptr<std::atomic<bool>>& release)
{
    while (!release->load())
        std::this_thread::sleep_for(5ms);
}
} // namespace

// A wedged previous device AND a hanging new device: still one 2.5 s bound for the call.
TEST_CASE("test_open_with_wedged_previous_and_hanging_new_returns_in_bound", "[coreaudio]")
{
    Fixture f;
    auto release = std::make_shared<std::atomic<bool>>(false);
    f.hooks.beforeTeardown = [release](AudioDeviceID id) { if (id == kA) blockUntil(release); };
    f.hooks.beforeOpen = [&f, release](AudioDeviceID id)
    {
        f.note(id);
        if (id == kX)
            blockUntil(release);
    };
    auto out = std::make_unique<CoreAudioOutput>(f.rt);
    f.attach(*out);
    REQUIRE(out->open(kA));

    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(!out->open(kX)); // teardown of A (2 s) + open of X (2 s) would be 4 s
    const double ms = msSince(t0);
    REQUIRE(ms > 2300);
    REQUIRE(ms < 2800);

    // The command completes later: both devices were wedged, so the fallback lands on D.
    const size_t n = f.statusCount();
    REQUIRE(waitUntil([&] { return f.statusCount() > n && out->deviceId() == kD; }, 8000ms));
    REQUIRE(f.lastStatus() == Status::FellBack);
    REQUIRE(CoreAudioOutputTestAccess::isUnresponsive(*out, kA));
    REQUIRE(CoreAudioOutputTestAccess::isUnresponsive(*out, kX));
    REQUIRE(!CoreAudioOutputTestAccess::isUnresponsive(*out, kD));
    const auto t1 = std::chrono::steady_clock::now();
    out.reset();
    REQUIRE(msSince(t1) < 2500);
    release->store(true);
}

// open(Y) while a fallback is in flight: the user's choice wins, and Y is not blamed.
TEST_CASE("test_open_during_fallback_user_choice_wins", "[coreaudio]")
{
    Fixture f;
    f.hooks.beforeOpen = [&f](AudioDeviceID id)
    {
        f.note(id);
        if (id == kD)
            std::this_thread::sleep_for(1500ms); // a slow (but not hung) default device
    };
    auto out = std::make_unique<CoreAudioOutput>(f.rt);
    f.attach(*out);
    REQUIRE(out->open(kA));
    REQUIRE(out->start());
    REQUIRE(out->isRunning());

    f.killedA = true;
    CoreAudioOutputTestAccess::postDeviceDead(*out); // fallback begins: opens D (1.5 s)
    REQUIRE(waitUntil([&] { return f.attempted(kD); }, 2000ms));
    REQUIRE(out->open(kY)); // queued behind the fallback; returns within 2.5 s
    REQUIRE(out->deviceId() == kY);
    REQUIRE(!CoreAudioOutputTestAccess::isUnresponsive(*out, kY));
    REQUIRE(!CoreAudioOutputTestAccess::isUnresponsive(*out, kD));
    std::this_thread::sleep_for(300ms); // nothing replaces Y afterwards
    REQUIRE(out->deviceId() == kY);
    REQUIRE(f.lastStatus() == Status::Opened);
}

// Destroying the output while a fallback is in flight: nothing runs after the destructor returns.
TEST_CASE("test_destructor_during_fallback_runs_nothing_afterwards", "[coreaudio]")
{
    Fixture f;
    f.hooks.beforeOpen = [&f](AudioDeviceID id)
    {
        f.note(id);
        if (id == kD)
            std::this_thread::sleep_for(3000ms); // longer than the 2 s op bound
    };
    auto out = std::make_unique<CoreAudioOutput>(f.rt);
    f.attach(*out);
    REQUIRE(out->open(kA));
    REQUIRE(waitUntil([&] { return f.statusCount() >= 1; }, 1000ms));
    const int sinkAfterOpen = f.sinkCalls.load();
    REQUIRE(sinkAfterOpen == 1);

    f.killedA = true;
    CoreAudioOutputTestAccess::postDeviceDead(*out);
    REQUIRE(waitUntil([&] { return f.attempted(kD); }, 2000ms));
    const auto t0 = std::chrono::steady_clock::now();
    out.reset(); // mid-fallback
    REQUIRE(msSince(t0) < 2500);
    const size_t statusAtReturn = f.statusCount();
    const int sinkAtReturn = f.sinkCalls.load();
    std::this_thread::sleep_for(4000ms); // the abandoned work finishes in the background
    REQUIRE(f.statusCount() == statusAtReturn); // no onStatus after destruction
    REQUIRE(f.sinkCalls.load() == sinkAtReturn); // no setWorkgroup after destruction
    REQUIRE(!f.attempted(kB));                   // the fallback chain stood down
}

// A start that hangs is a dead device: fall back, and say so.
TEST_CASE("test_start_hang_falls_back_with_status", "[coreaudio]")
{
    Fixture f;
    auto release = std::make_shared<std::atomic<bool>>(false);
    f.hooks.beforeStart = [release](AudioDeviceID id) { if (id == kA) blockUntil(release); };
    auto out = std::make_unique<CoreAudioOutput>(f.rt);
    f.attach(*out);
    REQUIRE(out->open(kA));
    const size_t n = f.statusCount();
    out->start(); // A's start wedges; the call returns by its deadline
    REQUIRE(waitUntil([&] { return f.statusCount() > n && f.lastStatus() == Status::FellBack; }, 6000ms));
    REQUIRE(CoreAudioOutputTestAccess::isUnresponsive(*out, kA));
    REQUIRE(out->deviceId() == kD);
    REQUIRE(out->isRunning()); // the run intent carried over to the new device
    out.reset();
    release->store(true);
}
