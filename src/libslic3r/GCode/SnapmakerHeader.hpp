#pragma once

#include <cstdio>
#include <string_view>

namespace Slic3r::SnapmakerHeader {

inline constexpr char LINE_COUNT_PREFIX[] = ";Header Start\n;file_total_lines: ";
inline constexpr char START[] = ";Header Start\n;file_total_lines: 00000000000000000000\n;header_type: 3dp\n";
inline constexpr int LINE_COUNT_WIDTH = 20;

inline bool is_snapmaker_2(std::string_view printer_model)
{
    constexpr std::string_view models[] = {"Snapmaker A250", "Snapmaker A350"};
    for (const std::string_view model : models) {
        if (printer_model == model ||
            (printer_model.size() > model.size() && printer_model.substr(0, model.size()) == model &&
             printer_model[model.size()] == ' '))
            return true;
    }
    return false;
}

// a250t F2: which extrusion the ;min_/;max_ bounds lines report. Normal prints keep the object extrusion (custom G-code such
// as the start purge is left out). Jobs made only of custom G-code (Orca's "Pressure advance - Line" calibration) fall back to
// every extrusion move. With no extrusion at all the bounds lines are left out: the touchscreen only shows them as the
// preview size (GcodeParser.java:417-446, PreviewFragment.java:351-354), it does not need them to accept or start a job.
enum class BoundsSource { Objects, AllExtrusion, None };

inline BoundsSource bounds_source(bool has_object_extrusion, bool has_any_extrusion)
{
    if (has_object_extrusion)
        return BoundsSource::Objects;
    return has_any_extrusion ? BoundsSource::AllExtrusion : BoundsSource::None;
}

inline bool update_line_count(std::FILE* file, std::size_t line_count)
{
    // Keep the field width unchanged so the viewer's final line offsets remain valid.
    char value[LINE_COUNT_WIDTH + 1];
    const int length = std::snprintf(value, sizeof(value), "%020zu", line_count);
    return length == LINE_COUNT_WIDTH && std::fseek(file, sizeof(LINE_COUNT_PREFIX) - 1, SEEK_SET) == 0 &&
           std::fwrite(value, 1, LINE_COUNT_WIDTH, file) == LINE_COUNT_WIDTH && std::fflush(file) == 0;
}

} // namespace Slic3r::SnapmakerHeader
