// Unit tests for the Snapmaker 2.0 touchscreen HTTP protocol helpers used by the "snapmaker2" print host.
// The helpers have no wx / curl / boost dependency, so this suite builds with only Catch2 and nlohmann/json.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <clocale>
#include <cmath>
#include <string>
#include <vector>

#include "slic3r/Utils/Snapmaker2Protocol.hpp"

using namespace Slic3r::Snapmaker2Protocol;
using Catch::Approx;

TEST_CASE("make_url adds the default touchscreen port and scheme", "[Snapmaker2]")
{
    CHECK(make_url("192.168.1.20", "api/v1/connect") == "http://192.168.1.20:8080/api/v1/connect");
    CHECK(make_url("192.168.1.20", "/api/v1/connect") == "http://192.168.1.20:8080/api/v1/connect");
    CHECK(make_url("  192.168.1.20  ", "api/v1/status") == "http://192.168.1.20:8080/api/v1/status");
    CHECK(make_url("a250t.local", "api/v1/status") == "http://a250t.local:8080/api/v1/status");
}

TEST_CASE("make_url keeps an explicit port and scheme", "[Snapmaker2]")
{
    CHECK(make_url("192.168.1.20:8081", "api/v1/connect") == "http://192.168.1.20:8081/api/v1/connect");
    CHECK(make_url("http://192.168.1.20", "api/v1/connect") == "http://192.168.1.20:8080/api/v1/connect");
    CHECK(make_url("http://192.168.1.20:8080/", "api/v1/connect") == "http://192.168.1.20:8080/api/v1/connect");
    CHECK(make_url("HTTP://192.168.1.20:9000//", "api/v1/connect") == "http://192.168.1.20:9000/api/v1/connect");
    CHECK(make_url("https://printer:1234", "x") == "https://printer:1234/x");
    CHECK(make_url("[fe80::1]", "x") == "http://[fe80::1]:8080/x");
    CHECK(make_url("[fe80::1]:9000", "x") == "http://[fe80::1]:9000/x");
    CHECK(make_url("192.168.1.20", "") == "http://192.168.1.20:8080/");
}

