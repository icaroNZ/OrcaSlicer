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
    GatewayTimeout,  // 504: the touchscreen gave up waiting for the controller (pause/resume/stop, 30 s)
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

// ---------------------------------------------------------------------------------------------------------------
// Bed levelling through api/v1/execute_code (feature spec _snapmaker2-review/features/bed-levelling-heat-soak.md).
//   POST api/v1/execute_code  body token=<t>&code=<one G-code line> -> 200, body = the controller's output for that
//                             line, sent only after the line finished (the touchscreen waits for the G-code ACK
//                             with no timeout); 403 "Machine is printing now" when a job is running or paused.
// The touchscreen runs only the first line of `code`, and URL-decodes the already decoded value a second time
// (URLDecoder.decode(code, "GBK")), so a literal '+' would turn into a space: never put '+' in a line.
// Controller firmware SM2-5.2.1-a250t.3 or newer:
//   G1029 A P<grid 2..11> H<bed C 0..110> K<soak s 0..3600>  heat the bed, soak, home, probe (single 3DP head only)
//   G91 / G1 Z<step> F300 / G90                              nozzle-zero jog, one request per line
//   G1029 S0                                                 nozzle rests on the card: apply and save
// The system status stays IDLE during G1029 A, so completion is the execute_code reply, not a status change;
// api/v1/status is polled only as keep-alive and for the bed temperature / Z progress.
// Controller firmware SM2-5.2.1-a250t.4 or newer (spec _snapmaker2-review/features/separate-mesh-and-z-offset.md,
// Contract) keeps the probe-to-nozzle Z offset as a separate calibration:
//   G1029                    report "echo:Z offset <mm> (valid|not set)" (older firmware prints nothing)
//   G1029 Z [H<C>] [K<s>]    Z-offset calibration: heat/soak as A, home, probe the centre only, park the nozzle over it,
//                            "echo:Z offset probed, ..." -> card step + G1029 S0 stores the offset (older firmware
//                            ignores the line silently: never follow it with the card step there)
//   G1029 A ...              with a valid offset: "echo:Z offset applied, mesh saved" = finished, no card step;
//                            otherwise "echo:Z offset not calibrated" and the card step + G1029 S0, which then also
//                            stores the offset.

constexpr int LEVEL_GRID_MIN      = 3;   // the dialog offers 3..11 (firmware accepts 2..11)
constexpr int LEVEL_GRID_MAX      = 11;  // GRID_MAX_NUM
constexpr int LEVEL_GRID_DEFAULT  = 3;
constexpr int LEVEL_BED_MAX_C     = 110; // contract limit (A250 bed), firmware clamps to the same value
constexpr int LEVEL_SOAK_MAX_S    = 3600;
constexpr int LEVEL_SOAK_MAX_MIN  = LEVEL_SOAK_MAX_S / 60;
// A "successful" G1029 A reply faster than this cannot have homed and probed: stock firmware handles P first,
// only sets the grid size (resetting the mesh in RAM) and returns.
constexpr int LEVEL_MIN_PLAUSIBLE_S = 15;
constexpr double JOG_MAX_STEP_MM  = 1.0;
constexpr int JOG_FEEDRATE        = 300; // mm/min, as the touchscreen's own levelling screens
// Card step: ask before the nozzle goes more than 2 mm below where probing left it (a mis-click or stacked clicks
// would otherwise drive it into the bed). Opt out with app config [snapmaker2_bed_level] confirm_deep_jog = 0.
constexpr double JOG_CONFIRM_BELOW_MM = -2.0;

struct LevelParams
{
    int grid { LEVEL_GRID_DEFAULT };
    int bed_temp_c { 0 }; // 0 = do not heat (the soak is then ignored)
    int soak_s { 0 };
};
// Clamp to the ranges above; a soak without a bed temperature is dropped (the firmware ignores it too).
LevelParams clamp_level_params(int grid, int bed_temp_c, int soak_s);

// "G1029 A P5 H60 K300"; without heating "G1029 A P5" (H/K omitted). Expects clamped parameters.
std::string level_gcode(const LevelParams& params);

// HTTP timeout for the blocking execute_code call: heat-up allowance + soak + homing + probing + margin.
long level_timeout_s(const LevelParams& params);

