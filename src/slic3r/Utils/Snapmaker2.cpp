#include "Snapmaker2.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/progdlg.h>
#include <wx/thread.h>

#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/format.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {

namespace proto = Snapmaker2Protocol;

namespace {

constexpr long CONNECT_TIMEOUT_S        = 5;
constexpr long SHORT_REQUEST_TIMEOUT_S  = 10;
constexpr long START_PRINT_TIMEOUT_S    = 120; // Luban uses 120 s for start/pause/resume/stop
constexpr int  TEST_APPROVAL_TIMEOUT_S   = 60;
constexpr int  UPLOAD_APPROVAL_TIMEOUT_S = 120;

// Tokens approved during this run of the application, per printer URL. Lets later uploads reuse an approved
// token even when printhost_apikey was never saved (upload jobs cannot write the printer preset).
std::mutex                         g_token_cache_mutex;
std::map<std::string, std::string> g_token_cache;

std::string cached_token(const std::string& key)
{
    std::lock_guard<std::mutex> lock(g_token_cache_mutex);
    auto it = g_token_cache.find(key);
    return it == g_token_cache.end() ? std::string() : it->second;
}

void cache_token(const std::string& key, const std::string& token)
{
    std::lock_guard<std::mutex> lock(g_token_cache_mutex);
    g_token_cache[key] = token;
}

// Polls api/v1/status in the background so the touchscreen does not drop the session (it closes a session
// after ~5 s without requests) while a long multipart upload or the start_print call is in flight.
class SessionKeepAlive
{
public:
    SessionKeepAlive(std::string url, std::string log_url) : m_url(std::move(url)), m_log_url(std::move(log_url))
    {
        m_thread = std::thread([this]() { run(); });
    }
    ~SessionKeepAlive()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: keep-alive stopped after " << m_polls << " polls, " << m_failures << " failed";
    }

private:
    void run()
    {
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: keep-alive started, GET " << m_log_url << " every " << proto::POLL_INTERVAL_MS << " ms";
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_cv.wait_for(lock, std::chrono::milliseconds(proto::POLL_INTERVAL_MS), [this]() { return m_stop; })) {
            lock.unlock();
            unsigned status = 0;
            std::string error;
            auto http = Http::get(m_url);
            http.timeout_connect(3)
                .timeout_max(4)
                .on_complete([&](std::string, unsigned s) { status = s; })
                .on_error([&](std::string, std::string e, unsigned s) { status = s; error = std::move(e); })
                .perform_sync();
            ++m_polls;
            if (status != 200) {
                // Log failures only (a 2 s poll is not worth an info line each time).
                if (++m_failures <= 3 || m_failures % 10 == 0)
                    BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: keep-alive GET " << m_log_url << " -> HTTP " << status << " " << error;
            }
            lock.lock();
        }
    }

    std::string             m_url;
    std::string             m_log_url;
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_stop { false };
    std::atomic<int>        m_polls { 0 };
    std::atomic<int>        m_failures { 0 };
};

void notify_user(const wxString& text)
{
    const std::string utf8 = text.ToUTF8().data();
    GUI::wxGetApp().CallAfter([utf8]() {
        if (auto* nm = GUI::wxGetApp().notification_manager(); nm != nullptr)
            nm->push_notification(utf8);
    });
}

} // namespace