TEST_CASE("form bodies match what Luban sends", "[Snapmaker2]")
{
    // Luban sends '' without a token; Orca's Http needs a non-empty POST body, and the touchscreen treats
    // "token=" like no token (AuthRequestHandler -> createSession()).
    CHECK(connect_body("") == "token=");
    CHECK(connect_body("0b0d6e0c-2f8a-4b8e-9a51-1d2a3b4c5d6e") == "token=0b0d6e0c-2f8a-4b8e-9a51-1d2a3b4c5d6e");
    CHECK(form_body({ { "token", "abc" } }) == "token=abc");
    CHECK(form_body({ { "token", "abc" }, { "code", "G28 X" } }) == "token=abc&code=G28%20X");
    CHECK(form_body({}) == "");
    CHECK(url_encode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
    CHECK(url_encode("AZaz09-_.~") == "AZaz09-_.~");
    CHECK(status_path("t 1") == "api/v1/status?token=t%201");
}

TEST_CASE("HTTP statuses map to touchscreen session states", "[Snapmaker2]")
{
    CHECK(classify(200) == Reply::Ok);
    CHECK(classify(204) == Reply::PendingApproval); // BaseRequestHandler: session pending -> SC_NO_CONTENT
    CHECK(classify(400) == Reply::BadRequest);      // no token parameter
    CHECK(classify(401) == Reply::NotAuthorised);   // token is not the current session
    CHECK(classify(403) == Reply::Busy);            // connect: someone else is trying to connect
    CHECK(classify(409) == Reply::Conflict);        // prepare_print wrong type / start_print refused
    CHECK(classify(500) == Reply::HttpError);
    CHECK(classify(504) == Reply::GatewayTimeout);  // pause/resume/stop: no controller reply within 30 s
    CHECK(std::string(reply_name(Reply::GatewayTimeout)) == "gateway-timeout");
    CHECK(classify(0) == Reply::NetworkError);
    CHECK(std::string(reply_name(Reply::PendingApproval)) == "pending-approval");
}

TEST_CASE("connect reply is parsed like Luban does", "[Snapmaker2]")
{
    const auto info = parse_connect(
        R"({"token":"5f1c0f9e-aaaa-bbbb-cccc-0123456789ab","readonly":false,"series":"A250","headType":1,"hasEnclosure":true})");
    CHECK(info.valid);
    CHECK(info.token == "5f1c0f9e-aaaa-bbbb-cccc-0123456789ab");
    CHECK(info.series == "A250");
    CHECK(info.head_type == 1);
    CHECK(info.has_enclosure);
    CHECK_FALSE(info.readonly);
    CHECK(is_printing_head(info.head_type));

    const auto wrapped = parse_connect(R"({"data":{"token":"t","headType":5}})");
    CHECK(wrapped.valid);
    CHECK(wrapped.head_type == 5);

    CHECK_FALSE(parse_connect("").valid);
    CHECK_FALSE(parse_connect("Failed to connect, there is someone trying to connect to Touchscreen.").valid);
    CHECK_FALSE(parse_connect(R"({"series":"A250"})").valid);
    CHECK_FALSE(parse_connect("[1,2]").valid);
}

TEST_CASE("toolhead types follow Luban's SM2 mapping", "[Snapmaker2]")
{
    CHECK(head_type_name(1) == "3D printing module (single extruder)");
    CHECK(head_type_name(5) == "dual extruder 3D printing module");
    CHECK(head_type_name(7) == "laser module (40 W)");
    CHECK(head_type_name(8) == "CNC module (200 W)");
    CHECK(head_type_name(42) == "unknown toolhead");
    CHECK(is_printing_head(5));
    CHECK_FALSE(is_printing_head(2));
    CHECK_FALSE(is_printing_head(-1));
}

TEST_CASE("status and start_print replies", "[Snapmaker2]")
{
    CHECK(machine_status(R"({"status":"IDLE","x":0})") == "IDLE");
    CHECK(machine_status(R"({"status":"running"})") == "RUNNING");
    CHECK(machine_status("") == "");
    CHECK(machine_status("not json") == "");
    CHECK(is_busy("RUNNING"));
    CHECK(is_busy("PAUSED"));
    CHECK_FALSE(is_busy("IDLE"));
    CHECK_FALSE(is_busy(""));
    CHECK(start_print_code(R"({"code":0})") == 0);
    CHECK(start_print_code(R"({"code":203})") == 203);
    CHECK(start_print_code("Prepare successfully.") == -1);
}

TEST_CASE("tokens never reach the log in full", "[Snapmaker2]")
{
    const std::string token = "5f1c0f9e-aaaa-bbbb-cccc-0123456789ab";
    CHECK(mask_token("") == "<none>");
    CHECK(mask_token("abc") == "****");
    CHECK(mask_token(token) == "****89ab");
    const std::string url = make_url("10.0.0.5", status_path(token));
    const std::string masked = mask_token_in(url, token);
    CHECK(masked == "http://10.0.0.5:8080/api/v1/status?token=****89ab");
    CHECK(masked.find(token) == std::string::npos);
    CHECK(mask_token_in("no token here", "") == "no token here");
    CHECK(truncate("short") == "short");
    CHECK(truncate(std::string(400, 'x'), 10) == std::string(10, 'x') + "...(400 bytes)");
}

TEST_CASE("bed levelling G-code follows the G1029 A contract", "[Snapmaker2][level]")
{
    // Contract: G1029 A [P<grid>] [H<bed C>] [K<soak s>] (features/bed-levelling-heat-soak.md).
    CHECK(level_gcode(clamp_level_params(5, 60, 300)) == "G1029 A P5 H60 K300");
    CHECK(level_gcode(clamp_level_params(11, 110, 3600)) == "G1029 A P11 H110 K3600");
    CHECK(level_gcode(clamp_level_params(3, 60, 0)) == "G1029 A P3 H60 K0");
    // No heating: H and K are left out, the soak is dropped like the firmware does.
    CHECK(level_gcode(clamp_level_params(7, 0, 600)) == "G1029 A P7");

    const LevelParams p = clamp_level_params(1, 150, 9000);
    CHECK(p.grid == LEVEL_GRID_MIN);
    CHECK(p.bed_temp_c == LEVEL_BED_MAX_C);
    CHECK(p.soak_s == LEVEL_SOAK_MAX_S);
    CHECK(clamp_level_params(42, -5, 100).grid == LEVEL_GRID_MAX);
    CHECK(clamp_level_params(42, -5, 100).bed_temp_c == 0);
    CHECK(clamp_level_params(42, -5, 100).soak_s == 0);
    CHECK(clamp_level_params(4, 60, -1).soak_s == 0);
    CHECK(LEVEL_SOAK_MAX_MIN == 60);
}

TEST_CASE("bed levelling timeout covers heat-up, soak and probing", "[Snapmaker2][level]")
{
    // 3x3 without heating: homing 60 s + 9 points x 15 s + 600 s margin.
    CHECK(level_timeout_s(clamp_level_params(3, 0, 0)) == 60 + 135 + 600);
    // 11x11, 110 C, 60 min soak: 30 min heat-up + 3600 s + 60 + 1815 + 600.
    CHECK(level_timeout_s(clamp_level_params(11, 110, 3600)) == 1800 + 3600 + 60 + 1815 + 600);
    // The soak always adds to the timeout.
    CHECK(level_timeout_s(clamp_level_params(5, 60, 600)) - level_timeout_s(clamp_level_params(5, 60, 0)) == 600);
}

TEST_CASE("bed levelling replies are classified from the controller output", "[Snapmaker2][level]")
{
    const std::string done = "echo:heat soak: bed 60C, soak 300s\necho:soak 300s left\necho:heat soak done\nok";
    CHECK(classify_level_reply(done, 900.) == LevelResult::Done);
    // Without H there is no heat soak line at all; a plain "ok" after a few minutes is success.
    CHECK(classify_level_reply("ok", 120.) == LevelResult::Done);
    CHECK(classify_level_reply("", 120.) == LevelResult::Done);
    // Stock firmware: P first, sets the grid and returns at once without probing.
    CHECK(classify_level_reply("Set grid size : 5\nok", 0.4) == LevelResult::TooFast);

    CHECK(classify_level_reply("cannot do auto leveling without 3DP\nok", 0.2) == LevelResult::NoSingleHead);
    CHECK(classify_level_reply("heat soak refused: bed fault\nauto leveling cancelled\n", 0.3) == LevelResult::BedFault);
    const std::string aborted = "echo:soak 120s left\r\necho:heat soak aborted: emergency stop\r\nauto leveling cancelled\r\nok";
    CHECK(classify_level_reply(aborted, 400.) == LevelResult::SoakAborted);
    CHECK(soak_abort_reason(aborted) == "emergency stop");
    CHECK(soak_abort_reason("echo:heat soak aborted: screen exit") == "screen exit");
    CHECK(soak_abort_reason(done).empty());
    CHECK(classify_level_reply("AUTO LEVELING CANCELLED", 30.) == LevelResult::Cancelled);
    CHECK(classify_level_reply("auto probing fail !\nok", 200.) == LevelResult::ProbeFailed);
    CHECK(classify_level_reply("Invalid grid size, range: 2 - 11", 0.1) == LevelResult::InvalidGrid);
    CHECK(std::string(level_result_name(LevelResult::TooFast)) == "too-fast");
    CHECK(std::string(level_result_name(LevelResult::Done)) == "done");
}

TEST_CASE("nozzle-zero jog and save lines", "[Snapmaker2][level]")
{
    CHECK(format_mm(0.05) == "0.05");
    CHECK(format_mm(-0.05) == "-0.05");
    CHECK(format_mm(0.1) == "0.1");
    CHECK(format_mm(0.5) == "0.5");
    CHECK(format_mm(-0.0001) == "0");
    CHECK(format_mm(1.0) == "1");
    CHECK(format_mm(12.345) == "12.345");
    CHECK(format_mm(-1.2) == "-1.2");
    CHECK(format_mm(0.0004) == "0");
    CHECK(format_mm(-2.0) == "-2");
    CHECK(format_mm(std::nan("")) == "0");

    const std::vector<std::string> down = jog_z_gcodes(-0.05);
    REQUIRE(down.size() == 3);
    CHECK(down[0] == "G91");
    CHECK(down[1] == "G1 Z-0.05 F300");
    CHECK(down[2] == "G90");
    const std::vector<std::string> up = jog_z_gcodes(0.5);
    REQUIRE(up.size() == 3);
    CHECK(up[1] == "G1 Z0.5 F300");
    // The touchscreen URL-decodes "code" twice: a '+' would become a space.
    for (const auto& line : jog_z_gcodes(0.1))
        CHECK(line.find('+') == std::string::npos);
    CHECK(jog_z_gcodes(0.).empty());
    CHECK(jog_z_gcodes(1.5).empty());
    CHECK(jog_z_gcodes(-2.).empty());
    CHECK(jog_z_gcodes(std::nan("")).empty());

    CHECK(save_z_zero_gcode() == "G1029 S0");
}

TEST_CASE("execute_code sends one line with the token", "[Snapmaker2][level]")
{
    const auto fields = execute_code_fields("tok", "G1029 A P5 H60 K300");
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].first == "token");
    CHECK(fields[0].second == "tok");
    CHECK(fields[1].first == "code");
    CHECK(fields[1].second == "G1029 A P5 H60 K300");
    CHECK(form_body(fields) == "token=tok&code=G1029%20A%20P5%20H60%20K300");
    // Only the first line: the touchscreen would silently drop the rest.
    CHECK(execute_code_fields("t", "G91\nG1 Z-0.05 F300\nG90")[1].second == "G91");
    CHECK(execute_code_fields("t", " G90\r\n")[1].second == "G90");
    CHECK(form_body(execute_code_fields("t", "G1 Z-0.05 F300")) == "token=t&code=G1%20Z-0.05%20F300");
}

