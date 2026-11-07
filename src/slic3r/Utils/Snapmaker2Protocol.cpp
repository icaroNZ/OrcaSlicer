#include "Snapmaker2Protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace Snapmaker2Protocol {

namespace {

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const char* prefix)
{
    const std::string p(prefix);
    return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

nlohmann::json parse_object(const std::string& body)
{
    if (body.empty())
        return nlohmann::json();
    nlohmann::json j = nlohmann::json::parse(body, nullptr, /*allow_exceptions*/ false);
    if (j.is_discarded() || !j.is_object())
        return nlohmann::json();
    return j;
}

bool contains_nocase(const std::string& haystack, const char* needle)
{
    const std::string n(needle);
    return std::search(haystack.begin(), haystack.end(), n.begin(), n.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != haystack.end();
}

bool number_field(const nlohmann::json& j, const char* key, double& out)
{
    if (!j.contains(key) || !j[key].is_number())
        return false;
    out = j[key].get<double>();
    return true;
}

} // namespace

std::string make_url(const std::string& host_in, const std::string& path_in)
{
    std::string host = trim(host_in);
    while (!host.empty() && host.back() == '/')
        host.pop_back();

    std::string path = path_in;
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());

    std::string scheme = "http://";
    if (starts_with(host, "http://")) {
        host = host.substr(7);
    } else if (starts_with(host, "https://")) {
        scheme = "https://";
        host   = host.substr(8);
    }

    // Only the authority part decides whether a port was given ("host/sub" is not expected, but be safe).
    const size_t slash     = host.find('/');
    const std::string auth = slash == std::string::npos ? host : host.substr(0, slash);
    const std::string rest = slash == std::string::npos ? std::string() : host.substr(slash);
    const bool        ipv6 = !auth.empty() && auth.front() == '[';
    const size_t      colon = ipv6 ? auth.find("]:") : auth.find(':');
    const std::string authority = colon == std::string::npos ? auth + ":" + std::to_string(DEFAULT_PORT) : auth;

    return scheme + authority + rest + "/" + path;
}

std::string url_encode(const std::string& value)
{
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string body;
    for (const auto& field : fields) {
        if (!body.empty())
            body += '&';
        body += url_encode(field.first) + "=" + url_encode(field.second);
    }
    return body;
}

std::string connect_body(const std::string& token)
{
    return form_body({ { "token", token } });
}

std::string status_path(const std::string& token) { return "api/v1/status?token=" + url_encode(token); }

Reply classify(unsigned http_status)
{
    if (http_status == 0)
        return Reply::NetworkError;
    if (http_status == 204)
        return Reply::PendingApproval;
    if (http_status >= 200 && http_status < 300)
        return Reply::Ok;
    switch (http_status) {
    case 400: return Reply::BadRequest;
    case 401: return Reply::NotAuthorised;
    case 403: return Reply::Busy;
    case 409: return Reply::Conflict;
    default: return Reply::HttpError;
    }
}

const char* reply_name(Reply reply)
{
    switch (reply) {
    case Reply::Ok: return "ok";
    case Reply::PendingApproval: return "pending-approval";
    case Reply::NotAuthorised: return "not-authorised";
    case Reply::BadRequest: return "bad-request";
    case Reply::Busy: return "busy";
    case Reply::Conflict: return "conflict";
    case Reply::HttpError: return "http-error";
    case Reply::NetworkError: return "network-error";
    }
    return "unknown";
}

ConnectInfo parse_connect(const std::string& body)
{
    ConnectInfo    info;
    nlohmann::json j = parse_object(body);
    if (j.is_null())
        return info;
    // Luban reads the fields at top level; tolerate a {data:{...}} wrapper as well.
    if (!j.contains("token") && j.contains("data") && j["data"].is_object())
        j = j["data"];

    if (j.contains("token") && j["token"].is_string())
        info.token = j["token"].get<std::string>();
    if (j.contains("series") && j["series"].is_string())
        info.series = j["series"].get<std::string>();
    if (j.contains("headType") && j["headType"].is_number_integer())
        info.head_type = j["headType"].get<int>();
    if (j.contains("hasEnclosure") && j["hasEnclosure"].is_boolean())
        info.has_enclosure = j["hasEnclosure"].get<bool>();
    if (j.contains("readonly") && j["readonly"].is_boolean())
        info.readonly = j["readonly"].get<bool>();
    info.valid = !info.token.empty();
    return info;
}

std::string head_type_name(int head_type)
{
    switch (head_type) {
    case 1: return "3D printing module (single extruder)";
    case 2: return "CNC module (50 W)";
    case 3: return "laser module (1.6 W)";
    case 4: return "laser module (10 W)";
    case 5: return "dual extruder 3D printing module";
    case 6: return "laser module (20 W)";
    case 7: return "laser module (40 W)";
    case 8: return "CNC module (200 W)";
    case 9: return "laser module (2 W)";
    default: return "unknown toolhead";
    }
}

bool is_printing_head(int head_type) { return head_type == 1 || head_type == 5; }