Snapmaker2::Snapmaker2(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_saved_token(config->opt_string("printhost_apikey"))
{}

const char* Snapmaker2::get_name() const { return "Snapmaker 2.0"; }

std::string Snapmaker2::log_url(const std::string& path) const
{
    return proto::mask_token_in(proto::make_url(m_host, path), m_token);
}

Snapmaker2::Response Snapmaker2::post_form(const std::string&                                     path,
                                           const std::vector<std::pair<std::string, std::string>>& fields,
                                           long                                                    timeout_s) const
{
    Response    r;
    const auto  url  = proto::make_url(m_host, path);
    std::string body = proto::form_body(fields);
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: POST " << log_url(path) << " (" << fields.size() << " form fields)";

    auto http = Http::post(url);
    http.header("Content-Type", "application/x-www-form-urlencoded")
        .set_post_body(body)
        .timeout_connect(CONNECT_TIMEOUT_S)
        .timeout_max(timeout_s)
        .on_complete([&](std::string b, unsigned status) {
            r.status = status;
            r.body   = std::move(b);
        })
        .on_error([&](std::string b, std::string error, unsigned status) {
            r.status = status;
            r.body   = std::move(b);
            r.error  = std::move(error);
        })
        .perform_sync();
    r.kind = proto::classify(r.status);

    if (r.kind == proto::Reply::Ok)
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: POST " << log_url(path) << " -> HTTP " << r.status;
    else
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: POST " << log_url(path) << " -> HTTP " << r.status << " ("
                                   << proto::reply_name(r.kind) << ") error: '" << r.error << "' body: '"
                                   << proto::mask_token_in(proto::truncate(r.body), m_token) << "'";
    return r;
}

Snapmaker2::Response Snapmaker2::get(const std::string& path, long timeout_s) const
{
    Response r;
    auto     http = Http::get(proto::make_url(m_host, path));
    http.timeout_connect(CONNECT_TIMEOUT_S)
        .timeout_max(timeout_s)
        .on_complete([&](std::string b, unsigned status) {
            r.status = status;
            r.body   = std::move(b);
        })
        .on_error([&](std::string b, std::string error, unsigned status) {
            r.status = status;
            r.body   = std::move(b);
            r.error  = std::move(error);
        })
        .perform_sync();
    r.kind = proto::classify(r.status);
    if (r.kind != proto::Reply::Ok && r.kind != proto::Reply::PendingApproval)
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: GET " << log_url(path) << " -> HTTP " << r.status << " ("
                                   << proto::reply_name(r.kind) << ") error: '" << r.error << "' body: '"
                                   << proto::truncate(r.body) << "'";
    return r;
}

wxString Snapmaker2::describe_failure(const char* step, const Response& r) const
{
    using proto::Reply;
    switch (r.kind) {
    case Reply::NetworkError:
        return format_error(r.body, r.error, 0) + "\n" +
               _L("Check the IP address, that the Snapmaker 2.0 is switched on and that Wi-Fi is connected on its touchscreen.");
    case Reply::Busy:
        return _L("Another client (for example Luban) is connected to this Snapmaker 2.0 or is waiting for approval. "
                  "Disconnect it, or answer its request on the touchscreen, then try again.");
    case Reply::NotAuthorised:
        return _L("The touchscreen closed the connection (session expired or taken over by another client). Please try again.");
    case Reply::PendingApproval:
        return _L("The connection has not been accepted on the touchscreen yet.");
    case Reply::Conflict:
        if (std::string(step) == "prepare_print")
            return _L("The touchscreen refused the G-code file type. Make sure the 3D printing module (single or dual extruder) is mounted "
                      "and selected on the machine.");
        return GUI::format_wxstr(_L("The touchscreen refused the request (%1%): %2%"), step, r.body);
    default: return GUI::format_wxstr("%1%: %2%", step, format_error(r.body, r.error, r.status));
    }
}

bool Snapmaker2::open_session(wxString& msg, proto::ConnectInfo& info, ApprovalUi ui, int approval_timeout_s) const
{
    m_granted              = false;
    const std::string key  = proto::make_url(m_host, "");
    // A token approved during this run wins over printhost_apikey: the saved one may be stale (touchscreen reset,
    // or evicted from its list of 10), and upload jobs cannot rewrite the preset, so preferring the saved token
    // would ask for approval on every upload.
    std::string token        = cached_token(key);
    const char* token_source = "session cache";
    if (token.empty()) {
        token        = m_saved_token;
        token_source = token.empty() ? "none" : "printhost_apikey";
    }
    m_token = token;
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: connecting to " << key << " with token " << proto::mask_token(token) << " (from "
                            << token_source << ")";

    // "token=" without a token (see Snapmaker2Protocol::connect_body); the touchscreen then creates a new session.
    Response r;
    {
        const auto url = proto::make_url(m_host, "api/v1/connect");
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: POST " << url;
        auto http = Http::post(url);
        http.header("Content-Type", "application/x-www-form-urlencoded")
            .set_post_body(proto::connect_body(token))
            .timeout_connect(CONNECT_TIMEOUT_S)
            .timeout_max(SHORT_REQUEST_TIMEOUT_S)
            .on_complete([&](std::string b, unsigned status) {
                r.status = status;
                r.body   = std::move(b);
            })
            .on_error([&](std::string b, std::string error, unsigned status) {
                r.status = status;
                r.body   = std::move(b);
                r.error  = std::move(error);
            })
            .perform_sync();
        r.kind = proto::classify(r.status);
        if (r.kind == proto::Reply::Ok)
            BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: POST " << url << " -> HTTP " << r.status;
        else
            BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: POST " << url << " -> HTTP " << r.status << " (" << proto::reply_name(r.kind)
                                       << ") error: '" << r.error << "' body: '" << proto::truncate(r.body) << "'";
    }
    if (r.kind != proto::Reply::Ok) {
        msg = describe_failure("connect", r);
        return false;
    }

    info = proto::parse_connect(r.body);
    if (!info.valid) {
        BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: connect answered without a token: '" << proto::truncate(r.body) << "'";
        msg = _L("The printer answered, but not like a Snapmaker 2.0 touchscreen (no token in the reply). Is this the right IP address?");
        return false;
    }
    if (info.token != token)
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: touchscreen issued new token " << proto::mask_token(info.token)
                                << (token.empty() ? "" : " (the stored token was unknown to it, e.g. after a reset)");
    m_token = info.token;
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: machine series '" << info.series << "', headType " << info.head_type << " ("
                            << proto::head_type_name(info.head_type) << "), enclosure " << info.has_enclosure;

    if (!wait_for_approval(msg, ui, approval_timeout_s))
        return false;

    m_granted = true;
    cache_token(key, m_token);
    return true;
}

bool Snapmaker2::wait_for_approval(wxString& msg, ApprovalUi ui, int approval_timeout_s) const
{
    const auto  path     = proto::status_path(m_token);
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(approval_timeout_s);
    std::unique_ptr<wxProgressDialog> dialog;
    bool        notified = false;
    int         polls    = 0;
    const wxString waiting_text = _L("Waiting for approval: tap \"Yes\" on the Snapmaker 2.0 touchscreen to allow Snapmaker Orca to connect.");

    for (;;) {
        Response r = get(path, 4);
        ++polls;
        if (r.kind == proto::Reply::Ok) {
            m_last_status = r.body;
            BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: session approved after " << polls << " status poll(s), machine status '"
                                    << proto::machine_status(r.body) << "'";
            return true;
        }
        if (r.kind != proto::Reply::PendingApproval) {
            msg = describe_failure("status", r);
            return false;
        }
        if (polls == 1) {
            BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: waiting up to " << approval_timeout_s << " s for approval on the touchscreen (GET "
                                    << log_url(path) << " -> 204)";
            if (ui == ApprovalUi::ProgressDialog && wxThread::IsMain())
                dialog = std::make_unique<wxProgressDialog>(_L("Snapmaker 2.0"), waiting_text, 100, nullptr,
                                                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
            else if (ui == ApprovalUi::Notification && !notified) {
                notify_user(waiting_text);
                notified = true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: approval timed out after " << approval_timeout_s << " s";
            msg = GUI::format_wxstr(_L("Nobody accepted the connection on the touchscreen within %1% seconds. "
                                       "Tap \"Yes\" on the Snapmaker 2.0 touchscreen when it asks, then try again."),
                                    approval_timeout_s);
            return false;
        }
        // Sleep until the next poll; keep the progress dialog responsive and honour Cancel.
        for (int slept = 0; slept < proto::POLL_INTERVAL_MS; slept += 100) {
            if (dialog && !dialog->Pulse()) {
                BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: approval wait cancelled by the user";
                msg = _L("Cancelled while waiting for approval on the touchscreen.");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Snapmaker2::close_session() const
{
    if (m_token.empty())
        return;
    // Release the session so that Luban or another client can connect right away (best effort).
    post_form("api/v1/disconnect", { { "token", m_token } }, SHORT_REQUEST_TIMEOUT_S);
}

bool Snapmaker2::test(wxString& msg) const
{
    m_test_ok_detail.clear();
    proto::ConnectInfo info;
    if (!open_session(msg, info, ApprovalUi::ProgressDialog, TEST_APPROVAL_TIMEOUT_S))
        return false;

    m_test_ok_detail = GUI::format_wxstr(_L("Connected to Snapmaker 2.0 %1% with the %2%. Machine status: %3%."),
                                         info.series.empty() ? std::string("(unknown model)") : info.series,
                                         proto::head_type_name(info.head_type), proto::machine_status(m_last_status));
    if (m_token != m_saved_token)
        m_test_ok_detail += "\n" + _L("The access token approved on the touchscreen was stored in the API key field. "
                                      "Save the printer settings to keep it, so the touchscreen will not ask again.");
    if (!proto::is_printing_head(info.head_type))
        m_test_ok_detail += "\n" + _L("Warning: the mounted toolhead is not a 3D printing module, so G-code uploads will be refused.");
    close_session();
    return true;
}

wxString Snapmaker2::get_test_ok_msg() const
{
    return m_test_ok_detail.empty() ? _L("Connection to the Snapmaker 2.0 touchscreen is working correctly.") : m_test_ok_detail;
}

wxString Snapmaker2::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%1%: %2%", _L("Could not connect to the Snapmaker 2.0 touchscreen"), msg);
}

nlohmann::json Snapmaker2::get_auth_info()
{
    nlohmann::json j = nlohmann::json::object();
    if (m_granted && !m_token.empty())
        j["printhost_apikey"] = m_token;
    return j;
}

bool Snapmaker2::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn /*info_fn*/) const
{
    const std::string filename = upload_data.upload_path.filename().empty() ? upload_data.source_path.filename().string() :
                                                                             upload_data.upload_path.filename().string();
    const bool        start    = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    boost::system::error_code ec;
    const auto        size     = fs::file_size(upload_data.source_path, ec);
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: upload of '" << upload_data.source_path.string() << "' as '" << filename << "' ("
                            << (ec ? std::string("size unknown") : std::to_string(size) + " bytes") << ") to " << m_host
                            << ", start print: " << start;

    // "Print plate" hands over a .gcode.3mf project; the touchscreen can only run plain G-code.
    if (upload_data.use_3mf || boost::iends_with(upload_data.source_path.string(), ".3mf")) {
        BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: refusing to upload a 3MF project, the touchscreen only accepts G-code";
        error_fn(_L("The Snapmaker 2.0 touchscreen can only print G-code files, not 3MF projects. Use \"Send\" (send G-code) instead."));
        return false;
    }

    wxString           msg;
    proto::ConnectInfo info;
    if (!open_session(msg, info, ApprovalUi::Notification, UPLOAD_APPROVAL_TIMEOUT_S)) {
        error_fn(get_test_failed_msg(msg));
        return false;
    }

    if (info.head_type > 0 && !proto::is_printing_head(info.head_type)) {
        BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: refusing upload, mounted toolhead is " << proto::head_type_name(info.head_type);
        error_fn(GUI::format_wxstr(_L("The Snapmaker 2.0 has the %1% mounted. Mount the 3D printing module before sending G-code."),
                                   proto::head_type_name(info.head_type)));
        close_session();
        return false;
    }
    const std::string machine_status = proto::machine_status(m_last_status);
    if (proto::is_busy(machine_status)) {
        // prepare_print would replace the workspace file of the job that is running or paused.
        BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: refusing upload, machine status is " << machine_status;
        error_fn(GUI::format_wxstr(_L("The Snapmaker 2.0 is busy (%1%). Finish or stop the current job first."), machine_status));
        close_session();
        return false;
    }

    bool result = true;
    {
        SessionKeepAlive keep_alive(proto::make_url(m_host, proto::status_path(m_token)), log_url(proto::status_path(m_token)));

        // prepare_print loads the file into the touchscreen workspace (same call Luban uses), fields as in Luban:
        // token, type=3DP, file.
        const std::string path = "api/v1/prepare_print";
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: POST " << log_url(path) << " multipart token, type=" << proto::FILE_TYPE_3DP << ", file='"
                                << filename << "'";
        Response r;
        auto     http = Http::post(proto::make_url(m_host, path));
        http.form_add("token", m_token)
            .form_add("type", proto::FILE_TYPE_3DP)
            .form_add_file("file", upload_data.source_path, filename)
            .timeout_connect(CONNECT_TIMEOUT_S)
            .on_complete([&](std::string b, unsigned status) {
                r.status = status;
                r.body   = std::move(b);
            })
            .on_error([&](std::string b, std::string error, unsigned status) {
                r.status = status;
                r.body   = std::move(b);
                r.error  = std::move(error);
            })
            .on_progress([&](Http::Progress progress, bool& cancel) {
                Http::Progress prog = std::move(progress);
                // Keep the progress bar below 100 % until start_print has been sent as well (same trick as ESP3D).
                if (start && prog.ulnow > 0)
                    prog.ulnow -= 1;
                if (progress_fn)
                    progress_fn(std::move(prog), cancel);
                if (cancel) {
                    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: upload cancelled by the user";
                    result = false;
                }
            })
            .perform_sync();
        r.kind = proto::classify(r.status);

        if (!result) {
            // cancelled
        } else if (r.kind != proto::Reply::Ok) {
            BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: POST " << log_url(path) << " -> HTTP " << r.status << " (" << proto::reply_name(r.kind)
                                     << ") error: '" << r.error << "' body: '" << proto::truncate(r.body) << "'";
            error_fn(describe_failure("prepare_print", r));
            result = false;
        } else {
            BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: POST " << log_url(path) << " -> HTTP " << r.status << " '" << proto::truncate(r.body) << "'";
        }

        if (result && start) {
            Response s = post_form("api/v1/start_print", { { "token", m_token } }, START_PRINT_TIMEOUT_S);
            if (s.kind != proto::Reply::Ok) {
                const int code = proto::start_print_code(s.body);
                BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: start_print failed, code " << code;
                error_fn(describe_failure("start_print", s) +
                         (code >= 0 ? GUI::format_wxstr(" (code %1%)", code) : wxString()) + "\n" +
                         _L("The file was uploaded; you can start it from the touchscreen."));
                result = false;
            } else {
                BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: print started, code " << proto::start_print_code(s.body);
            }
        }
    } // keep-alive stops here

    close_session();
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: upload finished, result " << result;
    return result;
}

} // namespace Slic3r
