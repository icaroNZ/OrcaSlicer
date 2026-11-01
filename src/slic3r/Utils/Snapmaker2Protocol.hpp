#ifndef slic3r_Snapmaker2Protocol_hpp_
#define slic3r_Snapmaker2Protocol_hpp_

// Pure helpers for the Snapmaker 2.0 (A150/A250/A350, incl. A250T) touchscreen HTTP API on port 8080.
// No wxWidgets / curl / boost dependency, so the request building and response classification can be
// unit tested without the GUI dependencies (tests/slic3rutils/Snapmaker2Protocol_test.cpp).
//
// Protocol summary (Luban src/server/services/machine/channels/SstpHttpChannel.ts and the touchscreen
// app's AuthRequestHandler / BaseRequestHandler / MainRequestHandler):
//   POST api/v1/connect        body "token=<t>" ("token=") -> 200 JSON {token, readonly, series, headType, hasEnclosure}
//                                                          -> 403 if another client is waiting for approval
//   GET  api/v1/status?token=  -> 200 JSON once the touchscreen user tapped "Yes",
//                                 204 while the request is still waiting for approval,
//                                 401 if the token is not the current session (expired / never connected)
//   POST api/v1/prepare_print  multipart token, type=3DP, file -> 200 "Prepare successfully.", 409 wrong head type
//   POST api/v1/start_print    body token=<t>                  -> 200 JSON {code:0}, 409 {code:n} if it cannot start
//   POST api/v1/disconnect     body token=<t>
// The touchscreen drops the current session after ~5 s without any request, so a client has to keep
// polling while it waits for approval and while a long upload is in flight.

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace Snapmaker2Protocol {

constexpr int         DEFAULT_PORT     = 8080;
constexpr const char* FILE_TYPE_3DP    = "3DP";
// Interval between keep-alive / approval polls. Must stay well below the touchscreen's 5 s session timeout.
constexpr int         POLL_INTERVAL_MS = 1500;

// Build "http://<host>:8080/<path>". `host` may be "192.168.1.20", "192.168.1.20:8080", "http://192.168.1.20/"
// or "https://name:1234"; surrounding white space and a trailing slash are ignored. An explicit port wins.
std::string make_url(const std::string& host, const std::string& path);

// application/x-www-form-urlencoded body from key/value pairs (RFC 3986 unreserved characters kept).
std::string url_encode(const std::string& value);
std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields);

// Body for api/v1/connect: always "token=<t>", "token=" when there is no token yet.
// Luban sends an empty body instead, but Orca's Http only sets CURLOPT_POSTFIELDS for a non-empty body, so an
// empty POST would make libcurl fall back to its read callback. The touchscreen treats an empty token exactly
// like a missing one (AuthRequestHandler: unknown token -> createSession()).
std::string connect_body(const std::string& token);

// "api/v1/status?token=<t>" (path for make_url()).
std::string status_path(const std::string& token);

enum class Reply {
    Ok,              // 2xx with content
    PendingApproval, // 204: waiting for the "Yes" on the touchscreen
    NotAuthorised,   // 401: token is not the current session (expired, or another client took over)
    BadRequest,      // 400: no token sent
    Busy,            // 403: another client is waiting for approval / connected
    Conflict,        // 409: wrong file type for the mounted toolhead, or start_print refused
    HttpError,       // any other HTTP status
    NetworkError,    // no HTTP status at all (timeout, refused, DNS)
};
Reply       classify(unsigned http_status);
const char* reply_name(Reply reply);

struct ConnectInfo
{
    bool        valid { false }; // body was JSON and carried a token
    std::string token;
    std::string series;          // e.g. "A250" (as reported by the touchscreen)
    int         head_type { -1 };
    bool        has_enclosure { false };
    bool        readonly { false };
};
ConnectInfo parse_connect(const std::string& body);

// Toolhead name for the touchscreen headType (mapping from Luban SstpHttpChannel.connectionOpen).
std::string head_type_name(int head_type);
// True for headType 1 (single extruder) and 5 (dual extruder).
bool        is_printing_head(int head_type);

// "IDLE" / "RUNNING" / "PAUSED" from an api/v1/status body, empty if missing or not JSON.
std::string machine_status(const std::string& status_body);
// RUNNING or PAUSED: uploading would replace the job the touchscreen is working on.
bool        is_busy(const std::string& machine_status);

// "start_print" answers {code:n}; returns n, or -1 when there is no code.
int         start_print_code(const std::string& body);

// Logging helpers: never log a full token, and keep error bodies short.
std::string mask_token(const std::string& token);
std::string mask_token_in(const std::string& text, const std::string& token);
std::string truncate(const std::string& text, size_t max_len = 300);

} // namespace Snapmaker2Protocol
} // namespace Slic3r

#endif // slic3r_Snapmaker2Protocol_hpp_