std::string machine_status(const std::string& status_body)
{
    nlohmann::json j = parse_object(status_body);
    if (j.is_null() || !j.contains("status") || !j["status"].is_string())
        return {};
    std::string s = j["status"].get<std::string>();
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool is_busy(const std::string& status) { return status == "RUNNING" || status == "PAUSED"; }

int start_print_code(const std::string& body)
{
    nlohmann::json j = parse_object(body);
    if (j.is_null() || !j.contains("code") || !j["code"].is_number_integer())
        return -1;
    return j["code"].get<int>();
}

LevelParams clamp_level_params(int grid, int bed_temp_c, int soak_s)
{
    LevelParams p;
    p.grid       = std::min(std::max(grid, LEVEL_GRID_MIN), LEVEL_GRID_MAX);
    p.bed_temp_c = std::min(std::max(bed_temp_c, 0), LEVEL_BED_MAX_C);
    p.soak_s     = p.bed_temp_c == 0 ? 0 : std::min(std::max(soak_s, 0), LEVEL_SOAK_MAX_S);
    return p;
}

std::string level_gcode(const LevelParams& params)
{
    std::string line = "G1029 A P" + std::to_string(params.grid);
    if (params.bed_temp_c > 0)
        line += " H" + std::to_string(params.bed_temp_c) + " K" + std::to_string(params.soak_s);
    return line;
}

long level_timeout_s(const LevelParams& params)
{
    // Heating an A250 bed to 110 C takes ~10 min (longer in a cold enclosure): allow 30 min. Homing ~1 min,
    // ~15 s per probe point, plus 10 min of margin. 11x11 at 110 C with a 60 min soak: 1800+3600+60+1815+600 s.
    const long heat  = params.bed_temp_c > 0 ? 30L * 60 + params.soak_s : 0;
    const long probe = 60L + 15L * params.grid * params.grid;
    return heat + probe + 10L * 60;
}

LevelResult classify_level_reply(const std::string& body, double elapsed_s)
{
    if (contains_nocase(body, "cannot do auto leveling without 3DP"))
        return LevelResult::NoSingleHead;
    if (contains_nocase(body, "heat soak refused"))
        return LevelResult::BedFault;
    if (contains_nocase(body, "heat soak aborted"))
        return LevelResult::SoakAborted;
    if (contains_nocase(body, "auto leveling cancelled"))
        return LevelResult::Cancelled;
    if (contains_nocase(body, "auto probing fail"))
        return LevelResult::ProbeFailed;
    if (contains_nocase(body, "Invalid grid size"))
        return LevelResult::InvalidGrid;
    if (elapsed_s < LEVEL_MIN_PLAUSIBLE_S)
        return LevelResult::TooFast;
    return LevelResult::Done;
}

const char* level_result_name(LevelResult result)
{
    switch (result) {
    case LevelResult::Done: return "done";
    case LevelResult::NoSingleHead: return "no-single-3dp-head";
    case LevelResult::BedFault: return "bed-fault";
    case LevelResult::SoakAborted: return "soak-aborted";
    case LevelResult::Cancelled: return "cancelled";
    case LevelResult::ProbeFailed: return "probe-failed";
    case LevelResult::InvalidGrid: return "invalid-grid";
    case LevelResult::TooFast: return "too-fast";
    }
    return "unknown";
}

std::string soak_abort_reason(const std::string& body)
{
    const std::string marker = "heat soak aborted: ";
    const size_t      pos    = body.find(marker);
    if (pos == std::string::npos)
        return {};
    const size_t start = pos + marker.size();
    const size_t end   = body.find_first_of("\r\n", start);
    return trim(body.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

std::string format_mm(double mm)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", mm);
    std::string s(buf);
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    if (s == "-0")
        s = "0";
    return s;
}

std::vector<std::string> jog_z_gcodes(double step_mm)
{
    if (!std::isfinite(step_mm) || std::fabs(step_mm) > JOG_MAX_STEP_MM || format_mm(step_mm) == "0")
        return {};
    return { "G91", "G1 Z" + format_mm(step_mm) + " F" + std::to_string(JOG_FEEDRATE), "G90" };
}

std::string save_z_zero_gcode() { return "G1029 S0"; }

std::vector<std::pair<std::string, std::string>> execute_code_fields(const std::string& token, const std::string& code)
{
    const size_t eol = code.find_first_of("\r\n");
    return { { "token", token }, { "code", trim(eol == std::string::npos ? code : code.substr(0, eol)) } };
}

StatusSnapshot parse_status(const std::string& status_body)
{
    StatusSnapshot snap;
    nlohmann::json j = parse_object(status_body);
    if (j.is_null())
        return snap;
    snap.valid   = true;
    snap.status  = machine_status(status_body);
    snap.has_bed = number_field(j, "heatedBedTemperature", snap.bed_c);
    if (snap.has_bed)
        number_field(j, "heatedBedTargetTemperature", snap.bed_target_c);
    snap.has_z = number_field(j, "z", snap.z);
    return snap;
}

std::string mask_token(const std::string& token)
{
    if (token.empty())
        return "<none>";
    if (token.size() <= 4)
        return "****";
    return "****" + token.substr(token.size() - 4);
}

std::string mask_token_in(const std::string& text, const std::string& token)
{
    if (token.empty())
        return text;
    std::string out    = text;
    const std::string masked = mask_token(token);
    for (const std::string& needle : { token, url_encode(token) }) {
        size_t pos = 0;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            out.replace(pos, needle.size(), masked);
            pos += masked.size();
        }
    }
    return out;
}

std::string truncate(const std::string& text, size_t max_len)
{
    if (text.size() <= max_len)
        return text;
    return text.substr(0, max_len) + "...(" + std::to_string(text.size()) + " bytes)";
}

} // namespace Snapmaker2Protocol
} // namespace Slic3r
