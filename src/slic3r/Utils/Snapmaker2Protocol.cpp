#include "Snapmaker2Protocol.hpp"

#include <algorithm>
#include <cctype>
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
