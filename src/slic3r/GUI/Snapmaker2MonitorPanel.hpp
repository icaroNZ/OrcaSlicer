#ifndef slic3r_GUI_Snapmaker2MonitorPanel_hpp_
#define slic3r_GUI_Snapmaker2MonitorPanel_hpp_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <wx/panel.h>
#include <wx/timer.h>

#include "slic3r/Utils/Snapmaker2.hpp"

class wxButton;
class wxGauge;
class wxSizer;
class wxSpinCtrl;
class wxStaticText;
class wxTimerEvent;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// Device tab for the "Snapmaker 2.0 (HTTP)" print host (proposal O1, _snapmaker2-review/proposals/OrcaSlicer.md).
// PrinterWebView shows it instead of the web view when the printer preset's host_type is snapmaker2: the touchscreen
// serves JSON on port 8080, not a web page, so the browser stayed blank.
//
// Session discipline (the stock touchscreen allows one client, answers 403 to any other connect while a session is
// current, and shows its Remote page while one is open):
//  - connects only when the user clicks Connect, never on its own (Luban / Cura would get 403);
//  - disconnects when the Device tab is left (PrinterWebView::on_page_hidden), when the printer changes, after
//    [snapmaker2_monitor] idle_disconnect_min minutes without a click (default 10, 0 = never) and hands the session to an
//    upload of this Orca run (Snapmaker2::upload_in_progress);
//  - after a 401 or 6 s without a good poll it stops and waits for the next Connect click (no automatic reconnect).
// Status comes from the keep-alive poll (Snapmaker2::last_status_body), parsed and gated by Snapmaker2Protocol
// (parse_status, monitor_controls, link_state); commands run on a worker thread (Snapmaker2::run_command), one at a time.
class Snapmaker2MonitorPanel : public wxPanel
{
public:
    explicit Snapmaker2MonitorPanel(wxWindow* parent);
    ~Snapmaker2MonitorPanel() override;

    // Printer to monitor (print_host / printhost_apikey of the printer preset). Another host disconnects first.
    void set_printer(const DynamicPrintConfig& config);
    // Stop polling and release the touchscreen. Safe when not connected. `reason` goes to the log and the status line.
    void disconnect_session(const char* reason);
    bool connected() const;

private:
    struct WorkerResult
    {
        bool                  done { false };
        wxString              what;     // for the user, e.g. "pause"
        std::string           step;     // touchscreen route for describe_failure, e.g. "pause_print"
        Snapmaker2::Response  response;
        std::function<void()> on_ok; // runs on the GUI thread after a 2xx reply
    };

    std::unique_ptr<Snapmaker2>        m_host;
    std::string                        m_print_host;
    Snapmaker2Protocol::ConnectInfo    m_info;
    Snapmaker2Protocol::StatusSnapshot m_last;
    // Last snapshot with a job block, kept across reconnects so a reconnect in the same job keeps what Orca sent.
    Snapmaker2Protocol::StatusSnapshot m_job_seen;
    // What Orca last sent; the touchscreen has no route to read these back, and resets both when a job starts.
    double                             m_z_offset_mm { 0. };
    int                                m_work_speed { Snapmaker2Protocol::MONITOR_WORK_SPEED_DEFAULT };
    bool                               m_spins_loaded { false };
    int                                m_layout_key { -1 }; // which optional rows are shown; Layout() only when it changes
    std::chrono::steady_clock::time_point m_last_ok;
    std::chrono::steady_clock::time_point m_last_action;

    std::thread       m_worker;
    std::atomic<bool> m_cancel { false };
    std::mutex        m_result_mutex;
    WorkerResult      m_result;
    bool              m_command_running { false };

    wxTimer m_timer;

    wxStaticText* m_banner { nullptr };
    wxButton*     m_connect_btn { nullptr };
    wxButton*     m_disconnect_btn { nullptr };
    wxStaticText* m_link { nullptr };
    wxStaticText* m_state { nullptr };
    wxStaticText* m_file { nullptr };
    wxGauge*      m_gauge { nullptr };
    wxStaticText* m_progress { nullptr };
    wxStaticText* m_elapsed { nullptr };
    wxStaticText* m_remaining { nullptr };
    wxStaticText* m_nozzle_text { nullptr };
    wxStaticText* m_bed_text { nullptr };
    wxStaticText* m_feed_text { nullptr };
    wxStaticText* m_warnings { nullptr };
    wxButton*     m_pause_btn { nullptr };
    wxButton*     m_resume_btn { nullptr };
    wxButton*     m_stop_btn { nullptr };
    wxSpinCtrl*   m_nozzle_spin[2] { nullptr, nullptr };
    wxButton*     m_nozzle_btn[2] { nullptr, nullptr };
    wxSizer*      m_nozzle_row[2] { nullptr, nullptr };
    wxSpinCtrl*   m_bed_spin { nullptr };
    wxButton*     m_bed_btn { nullptr };
    wxButton*     m_z_down_btn { nullptr };
    wxButton*     m_z_up_btn { nullptr };
    wxStaticText* m_z_text { nullptr };
    wxSpinCtrl*   m_speed_spin { nullptr };
    wxButton*     m_speed_btn { nullptr };
    wxSizer*      m_enclosure_row { nullptr };
    wxButton*     m_light_on_btn { nullptr };
    wxButton*     m_light_off_btn { nullptr };
    wxButton*     m_fan_on_btn { nullptr };
    wxButton*     m_fan_off_btn { nullptr };
    wxSizer*      m_purifier_row { nullptr };
    wxButton*     m_purifier_on_btn { nullptr };
    wxButton*     m_purifier_off_btn { nullptr };
    wxSizer*      m_main_sizer { nullptr };

    void build_ui();
    void on_connect();
    void on_timer(wxTimerEvent& event);
    void on_stop();
    void on_z_step(double step_mm);
    // Runs one command on the worker; ignored while another one runs or when not connected.
    void start_command(const wxString& what, const char* step, long timeout_s,
                       std::function<Snapmaker2Protocol::Command(const std::string&)> build, std::function<void()> on_ok = {});
    void finish_command();
    void end_session(bool send_disconnect, const wxString& why);
    void update_view();
    void touch() { m_last_action = std::chrono::steady_clock::now(); }
    void stop_worker();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Snapmaker2MonitorPanel_hpp_
