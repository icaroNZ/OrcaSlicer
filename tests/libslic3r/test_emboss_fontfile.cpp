#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Emboss.hpp"

#include <memory>
#include <vector>

using namespace Slic3r;

namespace {
Emboss::FontFile make_font_file(size_t size, std::vector<Emboss::FontFile::Info> infos)
{
    return Emboss::FontFile(std::make_unique<std::vector<unsigned char>>(size, 0x42), std::move(infos));
}
} // namespace

TEST_CASE("FontFile equality compares every face metric (#1009)", "[Emboss]")
{
    const Emboss::FontFile::Info face{800, -200, 50, 1000};
    const Emboss::FontFile       a = make_font_file(16, {face});
    const Emboss::FontFile       b = make_font_file(16, {face});
    // Used to be false: descent/linegap were compared with '=='.
    CHECK(a == b);

    Emboss::FontFile::Info other_descent = face;
    other_descent.descent                = -180;
    CHECK_FALSE(a == make_font_file(16, {other_descent}));

    Emboss::FontFile::Info other_linegap = face;
    other_linegap.linegap                = 0;
    CHECK_FALSE(a == make_font_file(16, {other_linegap}));

    CHECK_FALSE(a == make_font_file(32, {face}));       // different data size
    CHECK_FALSE(a == make_font_file(16, {face, face})); // different face count, no out-of-range read
}
