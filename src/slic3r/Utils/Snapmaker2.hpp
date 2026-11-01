#ifndef slic3r_Snapmaker2_hpp_
#define slic3r_Snapmaker2_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"
#include "Snapmaker2Protocol.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Print host for the Snapmaker 2.0 (A150 / A250 / A250T / A350) touchscreen HTTP API on port 8080.
// Host type "snapmaker2", shown as "Snapmaker 2.0 (HTTP)".
//
// Ported from hiarase/OrcaSlicer@f3d46d162 (HTTP 8080 half of src/slic3r/Utils/SnapmakerJ1.cpp only, the
// SACP / TCP 8888 J1 path was dropped) and fixed:
//  - the token returned by api/v1/connect is handed back through get_auth_info() so the physical printer
//    dialog stores it in printhost_apikey (the touchscreen remembers approved tokens, so it asks only once);
//  - the client waits, polling api/v1/status, until the touchscreen user taps "Yes" (HTTP 204 -> 200),
//    instead of failing straight away;
//  - a keep-alive poll runs during the upload, because the touchscreen drops a session after ~5 s without
//    requests and only checks the token once the whole multipart body has arrived;
//  - every HTTP step is logged (URL with the token masked, HTTP status, truncated error body).
class Snapmaker2 : public PrintHost
{
public:
    explicit Snapmaker2(DynamicPrintConfig* config);
    ~Snapmaker2() override = default;

    const char* get_name() const override;

    bool                       test(wxString& curl_msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return false; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return m_host; }

    // After a successful test(): {"printhost_apikey": "<token approved on the touchscreen>"}, otherwise {}.
    nlohmann::json get_auth_info() override;

private:
    struct Response
    {
        unsigned                  status { 0 };
        std::string               body;
        std::string               error;
        Snapmaker2Protocol::Reply kind { Snapmaker2Protocol::Reply::NetworkError };
    };

    enum class ApprovalUi { ProgressDialog, Notification, None };

    std::string m_host;
    std::string m_saved_token;            // printhost_apikey from the config
    mutable std::string m_token;          // token of the current session
    mutable std::string m_last_status;    // last api/v1/status body
    mutable bool        m_granted { false };
    mutable wxString    m_test_ok_detail;

    Response post_form(const std::string& path, const std::vector<std::pair<std::string, std::string>>& fields, long timeout_s) const;
    Response get(const std::string& path, long timeout_s) const;

    // connect + wait for approval. On success m_token is a granted token and m_last_status the status body.
    bool open_session(wxString& msg, Snapmaker2Protocol::ConnectInfo& info, ApprovalUi ui, int approval_timeout_s) const;
    bool wait_for_approval(wxString& msg, ApprovalUi ui, int approval_timeout_s) const;
    void close_session() const;

    wxString describe_failure(const char* step, const Response& response) const;
    std::string log_url(const std::string& path) const;
};

} // namespace Slic3r

#endif