TEST_CASE("status snapshot for the levelling progress", "[Snapmaker2][level]")
{
    const auto s = parse_status(
        R"({"status":"IDLE","x":117.5,"y":125,"z":12.25,"homed":true,"toolHead":"TOOLHEAD_3DPRINTING_1",)"
        R"("nozzleTemperature":24,"nozzleTargetTemperature":0,"heatedBedTemperature":58.4,"heatedBedTargetTemperature":60})");
    CHECK(s.valid);
    CHECK(s.status == "IDLE");
    CHECK(s.has_bed);
    CHECK(s.bed_c == 58.4);
    CHECK(s.bed_target_c == 60.);
    CHECK(s.has_z);
    CHECK(s.z == 12.25);

    // Laser / CNC heads report no bed values.
    const auto laser = parse_status(R"({"status":"RUNNING","z":3,"laserPower":0})");
    CHECK(laser.valid);
    CHECK(laser.status == "RUNNING");
    CHECK_FALSE(laser.has_bed);
    CHECK(laser.has_z);

    CHECK_FALSE(parse_status("").valid);
    CHECK_FALSE(parse_status("Machine is printing now, movement rejected.").valid);
}

// Output lines below are copied from Snapmaker2-Controller SM2-5.2.1-a250t.4 (G1029.cpp, abl.cpp, bed_level.cpp
// ReportZOffset); SERIAL_ECHO_MSG / SERIAL_ECHO_START prefix "echo:".
TEST_CASE("G1029 A replies with the stored Z offset (a250t.4)", "[Snapmaker2][zoffset]")
{
    const std::string applied = "echo:heat soak done\necho:Z offset applied, mesh saved\necho:Z offset 1.234 (valid)\nok";
    CHECK(classify_level_reply(applied, 600.) == LevelResult::Applied);
    const ZOffsetReport a = parse_zoffset_report(applied);
    CHECK(a.found);
    CHECK(a.valid);
    CHECK(a.mm == Approx(1.234));
    CHECK_FALSE(a.not_calibrated);
    CHECK(a.rejected.empty());

    const std::string not_cal = "echo:Z offset not calibrated\nok";
    CHECK(classify_level_reply(not_cal, 300.) == LevelResult::NotCalibrated);
    const ZOffsetReport n = parse_zoffset_report(not_cal);
    CHECK_FALSE(n.found);
    CHECK(n.not_calibrated);

    const std::string other_head = "echo:Z offset rejected: other toolhead\r\necho:Z offset not calibrated\r\nok";
    CHECK(classify_level_reply(other_head, 300.) == LevelResult::NotCalibrated);
    CHECK(parse_zoffset_report(other_head).rejected == "other toolhead");
    CHECK(parse_zoffset_report(other_head).not_calibrated);

    // Failures still win over the Z-offset lines, and a250t.3 (no Z-offset line) is still the card-step "Done".
    CHECK(classify_level_reply("auto probing fail !\nok", 200.) == LevelResult::ProbeFailed);
    CHECK(classify_level_reply("echo:heat soak done\nok", 600.) == LevelResult::Done);
    CHECK(std::string(level_result_name(LevelResult::Applied)) == "z-offset-applied");
    CHECK(std::string(level_result_name(LevelResult::NotCalibrated)) == "z-offset-not-calibrated");
}

