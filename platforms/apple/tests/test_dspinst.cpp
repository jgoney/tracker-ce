// Checks the PLATFORM_APPLE C implementations in dspinst.h against a reference model of the
// ARM instruction semantics. All reference arithmetic wraps (unsigned) like the hardware.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>
#include <vector>

#include "dspinst.h"

namespace
{
int32_t lo(uint32_t v) { return (int16_t)(v & 0xFFFF); }
int32_t hi(uint32_t v) { return (int16_t)(v >> 16); }
uint32_t u(int64_t v) { return (uint32_t)(uint64_t)v; }
int32_t s32(uint64_t v) { return (int32_t)(uint32_t)v; }
int64_t satTo(int64_t v, int bits)
{
    int64_t mx = (1ll << (bits - 1)) - 1, mn = -(1ll << (bits - 1));
    return v > mx ? mx : (v < mn ? mn : v);
}
uint32_t pk(int64_t h, int64_t l) { return ((uint32_t)h << 16) | ((uint32_t)l & 0xFFFF); }

std::vector<uint32_t> values()
{
    std::vector<uint32_t> v = {0,          1,          0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80008000,
                               0x7FFF7FFF, 0x80007FFF, 0x7FFF8000, 0xFFFF0000, 0x0000FFFF, 0x00010001,
                               0xFFFFFFFE, 2,          0x00008000, 0x80000001};
    std::mt19937 g(12345);
    for (int i = 0; i < 400; i++)
        v.push_back(g());
    return v;
}
} // namespace

TEST_CASE("SMLALD does not lose the 2^31 carry (repro)")
{
    REQUIRE(multiply_accumulate_16tx16t_add_16bx16b(0, 0x80008000u, 0x80008000u) == 0x0000000080000000LL);
    REQUIRE(multiply_16tx16t_add_16bx16b(0x80008000u, 0x80008000u) == (int32_t)0x80000000);
}

TEST_CASE("dspinst two-operand intrinsics match ARM reference")
{
    auto vals = values();
    for (uint32_t a : vals)
        for (uint32_t b : vals)
        {
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            CHECK(signed_multiply_32x16b(sa, b) == (int32_t)(((int64_t)sa * lo(b)) >> 16));
            CHECK(signed_multiply_32x16t(sa, b) == (int32_t)(((int64_t)sa * hi(b)) >> 16));
            CHECK(multiply_32x32_rshift32(sa, sb) == (int32_t)(((int64_t)sa * sb) >> 32));
            CHECK(multiply_32x32_rshift32_rounded(sa, sb) ==
                  s32(((uint64_t)((int64_t)sa * sb) + 0x80000000ull) >> 32));
            CHECK(pack_16t_16t(sa, sb) == ((a & 0xFFFF0000u) | (b >> 16)));
            CHECK(pack_16t_16b(sa, sb) == ((a & 0xFFFF0000u) | (b & 0xFFFF)));
            CHECK(pack_16b_16b(sa, sb) == pk(lo(a), lo(b)));
            CHECK(signed_add_16_and_16(a, b) == pk(satTo(hi(a) + hi(b), 16), satTo(lo(a) + lo(b), 16)));
            CHECK(signed_subtract_16_and_16(sa, sb) == (int32_t)pk(satTo(hi(a) - hi(b), 16), satTo(lo(a) - lo(b), 16)));
            CHECK(signed_halving_add_16_and_16(sa, sb) == (int32_t)pk((hi(a) + hi(b)) >> 1, (lo(a) + lo(b)) >> 1));
            CHECK(signed_halving_subtract_16_and_16(sa, sb) == (int32_t)pk((hi(a) - hi(b)) >> 1, (lo(a) - lo(b)) >> 1));
            CHECK(logical_and(a, b) == (a & b));
            CHECK(multiply_16tx16t_add_16bx16b(a, b) == s32((uint64_t)(int64_t)(lo(a) * lo(b)) + (uint64_t)(int64_t)(hi(a) * hi(b))));
            CHECK(multiply_16tx16b_add_16bx16t(a, b) == s32((uint64_t)(int64_t)(lo(a) * hi(b)) + (uint64_t)(int64_t)(hi(a) * lo(b))));
            CHECK(multiply_16bx16b(a, b) == lo(a) * lo(b));
            CHECK(multiply_16bx16t(a, b) == lo(a) * hi(b));
            CHECK(multiply_16tx16b(a, b) == hi(a) * lo(b));
            CHECK(multiply_16tx16t(a, b) == hi(a) * hi(b));
            CHECK(substract_32_saturate(a, b) == (int32_t)satTo((int64_t)sa - sb, 32));
            CHECK(multiply_32x32_rshift(sa, sb, 3) == (int32_t)(((int64_t)sa * sb) >> 28));
        }
}

TEST_CASE("dspinst three-operand intrinsics match ARM reference")
{
    auto vals = values();
    std::vector<uint32_t> sums(vals.begin(), vals.begin() + 40);
    for (uint32_t s : sums)
        for (uint32_t a : vals)
            for (size_t k = 0; k < vals.size(); k += 3)
            {
                uint32_t b = vals[k];
                int32_t ss = (int32_t)s, sa = (int32_t)a, sb = (int32_t)b;
                CHECK(multiply_accumulate_32x32_rshift32_rounded(ss, sa, sb) ==
                      s32((((uint64_t)s << 32) + (uint64_t)((int64_t)sa * sb) + 0x80000000ull) >> 32));
                CHECK(multiply_subtract_32x32_rshift32_rounded(ss, sa, sb) ==
                      s32((((uint64_t)s << 32) - (uint64_t)((int64_t)sa * sb) + 0x80000000ull) >> 32));
                CHECK(signed_multiply_accumulate_32x16b(ss, sa, b) == s32((uint64_t)s + (uint64_t)(((int64_t)sa * lo(b)) >> 16)));
                CHECK(signed_multiply_accumulate_32x16t(ss, sa, b) == s32((uint64_t)s + (uint64_t)(((int64_t)sa * hi(b)) >> 16)));
                int64_t s64 = (int64_t)(((uint64_t)s << 32) | a);
                CHECK(multiply_accumulate_16tx16t_add_16bx16b(s64, a, b) ==
                      (int64_t)((uint64_t)s64 + (uint64_t)(int64_t)(lo(a) * lo(b)) + (uint64_t)(int64_t)(hi(a) * hi(b))));
                CHECK(multiply_accumulate_16tx16b_add_16bx16t(s64, a, b) ==
                      (int64_t)((uint64_t)s64 + (uint64_t)(int64_t)(lo(a) * hi(b)) + (uint64_t)(int64_t)(hi(a) * lo(b))));
            }
}

TEST_CASE("dspinst saturation helpers")
{
    for (int32_t v : {0, 1, -1, 32767, 32768, -32768, -32769, INT32_MAX, INT32_MIN})
    {
        CHECK(saturate16(v) == (int16_t)satTo(v, 16));
        for (int bits : {8, 12, 16, 24})
            for (int sh : {0, 1, 8, 15})
                CHECK(signed_saturate_rshift(v, bits, sh) == (int32_t)satTo(v >> sh, bits));
    }
    CHECK(get_q_psr() == 0);
}