enum class LevelResult {
    Done,          // probing finished; the nozzle waits over the bed centre for the nozzle-zero step
                   // (firmware without the Z-offset lines, i.e. SM2-5.2.1-a250t.3)
    Applied,       // "Z offset applied, mesh saved": a250t.4 applied the stored offset, levelling finished
    NotCalibrated, // "Z offset not calibrated": a250t.4, card step next (its G1029 S0 also stores the offset)
    NoSingleHead,  // "cannot do auto leveling without 3DP" (dual extruder, laser, CNC, nothing mounted)
    BedFault,      // "heat soak refused: bed fault"
    SoakAborted,   // "heat soak aborted: <reason>" (touchscreen exit, emergency stop, bed fault, target changed)
    Cancelled,     // "auto leveling cancelled" without a reason line
    ProbeFailed,   // "auto probing fail !"
    InvalidGrid,   // "Invalid grid size"
    TooFast,       // looked fine but answered in < LEVEL_MIN_PLAUSIBLE_S: firmware older than SM2-5.2.1-a250t.3
};
LevelResult classify_level_reply(const std::string& body, double elapsed_s);
const char* level_result_name(LevelResult result);
// Text after "heat soak aborted: " up to the end of that line, empty if there is none.
std::string soak_abort_reason(const std::string& body);

// Millimetres without '+' and without trailing zeros: 0.05 -> "0.05", -0.1 -> "-0.1", 0.5 -> "0.5".
std::string format_mm(double mm);
// {"G91", "G1 Z-0.05 F300", "G90"} for one jog click; empty when |step| is 0, above JOG_MAX_STEP_MM or not finite.
std::vector<std::string> jog_z_gcodes(double step_mm);
// True when this jog takes the cumulative card-step travel `total_mm` (sum of jogs since probing) from at or above
// `threshold_mm` to below it, i.e. the first click that crosses the line asks for confirmation; later clicks below do not.
bool jog_needs_confirmation(double total_mm, double step_mm, double threshold_mm = JOG_CONFIRM_BELOW_MM);
// "G1029 S0": the nozzle rests on the calibration card, apply the offset and save the mesh.
std::string save_z_zero_gcode();

// --- Z offset (SM2-5.2.1-a250t.4) ---
// The "echo:Z offset <mm> (valid|not set)" report and the lines around it, from any G1029 / M420 V output.
struct ZOffsetReport
{
    bool        found { false };      // a report line was seen (firmware a250t.4 or newer)
    bool        valid { false };      // "(valid)"; "(not set)" otherwise
    double      mm { 0. };            // the stored value, also printed when it is not set/usable
    std::string rejected;             // "out of range" / "other toolhead" from "Z offset rejected: <reason>", else empty
    bool        not_calibrated { false }; // "Z offset not calibrated" (G1029 A without a usable offset)
};
// Uses the last report line in `body` (G1029 S0 prints a refusal first, then the report).
ZOffsetReport parse_zoffset_report(const std::string& body);

// "G1029": report the stored offset. An answer without a report line means firmware older than a250t.4.
std::string zoffset_query_gcode();
// "G1029 Z H60 K300"; without heating "G1029 Z" (H/K omitted). Expects clamped parameters (the grid is not used).
std::string zoffset_calibration_gcode(const LevelParams& params);
// HTTP timeout for G1029 Z: heat-up allowance + soak + homing + one probe point + margin.
long zoffset_timeout_s(const LevelParams& params);

enum class ZOffsetCalResult {
    Probed,       // "Z offset probed, lower the nozzle onto the card and send G1029 S0": card step next
    NoSingleHead, // "cannot calibrate Z offset without 3DP"
    BedFault,     // "heat soak refused: bed fault"
    SoakAborted,  // "heat soak aborted: <reason>"
    Cancelled,    // "Z offset calibration cancelled" without a reason line
    ProbeFailed,  // "auto probing fail !" / "Z offset calibration failed"
    Unsupported,  // none of these: firmware older than a250t.4 ignored "G1029 Z" (do NOT send G1029 S0 then)
};
// Success needs the positive "Z offset probed" line, never just the absence of an error.
ZOffsetCalResult classify_zoffset_calibration_reply(const std::string& body);
const char*      zoffset_cal_result_name(ZOffsetCalResult result);

enum class SaveResult {
    Stored,        // report "(valid)": the offset was stored (and the mesh saved after G1029 A)
    OutOfRange,    // "Z offset out of range, not stored: <v>"
    NotAtCentre,   // "Z offset not stored: nozzle is not over the centre point"
    NothingProbed, // "G1029 S0 ignored: nothing probed": nothing changed
    NotStored,     // a report line, but "(not set)"
    Legacy,        // no Z-offset line at all: a250t.3 or older (after G1029 A the mesh was shifted and saved)
};
// After G1029 A the firmware shifts and saves the mesh even when the offset is refused (OutOfRange, NotAtCentre);
// after G1029 Z a refusal saves nothing. The caller knows which flow it ran.
SaveResult  classify_save_reply(const std::string& body);
const char* save_result_name(SaveResult result);

// Form fields for api/v1/execute_code: token and the first line of `code` only (the touchscreen ignores the rest).
std::vector<std::pair<std::string, std::string>> execute_code_fields(const std::string& token, const std::string& code);