TEST_CASE("Z offset report line parsing", "[Snapmaker2][zoffset]")
{
    const ZOffsetReport v = parse_zoffset_report("echo:Z offset 1.234 (valid)\nok\n");
    CHECK(v.found);
    CHECK(v.valid);
    CHECK(v.mm == Approx(1.234));

    const ZOffsetReport z = parse_zoffset_report("echo:Z offset 0.000 (not set)");
    CHECK(z.found);
    CHECK_FALSE(z.valid);
    CHECK(z.mm == 0.);

    // SERIAL_ECHO_F(x, 3) prints negatives with a leading '-'.
    const ZOffsetReport neg = parse_zoffset_report("echo:Z offset -0.100 (valid)\r\n");
    CHECK(neg.found);
    CHECK(neg.mm == Approx(-0.1));

    const ZOffsetReport out = parse_zoffset_report("echo:Z offset rejected: out of range\necho:Z offset 7.250 (not set)\n");
    CHECK(out.found);
    CHECK_FALSE(out.valid);
    CHECK(out.mm == Approx(7.25));
    CHECK(out.rejected == "out of range");

    // The other lines that start with "Z offset " are not reports.
    for (const char* decoy : { "echo:Z offset probed, lower the nozzle onto the card and send G1029 S0",
                               "echo:Z offset applied, mesh saved", "echo:Z offset not calibrated",
                               "echo:Z offset rejected: other toolhead", "Z offset calibration failed",
                               "Z offset calibration cancelled", "echo:Z offset out of range, not stored: 6.20",
                               "echo:Z offset not stored: nozzle is not over the centre point",
                               "echo:Z offset 1.234 (valid) extra", "echo:Z offset abc (valid)", "", "ok",
                               "cannot calibrate Z offset without 3DP" })
        CHECK_FALSE(parse_zoffset_report(decoy).found);

    // The last report wins (S0 prints the new state after a refusal line).
    const ZOffsetReport last = parse_zoffset_report("echo:Z offset 0.500 (valid)\necho:Z offset 0.000 (not set)\n");
    CHECK_FALSE(last.valid);
    CHECK(last.mm == 0.);

    CHECK(zoffset_query_gcode() == "G1029");
}

TEST_CASE("Z-offset calibration command and replies", "[Snapmaker2][zoffset]")
{
    CHECK(zoffset_calibration_gcode(clamp_level_params(5, 60, 300)) == "G1029 Z H60 K300");
    CHECK(zoffset_calibration_gcode(clamp_level_params(5, 0, 300)) == "G1029 Z");
    CHECK(zoffset_calibration_gcode(clamp_level_params(11, 500, 99999)) == "G1029 Z H110 K3600");
    CHECK(zoffset_calibration_gcode(clamp_level_params(3, 60, 0)).find('+') == std::string::npos);
    CHECK(zoffset_timeout_s(clamp_level_params(3, 0, 0)) == 60 + 15 + 600);
    CHECK(zoffset_timeout_s(clamp_level_params(3, 110, 3600)) == 1800 + 3600 + 60 + 15 + 600);
    // One probe point: shorter than any mesh measurement.
    CHECK(zoffset_timeout_s(clamp_level_params(3, 60, 300)) < level_timeout_s(clamp_level_params(3, 60, 300)));

    CHECK(classify_zoffset_calibration_reply("echo:Z offset probed, lower the nozzle onto the card and send G1029 S0\nok") ==
          ZOffsetCalResult::Probed);
    CHECK(classify_zoffset_calibration_reply("echo:heat soak done\necho:Z offset probed, lower the nozzle onto the card and send G1029 S0\n") ==
          ZOffsetCalResult::Probed);
    CHECK(classify_zoffset_calibration_reply("cannot calibrate Z offset without 3DP\nok") == ZOffsetCalResult::NoSingleHead);
    CHECK(classify_zoffset_calibration_reply("heat soak refused: bed fault\nZ offset calibration cancelled\n") ==
          ZOffsetCalResult::BedFault);
    const std::string aborted = "echo:soak 120s left\necho:heat soak aborted: screen exit\nZ offset calibration cancelled\nok";
    CHECK(classify_zoffset_calibration_reply(aborted) == ZOffsetCalResult::SoakAborted);
    CHECK(soak_abort_reason(aborted) == "screen exit");
    CHECK(classify_zoffset_calibration_reply("Z offset calibration cancelled\n") == ZOffsetCalResult::Cancelled);
    CHECK(classify_zoffset_calibration_reply("auto probing fail !\nZ offset calibration failed\nok") == ZOffsetCalResult::ProbeFailed);
    CHECK(classify_zoffset_calibration_reply("Z offset calibration failed") == ZOffsetCalResult::ProbeFailed);
    // a250t.3 / stock ignore "G1029 Z" and answer at once: never treat that as probed.
    CHECK(classify_zoffset_calibration_reply("") == ZOffsetCalResult::Unsupported);
    CHECK(classify_zoffset_calibration_reply("ok") == ZOffsetCalResult::Unsupported);
    CHECK(classify_zoffset_calibration_reply("echo:heat soak done\nok") == ZOffsetCalResult::Unsupported);
    CHECK(std::string(zoffset_cal_result_name(ZOffsetCalResult::Unsupported)) == "unsupported");
    CHECK(std::string(zoffset_cal_result_name(ZOffsetCalResult::Probed)) == "probed");
}

