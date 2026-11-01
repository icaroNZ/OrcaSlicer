// Unit tests for the Snapmaker 2.0 touchscreen HTTP protocol helpers used by the "snapmaker2" print host.
// The helpers have no wx / curl / boost dependency, so this suite builds with only Catch2 and nlohmann/json.

#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/Snapmaker2Protocol.hpp"

using namespace Slic3r::Snapmaker2Protocol;

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