// Values from an api/v1/status body (touchscreen MainRequestHandler.getStatus). Used for the levelling progress and
// by the monitor panel.
struct StatusSnapshot
{
    bool        valid { false }; // JSON object
    std::string status;          // "IDLE" / "RUNNING" / "PAUSED" (upper case), empty if missing: the controller state
    std::string print_status;    // "Idle" / "Printing" / "Paused" / "Complete" as sent: the touchscreen's job state
    std::string tool_head;       // deprecated "toolHead" key, e.g. "TOOLHEAD_3DPRINTING_1", "unplugged"
    bool        has_bed { false };
    double      bed_c { 0. };
    double      bed_target_c { 0. };
    bool        has_z { false };
    double      z { 0. };
    // Nozzles: single head "nozzleTemperature"/"nozzleTargetTemperature" (1), dual head "nozzleTemperature1/2" and
    // "nozzleTargetTemperature1/2" (2). 0 for laser / CNC / unplugged.
    int         nozzles { 0 };
    double      nozzle_c[2] { 0., 0. };
    double      nozzle_target_c[2] { 0., 0. };
    // "workSpeed" is the controller's current feed rate (mm/min, already scaled by the override), NOT the override %.
    bool        has_feed_rate { false };
    double      feed_rate { 0. };
    // Job block, only sent while the touchscreen's job state is printing/paused (isStatePrinting).
    bool        has_job { false };
    std::string file_name;
    double      progress { 0. };      // 0..1
    double      elapsed_s { 0. };     // "elapsedTime", tick counter (seconds)
    double      remaining_s { 0. };   // "remainingTime", the touchscreen's own blend of elapsed and estimate
    double      estimated_s { 0. };   // "estimatedTime" from the file header
    double      current_line { 0. };
    double      total_lines { 0. };
    // Flags and add-on modules.
    bool        filament_out { false };
    bool        emergency_stopped { false };
    bool        enclosure_module { false }; // moduleList.enclosure
    bool        purifier_module { false };  // moduleList.airPurifier
    bool        has_door { false };
    bool        door_open { false };
    bool        has_purifier_state { false };
    bool        purifier_on { false };
    int         purifier_fan_speed { 0 };
};
StatusSnapshot parse_status(const std::string& status_body);

// ---------------------------------------------------------------------------------------------------------------
// Monitor panel (proposal O1, src/slic3r/GUI/Snapmaker2MonitorPanel.cpp). Touchscreen routes used (MainRequestHandler.java,
// every one a form POST with token=<t>; none goes through execute_code, which is refused while a job runs):
//   pause_print / resume_print / stop_print       -> 200 {code:0}, 409 {code:n}; the reply waits for the controller
//                                                    (504 after 30 s on our screen build, no limit on stock)
//   override_nozzle_temperature           nozzleTemp=<C>                            single extruder
//   override_nozzle_temperature_by_index  extruderTemperature=<C>&extruderIndex=<0|1>  dual extruder
//   override_bed_temperature              heatedBedTemp=<C>
//   override_z_offset                     zOffset=<mm>  ABSOLUTE live Z offset (OnAirGcodeModifier keeps the value,
//                                         the controller moves by new - old, BedLevelService::UpdateLiveZOffset), range
//                                         -0.5..2.0 mm, an unchanged value is ignored; reset to 0 when a job starts
//   override_work_speed                   workSpeed=<%>  feed-rate percentage, controller accepts 1..500, reset to 100 per job
//   enclosure                             led=<0..100> | fan=<0..100>   (Luban toggles 0 / 100)
//   air_purifier_switch                   switch=true|false
//   air_purifier_fan_speed                fan_speed=<1..3>
// Nozzle, bed and Z offset answer 409 "Wrong head type." without a 3D printing head. Idle, the nozzle and bed routes send
// M104 / M140 directly, so the panel offers temperature overrides only while a job is running or paused.
// There is no route that reads the live Z offset or the override percentages back.

constexpr int    MONITOR_NOZZLE_MAX_C      = 275;  // controller caps targets at MAXTEMP - 15: T0 290 - 15, T1 275 - 15 = 260
constexpr int    MONITOR_BED_MAX_C         = 110;  // A250 bed, same limit as the levelling contract (controller < 130)
constexpr double MONITOR_Z_OFFSET_MIN_MM   = -0.5; // LIVE_Z_OFFSET_MIN, bed_level.h
constexpr double MONITOR_Z_OFFSET_MAX_MM   = 2.0;  // LIVE_Z_OFFSET_MAX
constexpr double MONITOR_Z_OFFSET_STEP_MM  = 0.05;
constexpr int    MONITOR_WORK_SPEED_MIN    = 10;   // controller accepts > 0.9999; a lower floor keeps a mis-typed 0/1 away
constexpr int    MONITOR_WORK_SPEED_MAX    = 500;
constexpr int    MONITOR_WORK_SPEED_DEFAULT = 100;
constexpr int    MONITOR_PURIFIER_LEVEL_MIN = 1;   // Luban SPEED_LOW .. SPEED_HIGH
constexpr int    MONITOR_PURIFIER_LEVEL_MAX = 3;
// The touchscreen drops a session 5 s after its last request; past this without a good poll the session is gone.
constexpr long   MONITOR_LINK_LOST_MS      = 6000;
// Disconnect after this long without a click in the panel (app config [snapmaker2_monitor] idle_disconnect_min, 0 = never).
constexpr int    MONITOR_IDLE_DISCONNECT_MIN_DEFAULT = 10;