TEST_CASE("G1029 S0 replies", "[Snapmaker2][zoffset]")
{
    CHECK(classify_save_reply("echo:Z offset 1.180 (valid)\nok") == SaveResult::Stored);
    CHECK(classify_save_reply("echo:Z offset out of range, not stored: 6.20\necho:Z offset 0.000 (not set)\nok") ==
          SaveResult::OutOfRange);
    CHECK(classify_save_reply("echo:Z offset not stored: nozzle is not over the centre point\necho:Z offset 1.000 (valid)\n") ==
          SaveResult::NotAtCentre);
    CHECK(classify_save_reply("echo:G1029 S0 ignored: nothing probed\nok") == SaveResult::NothingProbed);
    CHECK(classify_save_reply("echo:Z offset 0.000 (not set)\n") == SaveResult::NotStored);
    // a250t.3 prints nothing for S0.
    CHECK(classify_save_reply("") == SaveResult::Legacy);
    CHECK(classify_save_reply("ok") == SaveResult::Legacy);
    CHECK(std::string(save_result_name(SaveResult::NotAtCentre)) == "not-at-centre");
    CHECK(std::string(save_result_name(SaveResult::Legacy)) == "legacy");
}

TEST_CASE("jog lines do not follow a comma-decimal LC_NUMERIC", "[Snapmaker2][level][locale]")
{
    // GUI_App initialises wxLocale for the UI language (setlocale LC_ALL); snprintf("%.3f") then wrote "0,05".
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = current != nullptr ? current : "C";
    const char* comma_locale = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "pt_BR.UTF-8", "German_Germany.1252"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            comma_locale = name;
            break;
        }
    }
    if (comma_locale == nullptr) {
        WARN("no comma-decimal locale installed, locale case skipped");
        return;
    }
    INFO("LC_NUMERIC=" << comma_locale);
    CHECK(format_mm(0.05) == "0.05");
    CHECK(format_mm(-1.25) == "-1.25");
    const std::vector<std::string> down = jog_z_gcodes(-0.5);
    std::setlocale(LC_NUMERIC, saved.c_str());
    REQUIRE(down.size() == 3);
    CHECK(down[1] == "G1 Z-0.5 F300");
}

TEST_CASE("card-step jog asks once before going below -2 mm", "[Snapmaker2][level][jog]")
{
    CHECK(JOG_CONFIRM_BELOW_MM == Approx(-2.0));
    CHECK_FALSE(jog_needs_confirmation(0., -0.5));
    CHECK_FALSE(jog_needs_confirmation(-1.5, -0.5));  // lands exactly on -2: no prompt yet
    CHECK(jog_needs_confirmation(-2.0, -0.05));       // first step below
    CHECK(jog_needs_confirmation(-1.9, -0.5));        // crosses in one click
    CHECK_FALSE(jog_needs_confirmation(-2.5, -0.1));  // already below, confirmed earlier
    CHECK_FALSE(jog_needs_confirmation(-2.5, 0.5));   // raising never asks
    CHECK_FALSE(jog_needs_confirmation(-1.9, -0.5, -3.0));
    CHECK_FALSE(jog_needs_confirmation(std::nan(""), -0.5));
    // 40 clicks of -0.05 accumulate floating-point error but must land on -2 without a prompt.
    double total = 0.;
    int    prompts = 0;
    for (int i = 0; i < 40; ++i) {
        prompts += jog_needs_confirmation(total, -0.05) ? 1 : 0;
        total += -0.05;
    }
    CHECK(prompts == 0);
    CHECK(jog_needs_confirmation(total, -0.05));
}

// ---------------------------------------------------------------------------------------------------------------
// Monitor panel (proposal O1). Status bodies below are built from the keys MainRequestHandler.getStatus writes
// (Snapmaker-SM2-Screen-App fabscreen-legacy, V1.19.3 source); no capture from a real touchscreen exists yet.

namespace {
const char* STATUS_SINGLE_PRINTING =
    R"({"status":"RUNNING","x":100.2,"y":80,"z":1.6,"homed":true,"offsetX":0,"offsetY":0,"offsetZ":0,)"
    R"("toolHead":"TOOLHEAD_3DPRINTING_1","nozzleTemperature":209.6,"nozzleTargetTemperature":210,)"
    R"("heatedBedTemperature":59.9,"heatedBedTargetTemperature":60,"isFilamentOut":false,"workSpeed":3600,)"
    R"("printStatus":"Printing","fileName":"benchy.gcode","totalLines":182733,"estimatedTime":5400,)"
    R"("currentLine":45000,"progress":0.2463,"elapsedTime":1322,"remainingTime":4100,)"
    R"("moduleList":{"enclosure":true,"rotaryModule":false,"emergencyStopButton":true,"airPurifier":false},)"
    R"("isEnclosureDoorOpen":false,"doorSwitchCount":0})";
const char* STATUS_DUAL_PAUSED =
    R"({"status":"PAUSED","x":0,"y":0,"z":20,"homed":true,"toolHead":"TOOLHEAD_3DPRINTING_1",)"
    R"("nozzleTemperature1":205,"nozzleTargetTemperature1":205,"nozzleTemperature2":180.5,"nozzleTargetTemperature2":0,)"
    R"("heatedBedTemperature":60,"heatedBedTargetTemperature":60,"isFilamentOut":true,"workSpeed":0,)"
    R"("printStatus":"Paused","fileName":"two colour.gcode","totalLines":1000,"estimatedTime":600,)"
    R"("currentLine":500,"progress":0.5,"elapsedTime":300,"remainingTime":225,)"
    R"("moduleList":{"enclosure":false,"rotaryModule":false,"emergencyStopButton":false,"airPurifier":true},)"
    R"("airPurifierSwitch":true,"airPurifierFanSpeed":2,"airPurifierFilterHealth":80})";
const char* STATUS_SINGLE_IDLE =
    R"({"status":"IDLE","x":0,"y":0,"z":0,"homed":false,"toolHead":"TOOLHEAD_3DPRINTING_1","nozzleTemperature":24,)"
    R"("nozzleTargetTemperature":0,"heatedBedTemperature":23,"heatedBedTargetTemperature":0,"isFilamentOut":false,)"
    R"("workSpeed":0,"printStatus":"Idle","moduleList":{"enclosure":false,"rotaryModule":false,"emergencyStopButton":false,"airPurifier":false}})";
const char* STATUS_COMPLETE =
    R"({"status":"IDLE","z":120,"toolHead":"TOOLHEAD_3DPRINTING_1","nozzleTemperature":80,"nozzleTargetTemperature":0,)"
    R"("heatedBedTemperature":50,"heatedBedTargetTemperature":0,"workSpeed":0,"printStatus":"Complete","moduleList":{}})";
const char* STATUS_LASER =
    R"({"status":"RUNNING","z":20,"toolHead":"TOOLHEAD_LASER_2","laserFocalLength":20,"laserPower":30,"laserCamera":false,)"
    R"("workSpeed":1500,"printStatus":"Printing","fileName":"cut.nc","progress":0.1,"elapsedTime":60,"remainingTime":540,)"
    R"("isEmergencyStopped":true,"moduleList":{"enclosure":true,"airPurifier":true}})";

ConnectInfo connect_info(int head_type, bool enclosure = false, bool readonly = false)
{
    ConnectInfo info;
    info.valid         = true;
    info.token         = "tok";
    info.head_type     = head_type;
    info.has_enclosure = enclosure;
    info.readonly      = readonly;
    return info;
}

std::string field(const Command& c, const std::string& key)
{
    for (const auto& f : c.fields)
        if (f.first == key)
            return f.second;
    return "<missing>";
}
} // namespace

