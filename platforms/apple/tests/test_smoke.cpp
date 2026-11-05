#include <catch2/catch_test_macros.hpp>
#include <string>

#include "Tracker.h"

TEST_CASE("core library links")
{
    REQUIRE(std::string(Tracker::getVersion()).size() > 0);
}