// One request for the monitor panel: path for make_url() plus the form fields (token first).
struct Command
{
    std::string                                      path;
    std::vector<std::pair<std::string, std::string>> fields;
    std::string                                      what; // for the log, e.g. "bed temperature 60 C"
    bool        valid() const { return !path.empty(); }
};

Command pause_command(const std::string& token);
Command resume_command(const std::string& token);
Command stop_command(const std::string& token);
// Clamped to 0..MONITOR_NOZZLE_MAX_C, whole degrees. `nozzle_count` >= 2 (dual head) uses the by-index route; an index
// outside 0..nozzle_count-1 gives an invalid command.
Command nozzle_temperature_command(const std::string& token, int index, int nozzle_count, double temp_c);
Command bed_temperature_command(const std::string& token, double temp_c);    // 0..MONITOR_BED_MAX_C
Command z_offset_command(const std::string& token, double absolute_mm);      // clamped to the live Z offset range
Command work_speed_command(const std::string& token, int percent);           // MONITOR_WORK_SPEED_MIN..MAX
Command enclosure_light_command(const std::string& token, bool on);          // led=100 / led=0
Command enclosure_fan_command(const std::string& token, bool on);            // fan=100 / fan=0
Command purifier_switch_command(const std::string& token, bool on);          // switch=true / switch=false
Command purifier_fan_speed_command(const std::string& token, int level);     // 1..3

// Live Z offset after one +/- click: `current` + `step`, rounded to 0.01 mm and clamped to the controller's range.
double next_z_offset(double current_mm, double step_mm);

// What the panel shows as the job state.
enum class MonitorState {
    Unknown,  // no (valid) status yet
    Idle,
    Printing, // status RUNNING
    Paused,   // status PAUSED
    Finished, // status IDLE and printStatus "Complete"
};
MonitorState monitor_state(const StatusSnapshot& snapshot);
const char*  monitor_state_name(MonitorState state);

// Which controls the panel enables. Everything is off while a command is in flight, for a read-only session
// (connect "readonly": stock screens always say false) and without a valid status.
struct MonitorControls
{
    bool pause { false };
    bool resume { false };
    bool stop { false };
    bool nozzle { false };     // temperature overrides: job running or paused and a 3D printing head
    bool bed { false };
    bool z_offset { false };
    bool work_speed { false }; // job running or paused
    bool enclosure { false };  // enclosure present (connect hasEnclosure or moduleList.enclosure)
    bool purifier { false };   // moduleList.airPurifier
    int  nozzle_count { 1 };
};
MonitorControls monitor_controls(const StatusSnapshot& snapshot, const ConnectInfo& info, bool command_running);

// A new job started between two status snapshots: the touchscreen then resets its Z offset (0) and speed (100 %)
// overrides, so the panel resets what it last sent.
bool job_changed(const StatusSnapshot& previous, const StatusSnapshot& current);

enum class LinkState {
    Ok,      // last keep-alive poll answered 200
    Waiting, // no good poll for a moment (or none yet), still inside the touchscreen's 5 s window
    Lost,    // 401 (the touchscreen ended the session: expired, an upload's disconnect, or another client) or no good
             // poll for MONITOR_LINK_LOST_MS. The panel stops and waits for the user's Connect: never reconnect on its own,
             // that would take the touchscreen back from Luban.
};
LinkState   link_state(unsigned last_http, long ms_since_last_ok);
const char* link_state_name(LinkState state);

// True when the panel should disconnect after `idle_s` seconds without a click; `limit_min` <= 0 never disconnects.
bool idle_disconnect_due(long idle_s, int limit_min);

// "1 h 05 min", "12 min 30 s", "45 s"; "-" for a negative or non-finite value.
std::string format_duration(double seconds);
// 0..100, rounded; 0 for a non-finite value.
int progress_percent(double progress);

// Logging helpers: never log a full token, and keep error bodies short.
std::string mask_token(const std::string& token);
std::string mask_token_in(const std::string& text, const std::string& token);
std::string truncate(const std::string& text, size_t max_len = 300);

} // namespace Snapmaker2Protocol
} // namespace Slic3r

#endif // slic3r_Snapmaker2Protocol_hpp_