TEST_CASE("monitor status: single extruder printing", "[Snapmaker2][monitor]")
{
    const auto s = parse_status(STATUS_SINGLE_PRINTING);
    REQUIRE(s.valid);
    CHECK(s.status == "RUNNING");
    CHECK(s.print_status == "Printing");
    CHECK(s.tool_head == "TOOLHEAD_3DPRINTING_1");
    CHECK(s.nozzles == 1);
    CHECK(s.nozzle_c[0] == Approx(209.6));
    CHECK(s.nozzle_target_c[0] == 210.);
    CHECK(s.bed_target_c == 60.);
    CHECK(s.has_feed_rate);
    CHECK(s.feed_rate == 3600.);
    REQUIRE(s.has_job);
    CHECK(s.file_name == "benchy.gcode");
    CHECK(s.progress == Approx(0.2463));
    CHECK(s.elapsed_s == 1322.);
    CHECK(s.remaining_s == 4100.);
    CHECK(s.estimated_s == 5400.);
    CHECK(s.current_line == 45000.);
    CHECK(s.total_lines == 182733.);
    CHECK_FALSE(s.filament_out);
    CHECK(s.enclosure_module);
    CHECK_FALSE(s.purifier_module);
    CHECK(s.has_door);
    CHECK_FALSE(s.door_open);
    CHECK_FALSE(s.has_purifier_state);
    CHECK_FALSE(s.emergency_stopped);
    CHECK(monitor_state(s) == MonitorState::Printing);
}

TEST_CASE("monitor status: dual extruder paused, purifier, filament out", "[Snapmaker2][monitor]")
{
    const auto s = parse_status(STATUS_DUAL_PAUSED);
    REQUIRE(s.valid);
    CHECK(s.nozzles == 2);
    CHECK(s.nozzle_c[0] == 205.);
    CHECK(s.nozzle_c[1] == Approx(180.5));
    CHECK(s.nozzle_target_c[1] == 0.);
    CHECK(s.filament_out);
    CHECK(s.purifier_module);
    CHECK(s.has_purifier_state);
    CHECK(s.purifier_on);
    CHECK(s.purifier_fan_speed == 2);
    CHECK_FALSE(s.has_door);
    CHECK(s.file_name == "two colour.gcode");
    CHECK(monitor_state(s) == MonitorState::Paused);
}

TEST_CASE("monitor status: idle, complete, laser and garbage", "[Snapmaker2][monitor]")
{
    const auto idle = parse_status(STATUS_SINGLE_IDLE);
    CHECK(idle.valid);
    CHECK_FALSE(idle.has_job); // no job block while the touchscreen is not printing
    CHECK(idle.file_name.empty());
    CHECK(monitor_state(idle) == MonitorState::Idle);

    const auto done = parse_status(STATUS_COMPLETE);
    CHECK(monitor_state(done) == MonitorState::Finished);
    CHECK(std::string(monitor_state_name(MonitorState::Finished)) == "finished");

    const auto laser = parse_status(STATUS_LASER);
    CHECK(laser.nozzles == 0);
    CHECK_FALSE(laser.has_bed);
    CHECK(laser.has_job);
    CHECK(laser.emergency_stopped);
    CHECK(monitor_state(laser) == MonitorState::Printing);

    // Progress outside 0..1 is clamped like the touchscreen does.
    CHECK(parse_status(R"({"status":"RUNNING","progress":1.7})").progress == 1.);
    CHECK(parse_status(R"({"status":"RUNNING","progress":-0.2})").progress == 0.);

    CHECK(monitor_state(parse_status("")) == MonitorState::Unknown);
    CHECK(monitor_state(parse_status("Machine is not connected yet.")) == MonitorState::Unknown);
    CHECK(monitor_state(parse_status(R"({"x":1})")) == MonitorState::Unknown);
}

TEST_CASE("monitor commands use the touchscreen's parameter names", "[Snapmaker2][monitor]")
{
    for (const Command& c : { pause_command("tok"), resume_command("tok"), stop_command("tok") }) {
        REQUIRE(c.fields.size() == 1);
        CHECK(c.fields[0] == std::make_pair(std::string("token"), std::string("tok")));
    }
    CHECK(pause_command("t").path == "api/v1/pause_print");
    CHECK(resume_command("t").path == "api/v1/resume_print");
    CHECK(stop_command("t").path == "api/v1/stop_print");

    const Command single = nozzle_temperature_command("tok", 0, 1, 215.4);
    CHECK(single.path == "api/v1/override_nozzle_temperature");
    CHECK(field(single, "token") == "tok");
    CHECK(field(single, "nozzleTemp") == "215");
    CHECK(form_body(single.fields) == "token=tok&nozzleTemp=215");

    const Command dual = nozzle_temperature_command("tok", 1, 2, 199.6);
    CHECK(dual.path == "api/v1/override_nozzle_temperature_by_index");
    CHECK(field(dual, "extruderTemperature") == "200");
    CHECK(field(dual, "extruderIndex") == "1");
    CHECK_FALSE(nozzle_temperature_command("tok", 1, 1, 200).valid()); // no second nozzle on the single head
    CHECK_FALSE(nozzle_temperature_command("tok", -1, 2, 200).valid());
    CHECK(nozzle_temperature_command("tok", 0, 0, 200).valid());       // unknown count -> single route
    CHECK(nozzle_temperature_command("tok", 0, 0, 200).path == "api/v1/override_nozzle_temperature");

    const Command bed = bed_temperature_command("tok", 65);
    CHECK(bed.path == "api/v1/override_bed_temperature");
    CHECK(field(bed, "heatedBedTemp") == "65");

    const Command z = z_offset_command("tok", -0.05);
    CHECK(z.path == "api/v1/override_z_offset");
    CHECK(field(z, "zOffset") == "-0.05");
    CHECK(form_body(z.fields) == "token=tok&zOffset=-0.05");
    CHECK(field(z_offset_command("tok", 0.1), "zOffset") == "0.1"); // never a '+'

    const Command speed = work_speed_command("tok", 120);
    CHECK(speed.path == "api/v1/override_work_speed");
    CHECK(field(speed, "workSpeed") == "120");

    CHECK(enclosure_light_command("tok", true).path == "api/v1/enclosure");
    CHECK(field(enclosure_light_command("tok", true), "led") == "100");
    CHECK(field(enclosure_light_command("tok", false), "led") == "0");
    CHECK(field(enclosure_fan_command("tok", true), "fan") == "100");
    CHECK(field(enclosure_fan_command("tok", true), "led") == "<missing>"); // one setting per request
    CHECK(purifier_switch_command("tok", true).path == "api/v1/air_purifier_switch");
    CHECK(field(purifier_switch_command("tok", true), "switch") == "true");
    CHECK(field(purifier_switch_command("tok", false), "switch") == "false");
    CHECK(purifier_fan_speed_command("tok", 2).path == "api/v1/air_purifier_fan_speed");
    CHECK(field(purifier_fan_speed_command("tok", 2), "fan_speed") == "2");
    CHECK_FALSE(single.what.empty());
}

TEST_CASE("monitor commands clamp to the controller's limits", "[Snapmaker2][monitor]")
{
    CHECK(field(nozzle_temperature_command("t", 0, 1, 400), "nozzleTemp") == "275");
    CHECK(field(nozzle_temperature_command("t", 0, 1, -20), "nozzleTemp") == "0");
    CHECK(field(nozzle_temperature_command("t", 0, 1, std::nan("")), "nozzleTemp") == "0");
    CHECK(field(bed_temperature_command("t", 150), "heatedBedTemp") == "110");
    CHECK(field(bed_temperature_command("t", -1), "heatedBedTemp") == "0");
    CHECK(field(z_offset_command("t", 5), "zOffset") == "2");
    CHECK(field(z_offset_command("t", -3), "zOffset") == "-0.5");
    CHECK(field(z_offset_command("t", std::nan("")), "zOffset") == "0");
    CHECK(field(work_speed_command("t", 0), "workSpeed") == "10");
    CHECK(field(work_speed_command("t", 900), "workSpeed") == "500");
    CHECK(field(purifier_fan_speed_command("t", 0), "fan_speed") == "1");
    CHECK(field(purifier_fan_speed_command("t", 9), "fan_speed") == "3");
}

TEST_CASE("monitor Z offset is absolute and accumulates per click", "[Snapmaker2][monitor]")
{
    // The touchscreen and the controller treat zOffset as the new absolute live offset (an unchanged value is ignored),
    // so two "+0.05" clicks must send 0.05 then 0.1, and "+" then "-" must return to 0.
    double z = 0.;
    z = next_z_offset(z, 0.05);
    CHECK(field(z_offset_command("t", z), "zOffset") == "0.05");
    z = next_z_offset(z, 0.05);
    CHECK(field(z_offset_command("t", z), "zOffset") == "0.1");
    z = next_z_offset(z, -0.05);
    z = next_z_offset(z, -0.05);
    CHECK(z == 0.);
    CHECK(field(z_offset_command("t", z), "zOffset") == "0");
    CHECK_FALSE(std::signbit(next_z_offset(0.05, -0.05)));
    // Twenty clicks of 0.05 do not drift (rounded to 0.01 mm each time).
    z = 0.;
    for (int i = 0; i < 20; ++i)
        z = next_z_offset(z, 0.05);
    CHECK(field(z_offset_command("t", z), "zOffset") == "1");
    // Clamped at the controller's range: a click past the limit leaves the value unchanged.
    CHECK(next_z_offset(1.98, 0.05) == 2.);
    CHECK(next_z_offset(2., 0.05) == 2.);
    CHECK(next_z_offset(-0.5, -0.05) == -0.5);
    CHECK(next_z_offset(std::nan(""), 0.05) == Approx(0.05));
}

TEST_CASE("monitor controls follow the job state and the session", "[Snapmaker2][monitor]")
{
    const auto printing = parse_status(STATUS_SINGLE_PRINTING);
    auto       c        = monitor_controls(printing, connect_info(1, true), false);
    CHECK(c.pause);
    CHECK_FALSE(c.resume);
    CHECK(c.stop);
    CHECK(c.nozzle);
    CHECK(c.bed);
    CHECK(c.z_offset);
    CHECK(c.work_speed);
    CHECK(c.enclosure);
    CHECK_FALSE(c.purifier);
    CHECK(c.nozzle_count == 1);

    const auto paused = parse_status(STATUS_DUAL_PAUSED);
    c = monitor_controls(paused, connect_info(5), false);
    CHECK_FALSE(c.pause);
    CHECK(c.resume);
    CHECK(c.stop);
    CHECK(c.nozzle);
    CHECK(c.nozzle_count == 2);
    CHECK_FALSE(c.enclosure);
    CHECK(c.purifier);

    // Idle: no job controls and no temperature overrides (idle they would send M104 / M140 straight away).
    c = monitor_controls(parse_status(STATUS_SINGLE_IDLE), connect_info(1), false);
    CHECK_FALSE(c.pause);
    CHECK_FALSE(c.resume);
    CHECK_FALSE(c.stop);
    CHECK_FALSE(c.nozzle);
    CHECK_FALSE(c.bed);
    CHECK_FALSE(c.z_offset);
    CHECK_FALSE(c.work_speed);
    CHECK_FALSE(monitor_controls(parse_status(STATUS_COMPLETE), connect_info(1), false).stop);

    // Laser job: pause/stop/speed yes, no FDM overrides; enclosure from the module list.
    c = monitor_controls(parse_status(STATUS_LASER), connect_info(4), false);
    CHECK(c.pause);
    CHECK(c.stop);
    CHECK(c.work_speed);
    CHECK_FALSE(c.nozzle);
    CHECK_FALSE(c.z_offset);
    CHECK(c.enclosure);
    CHECK(c.purifier);

    // Nothing while a command runs, for a read-only session, or without a status.
    for (const MonitorControls& off : { monitor_controls(printing, connect_info(1, true), true),
                                        monitor_controls(printing, connect_info(1, true, true), false),
                                        monitor_controls(parse_status(""), connect_info(1, true), false) }) {
        CHECK_FALSE(off.pause);
        CHECK_FALSE(off.stop);
        CHECK_FALSE(off.nozzle);
        CHECK_FALSE(off.work_speed);
        CHECK_FALSE(off.enclosure);
        CHECK_FALSE(off.purifier);
    }
}

TEST_CASE("monitor notices a new job", "[Snapmaker2][monitor]")
{
    const auto idle     = parse_status(STATUS_SINGLE_IDLE);
    const auto printing = parse_status(STATUS_SINGLE_PRINTING);
    CHECK(job_changed(idle, printing));
    CHECK_FALSE(job_changed(printing, printing));
    CHECK_FALSE(job_changed(printing, idle));
    auto other = printing;
    other.file_name = "other.gcode";
    CHECK(job_changed(printing, other));
    auto again = printing;
    again.elapsed_s = 3; // same file started again: the tick counter restarted
    CHECK(job_changed(printing, again));
    auto jitter = printing;
    jitter.elapsed_s = printing.elapsed_s - 2;
    CHECK_FALSE(job_changed(printing, jitter));
}

TEST_CASE("monitor session link and idle disconnect", "[Snapmaker2][monitor]")
{
    CHECK(link_state(200, 0) == LinkState::Ok);
    CHECK(link_state(200, 1500) == LinkState::Ok);
    CHECK(link_state(0, 0) == LinkState::Waiting);     // before the first poll
    CHECK(link_state(0, 3000) == LinkState::Waiting);  // one timed-out poll
    CHECK(link_state(500, 4000) == LinkState::Waiting);
    CHECK(link_state(0, MONITOR_LINK_LOST_MS) == LinkState::Lost);
    CHECK(link_state(200, MONITOR_LINK_LOST_MS) == LinkState::Lost); // stale 200
    // 401: the touchscreen ended the session (expired, an upload's disconnect, another client). Never "Waiting":
    // the panel must not keep polling and must not reconnect by itself.
    CHECK(link_state(401, 0) == LinkState::Lost);
    CHECK(std::string(link_state_name(LinkState::Lost)) == "lost");

    CHECK_FALSE(idle_disconnect_due(599, 10));
    CHECK(idle_disconnect_due(600, 10));
    CHECK_FALSE(idle_disconnect_due(100000, 0));
    CHECK_FALSE(idle_disconnect_due(100000, -1));
    CHECK(MONITOR_IDLE_DISCONNECT_MIN_DEFAULT > 0);
}

TEST_CASE("monitor time and progress text", "[Snapmaker2][monitor]")
{
    CHECK(format_duration(0) == "0 s");
    CHECK(format_duration(45) == "45 s");
    CHECK(format_duration(750) == "12 min 30 s");
    CHECK(format_duration(3900) == "1 h 05 min");
    CHECK(format_duration(36000 + 59) == "10 h 00 min");
    CHECK(format_duration(-1) == "-");
    CHECK(format_duration(std::nan("")) == "-");
    CHECK(progress_percent(0.2463) == 25);
    CHECK(progress_percent(0.) == 0);
    CHECK(progress_percent(1.) == 100);
    CHECK(progress_percent(1.5) == 100);
    CHECK(progress_percent(std::nan("")) == 0);
}
