#include "Snapmaker2MonitorPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"

namespace Slic3r {
namespace GUI {

namespace proto = Snapmaker2Protocol;

namespace {

constexpr const char* CONFIG_SECTION     = "snapmaker2_monitor";
constexpr int         TIMER_MS           = 1000;
constexpr long        JOB_COMMAND_TIMEOUT_S = 120; // pause/resume/stop wait for the controller; Luban allows 120 s
constexpr long        OVERRIDE_TIMEOUT_S = 15;     // overrides, enclosure and purifier answer at once (nozzle/bed after M104/M140)

int config_int(const char* key, int fallback)
{
    const std::string value = wxGetApp().app_config->get(CONFIG_SECTION, key);
    if (value.empty())
        return fallback;
    char*      end = nullptr;
    const long v   = std::strtol(value.c_str(), &end, 10);
    return (end == value.c_str()) ? fallback : int(v);
}

int idle_limit_min() { return config_int("idle_disconnect_min", proto::MONITOR_IDLE_DISCONNECT_MIN_DEFAULT); }

wxString degrees(double current, double target)
{
    return wxString::Format("%.1f / %.0f", current, target) + wxString::FromUTF8(" \xC2\xB0" "C");
}

wxString state_text(proto::MonitorState state)
{
    switch (state) {
    case proto::MonitorState::Unknown: return _L("Waiting for the first status...");
    case proto::MonitorState::Idle: return _L("Idle");
    case proto::MonitorState::Printing: return _L("Working");
    case proto::MonitorState::Paused: return _L("Paused");
    case proto::MonitorState::Finished: return _L("Finished");
    }
    return wxString();
}

long seconds_since(std::chrono::steady_clock::time_point t)
{
    return long(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t).count());
}

} // namespace

Snapmaker2MonitorPanel::Snapmaker2MonitorPanel(wxWindow* parent)
    : wxPanel(parent)
    , m_timer(this)
{
    m_last_ok     = std::chrono::steady_clock::now();
    m_last_action = m_last_ok;
    build_ui();
    Bind(wxEVT_TIMER, &Snapmaker2MonitorPanel::on_timer, this);
    update_view();
}

Snapmaker2MonitorPanel::~Snapmaker2MonitorPanel()
{
    m_timer.Stop();
    stop_worker();
    if (m_host && m_host->interactive()) {
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: panel destroyed, releasing the touchscreen";
        m_host->end_interactive();
    }
}

void Snapmaker2MonitorPanel::build_ui()
{
    const int gap  = FromDIP(8);
    const int wrap = FromDIP(640);

    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    m_banner = new wxStaticText(this, wxID_ANY,
                                GUI::format_wxstr(_L("Snapmaker 2.0 (HTTP). Snapmaker Orca connects only when you click Connect. While it is "
                                                     "connected, the touchscreen shows its Remote page and Luban or another computer cannot "
                                                     "connect. Orca disconnects when you leave this tab, before it uploads a file, and after "
                                                     "%1% minutes without a click here."),
                                                  idle_limit_min()));
    m_banner->Wrap(wrap);
    m_main_sizer->Add(m_banner, 0, wxEXPAND | wxALL, gap);

    auto* conn_row  = new wxBoxSizer(wxHORIZONTAL);
    m_connect_btn    = new wxButton(this, wxID_ANY, _L("Connect"));
    m_disconnect_btn = new wxButton(this, wxID_ANY, _L("Disconnect"));
    m_link           = new wxStaticText(this, wxID_ANY, _L("Not connected."));
    conn_row->Add(m_connect_btn, 0, wxRIGHT, gap);
    conn_row->Add(m_disconnect_btn, 0, wxRIGHT, gap);
    conn_row->Add(m_link, 1, wxALIGN_CENTER_VERTICAL);
    m_main_sizer->Add(conn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);
    m_connect_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_connect(); });
    m_disconnect_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { disconnect_session("Disconnect clicked"); });

    // Status
    auto* grid = new wxFlexGridSizer(2, FromDIP(4), FromDIP(16));
    grid->AddGrowableCol(1, 1);
    auto add_row = [this, grid](const wxString& label, wxStaticText*& value) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        value = new wxStaticText(this, wxID_ANY, "-");
        grid->Add(value, 0, wxALIGN_CENTER_VERTICAL);
    };
    add_row(_L("State"), m_state);
    add_row(_L("File"), m_file);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Progress")), 0, wxALIGN_CENTER_VERTICAL);
    auto* progress_row = new wxBoxSizer(wxHORIZONTAL);
    m_gauge            = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(240), -1));
    m_progress         = new wxStaticText(this, wxID_ANY, "-");
    progress_row->Add(m_gauge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    progress_row->Add(m_progress, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(progress_row, 0, wxEXPAND);
    add_row(_L("Elapsed"), m_elapsed);
    add_row(_L("Remaining (touchscreen estimate)"), m_remaining);
    add_row(_L("Nozzle"), m_nozzle_text);
    add_row(_L("Bed"), m_bed_text);
    add_row(_L("Feed rate"), m_feed_text);
    add_row(_L("Warnings"), m_warnings);
    m_main_sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    // Job
    auto* job_row = new wxBoxSizer(wxHORIZONTAL);
    m_pause_btn   = new wxButton(this, wxID_ANY, _L("Pause"));
    m_resume_btn  = new wxButton(this, wxID_ANY, _L("Resume"));
    m_stop_btn    = new wxButton(this, wxID_ANY, _L("Stop") + "...");
    job_row->Add(m_pause_btn, 0, wxRIGHT, gap);
    job_row->Add(m_resume_btn, 0, wxRIGHT, gap);
    job_row->Add(m_stop_btn, 0, wxRIGHT, gap);
    m_main_sizer->Add(job_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    m_pause_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_command(_L("pause"), "pause_print", JOB_COMMAND_TIMEOUT_S, proto::pause_command); });
    m_resume_btn->Bind(wxEVT_BUTTON,
                       [this](wxCommandEvent&) { start_command(_L("resume"), "resume_print", JOB_COMMAND_TIMEOUT_S, proto::resume_command); });
    m_stop_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_stop(); });

    // Overrides (only while a job is running or paused, see Snapmaker2Protocol::monitor_controls)
    m_main_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Live adjustments (while a job runs)")), 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    auto spin_row = [this, gap](const wxString& label, int min, int max, int initial, wxSpinCtrl*& spin, wxButton*& button) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        spin   = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, min, max, initial);
        button = new wxButton(this, wxID_ANY, _L("Set"));
        row->Add(spin, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        row->Add(button, 0, wxALIGN_CENTER_VERTICAL);
        m_main_sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
        return row;
    };
    for (int i = 0; i < 2; ++i) {
        m_nozzle_row[i] = spin_row(i == 0 ? _L("Nozzle temperature (°C)") : _L("Second nozzle temperature (°C)"), 0, proto::MONITOR_NOZZLE_MAX_C,
                                   200, m_nozzle_spin[i], m_nozzle_btn[i]);
        m_nozzle_btn[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            const int value = m_nozzle_spin[i]->GetValue();
            const int count = proto::monitor_controls(m_last, m_info, false).nozzle_count;
            start_command(GUI::format_wxstr(_L("nozzle %1% temperature %2% °C"), i + 1, value), "override_nozzle_temperature", OVERRIDE_TIMEOUT_S,
                          [i, count, value](const std::string& token) { return proto::nozzle_temperature_command(token, i, count, value); });
        });
    }
    spin_row(_L("Bed temperature (°C)"), 0, proto::MONITOR_BED_MAX_C, 60, m_bed_spin, m_bed_btn);
    m_bed_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const int value = m_bed_spin->GetValue();
        start_command(GUI::format_wxstr(_L("bed temperature %1% °C"), value), "override_bed_temperature", OVERRIDE_TIMEOUT_S,
                      [value](const std::string& token) { return proto::bed_temperature_command(token, value); });
    });
    spin_row(_L("Work speed (%)"), proto::MONITOR_WORK_SPEED_MIN, proto::MONITOR_WORK_SPEED_MAX, proto::MONITOR_WORK_SPEED_DEFAULT, m_speed_spin,
             m_speed_btn);
    m_speed_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const int value = m_speed_spin->GetValue();
        start_command(GUI::format_wxstr(_L("work speed %1% %%"), value), "override_work_speed", OVERRIDE_TIMEOUT_S,
                      [value](const std::string& token) { return proto::work_speed_command(token, value); },
                      [this, value]() { m_work_speed = value; });
    });

    auto* z_row  = new wxBoxSizer(wxHORIZONTAL);
    z_row->Add(new wxStaticText(this, wxID_ANY, _L("Live Z offset")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    m_z_down_btn = new wxButton(this, wxID_ANY, wxString::FromUTF8("-" + proto::format_mm(proto::MONITOR_Z_OFFSET_STEP_MM) + " mm"));
    m_z_up_btn   = new wxButton(this, wxID_ANY, wxString::FromUTF8("+" + proto::format_mm(proto::MONITOR_Z_OFFSET_STEP_MM) + " mm"));
    m_z_text     = new wxStaticText(this, wxID_ANY, "-");
    m_z_down_btn->SetToolTip(_L("Nozzle closer to the bed. The touchscreen keeps one absolute offset per job; Orca sends the sum of its "
                                "own clicks and cannot read changes made on the touchscreen."));
    m_z_up_btn->SetToolTip(_L("Nozzle further from the bed."));
    z_row->Add(m_z_down_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    z_row->Add(m_z_up_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    z_row->Add(m_z_text, 0, wxALIGN_CENTER_VERTICAL);
    m_main_sizer->Add(z_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    m_z_down_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_z_step(-proto::MONITOR_Z_OFFSET_STEP_MM); });
    m_z_up_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_z_step(proto::MONITOR_Z_OFFSET_STEP_MM); });

    // Enclosure and air purifier (shown only when present)
    auto on_off_row = [this, gap](const wxString& label, wxButton*& on, wxButton*& off) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        on  = new wxButton(this, wxID_ANY, _L("On"));
        off = new wxButton(this, wxID_ANY, _L("Off"));
        row->Add(on, 0, wxRIGHT, FromDIP(4));
        row->Add(off, 0, wxRIGHT, gap);
        return row;
    };
    m_enclosure_row = on_off_row(_L("Enclosure light"), m_light_on_btn, m_light_off_btn);
    auto* fan_part  = on_off_row(_L("Enclosure fan"), m_fan_on_btn, m_fan_off_btn);
    m_enclosure_row->Add(fan_part, 0);
    m_main_sizer->Add(m_enclosure_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    m_purifier_row = on_off_row(_L("Air purifier"), m_purifier_on_btn, m_purifier_off_btn);
    m_main_sizer->Add(m_purifier_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    auto bind_switch = [this](wxButton* btn, const wxString& what, const char* step, Snapmaker2Protocol::Command (*make)(const std::string&, bool),
                              bool on) {
        btn->Bind(wxEVT_BUTTON, [this, what, step, make, on](wxCommandEvent&) {
            start_command(what, step, OVERRIDE_TIMEOUT_S, [make, on](const std::string& token) { return make(token, on); });
        });
    };
    bind_switch(m_light_on_btn, _L("enclosure light on"), "enclosure", proto::enclosure_light_command, true);
    bind_switch(m_light_off_btn, _L("enclosure light off"), "enclosure", proto::enclosure_light_command, false);
    bind_switch(m_fan_on_btn, _L("enclosure fan on"), "enclosure", proto::enclosure_fan_command, true);
    bind_switch(m_fan_off_btn, _L("enclosure fan off"), "enclosure", proto::enclosure_fan_command, false);
    bind_switch(m_purifier_on_btn, _L("air purifier on"), "air_purifier_switch", proto::purifier_switch_command, true);
    bind_switch(m_purifier_off_btn, _L("air purifier off"), "air_purifier_switch", proto::purifier_switch_command, false);

    SetSizer(m_main_sizer);
}

bool Snapmaker2MonitorPanel::connected() const { return m_host && m_host->interactive(); }

void Snapmaker2MonitorPanel::set_printer(const DynamicPrintConfig& config)
{
    DynamicPrintConfig copy = config;
    const std::string  host = copy.opt_string("print_host");
    if (m_host && host == m_print_host)
        return;
    if (connected())
        disconnect_session("printer changed");
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: printer set to '" << host << "'";
    m_host       = std::make_unique<Snapmaker2>(&copy);
    m_print_host = host;
    m_info       = proto::ConnectInfo();
    m_last       = proto::StatusSnapshot();
    m_job_seen   = proto::StatusSnapshot();
    m_z_offset_mm = 0.;
    m_work_speed  = proto::MONITOR_WORK_SPEED_DEFAULT;
    m_link->SetLabel(_L("Not connected."));
    update_view();
}

void Snapmaker2MonitorPanel::on_connect()
{
    touch();
    if (connected())
        return;
    if (!m_host || m_print_host.empty()) {
        show_error(this, _L("Enter the IP address of the Snapmaker 2.0 in the printer's connection settings first."));
        return;
    }
    if (Snapmaker2::upload_in_progress(m_print_host)) {
        show_error(this, _L("Snapmaker Orca is uploading a file to this printer. Connect again when the upload has finished."));
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: Connect clicked for " << m_print_host;
    m_link->SetLabel(_L("Connecting... tap \"Yes\" on the touchscreen if it asks."));
    wxString           msg;
    proto::ConnectInfo info;
    if (!m_host->begin_interactive(msg, info, "monitor")) {
        m_link->SetLabel(_L("Not connected."));
        show_error(this, m_host->get_test_failed_msg(msg));
        update_view();
        return;
    }
    m_info         = info;
    m_last         = proto::StatusSnapshot();
    m_spins_loaded = false;
    m_last_ok      = std::chrono::steady_clock::now();
    touch();
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: connected, series '" << info.series << "', headType " << info.head_type << " ("
                            << proto::head_type_name(info.head_type) << "), enclosure " << info.has_enclosure << ", readonly "
                            << info.readonly << ", idle disconnect after " << idle_limit_min() << " min";
    m_link->SetLabel(info.readonly ? _L("Connected (read-only session: the controls stay disabled).") : _L("Connected."));
    m_timer.Start(TIMER_MS);
    update_view();
}

void Snapmaker2MonitorPanel::disconnect_session(const char* reason)
{
    if (!connected() && !m_worker.joinable())
        return;
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: disconnecting (" << (reason ? reason : "") << ")";
    end_session(true, _L("Disconnected. The touchscreen is free for Luban or the touchscreen user. Click Connect to watch again."));
}

void Snapmaker2MonitorPanel::end_session(bool send_disconnect, const wxString& why)
{
    m_timer.Stop();
    stop_worker();
    m_command_running = false;
    if (m_host)
        m_host->end_interactive(send_disconnect);
    m_link->SetLabel(why);
    update_view();
}

void Snapmaker2MonitorPanel::on_timer(wxTimerEvent&)
{
    finish_command();
    if (!connected()) {
        m_timer.Stop();
        return;
    }
    if (Snapmaker2::upload_in_progress(m_print_host)) {
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: an upload to " << m_print_host << " started, handing the session over";
        end_session(false, _L("Disconnected: an upload from Snapmaker Orca took over the touchscreen session. Click Connect when it has "
                              "finished."));
        return;
    }

    const unsigned http = m_host->last_status_http();
    if (http == 200)
        m_last_ok = std::chrono::steady_clock::now();
    const long ms_since_ok = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_last_ok).count());
    const proto::LinkState link = proto::link_state(http, ms_since_ok);
    if (link == proto::LinkState::Lost) {
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: monitor: session lost (last poll HTTP " << http << ", " << ms_since_ok
                                   << " ms since the last good poll); not reconnecting automatically";
        // After a 401 the session is not ours any more: no disconnect (it would only get a 401 as well).
        end_session(http != 401, http == 401 ?
                                     _L("The touchscreen ended the session (it expired, another client connected or an upload finished). "
                                        "Click Connect to watch again.") :
                                     _L("Connection lost: no answer from the touchscreen for several seconds. Click Connect to try again."));
        return;
    }

    const proto::StatusSnapshot snap = proto::parse_status(m_host->last_status_body());
    if (snap.valid) {
        if (proto::job_changed(m_job_seen, snap)) {
            BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: job '" << snap.file_name << "' seen, Z offset and speed overrides start "
                                    << "from 0 mm / 100 %";
            m_z_offset_mm  = 0.;
            m_work_speed   = proto::MONITOR_WORK_SPEED_DEFAULT;
            m_spins_loaded = false;
        }
        if (snap.has_job)
            m_job_seen = snap;
        const proto::MonitorState before = proto::monitor_state(m_last), after = proto::monitor_state(snap);
        if (before != after)
            BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: state " << proto::monitor_state_name(before) << " -> "
                                    << proto::monitor_state_name(after) << " (status '" << snap.status << "', job '" << snap.print_status << "')";
        m_last = snap;
    }

    const int limit = idle_limit_min();
    if (proto::idle_disconnect_due(seconds_since(m_last_action), limit)) {
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: no click for " << limit << " min, disconnecting";
        end_session(true, GUI::format_wxstr(_L("Disconnected after %1% minutes without a click here, so Luban and the touchscreen are "
                                               "free again. Click Connect to watch again."),
                                            limit));
        return;
    }
    update_view();
}

void Snapmaker2MonitorPanel::on_stop()
{
    touch();
    if (!connected() || m_command_running)
        return;
    MessageDialog dlg(this,
                      _L("Stop the job on the Snapmaker 2.0? It cannot be resumed afterwards."),
                      _L("Snapmaker 2.0"), wxICON_WARNING | wxYES_NO);
    if (dlg.ShowModal() != wxID_YES) {
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: monitor: stop not confirmed";
        return;
    }
    start_command(_L("stop"), "stop_print", JOB_COMMAND_TIMEOUT_S, proto::stop_command);
}

void Snapmaker2MonitorPanel::on_z_step(double step_mm)
{
    touch();
    const double next = proto::next_z_offset(m_z_offset_mm, step_mm);
    if (next == m_z_offset_mm) {
        m_link->SetLabel(GUI::format_wxstr(_L("The live Z offset is at its limit (%1% to %2% mm)."), proto::format_mm(proto::MONITOR_Z_OFFSET_MIN_MM),
                                           proto::format_mm(proto::MONITOR_Z_OFFSET_MAX_MM)));
        return;
    }
    start_command(GUI::format_wxstr(_L("live Z offset %1% mm"), proto::format_mm(next)), "override_z_offset", OVERRIDE_TIMEOUT_S,
                  [next](const std::string& token) { return proto::z_offset_command(token, next); }, [this, next]() { m_z_offset_mm = next; });
}

void Snapmaker2MonitorPanel::start_command(const wxString& what, const char* step, long timeout_s,
                                           std::function<proto::Command(const std::string&)> build, std::function<void()> on_ok)
{
    touch();
    if (!connected() || m_command_running)
        return;
    if (m_worker.joinable())
        m_worker.join();
    m_cancel = false;
    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_result       = WorkerResult();
        m_result.what  = what;
        m_result.step  = step ? step : "";
        m_result.on_ok = std::move(on_ok);
    }
    m_command_running = true;
    Snapmaker2* host  = m_host.get();
    m_worker          = std::thread([this, host, build = std::move(build), timeout_s]() {
        Snapmaker2::Response r = host->run_command(build, timeout_s, &m_cancel);
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_result.response = std::move(r);
        m_result.done     = true;
    });
    m_link->SetLabel(GUI::format_wxstr(_L("Sending: %1%..."), what));
    update_view();
}

void Snapmaker2MonitorPanel::finish_command()
{
    if (!m_command_running)
        return;
    WorkerResult result;
    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        if (!m_result.done)
            return;
        result = m_result;
    }
    if (m_worker.joinable())
        m_worker.join();
    m_command_running = false;
    const Snapmaker2::Response& r = result.response;
    if (r.cancelled) {
        m_link->SetLabel(GUI::format_wxstr(_L("Stopped waiting for: %1%."), result.what));
    } else if (r.kind == proto::Reply::Ok) {
        if (result.on_ok)
            result.on_ok();
        m_link->SetLabel(GUI::format_wxstr(_L("Accepted by the touchscreen: %1%."), result.what));
    } else {
        const int code = proto::start_print_code(r.body);
        wxString  msg  = m_host->describe_failure(result.step.c_str(), r);
        if (code >= 0)
            msg += GUI::format_wxstr(" (code %1%)", code);
        BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: monitor: " << result.step << " failed, HTTP " << r.status << ", code " << code;
        m_link->SetLabel(msg);
        if (r.kind == proto::Reply::NotAuthorised)
            end_session(false, msg);
        else
            show_error(this, msg);
    }
    update_view();
}

void Snapmaker2MonitorPanel::update_view()
{
    if (m_connect_btn == nullptr)
        return;
    const bool on = connected();
    m_connect_btn->Enable(!on && !m_print_host.empty());
    m_disconnect_btn->Enable(on);

    const proto::StatusSnapshot& s = m_last;
    const proto::MonitorControls c = on ? proto::monitor_controls(s, m_info, m_command_running) : proto::MonitorControls();
    const bool                   show = on && s.valid;

    m_state->SetLabel(on ? state_text(proto::monitor_state(s)) : _L("Not connected"));
    m_file->SetLabel(show && s.has_job && !s.file_name.empty() ? wxString::FromUTF8(s.file_name) : wxString("-"));
    m_gauge->SetValue(show && s.has_job ? proto::progress_percent(s.progress) : 0);
    if (show && s.has_job && s.total_lines > 0)
        m_progress->SetLabel(wxString::Format("%d %%  (%.0f / %.0f)", proto::progress_percent(s.progress), s.current_line, s.total_lines));
    else
        m_progress->SetLabel(show && s.has_job ? wxString::Format("%d %%", proto::progress_percent(s.progress)) : wxString("-"));
    m_elapsed->SetLabel(show && s.has_job ? wxString::FromUTF8(proto::format_duration(s.elapsed_s)) : wxString("-"));
    m_remaining->SetLabel(show && s.has_job ? wxString::FromUTF8(proto::format_duration(s.remaining_s)) : wxString("-"));

    wxString nozzle = "-";
    if (show && s.nozzles == 1)
        nozzle = degrees(s.nozzle_c[0], s.nozzle_target_c[0]);
    else if (show && s.nozzles >= 2)
        nozzle = "1: " + degrees(s.nozzle_c[0], s.nozzle_target_c[0]) + "    2: " + degrees(s.nozzle_c[1], s.nozzle_target_c[1]);
    m_nozzle_text->SetLabel(nozzle);
    m_bed_text->SetLabel(show && s.has_bed ? degrees(s.bed_c, s.bed_target_c) : wxString("-"));
    m_feed_text->SetLabel(show && s.has_feed_rate ? wxString::Format("%.0f mm/min", s.feed_rate) : wxString("-"));

    wxString warnings;
    auto     add_warning = [&warnings](const wxString& w) { warnings += (warnings.empty() ? wxString() : wxString(", ")) + w; };
    if (show && s.filament_out)
        add_warning(_L("filament run-out"));
    if (show && s.has_door && s.door_open)
        add_warning(_L("enclosure door open"));
    if (show && s.emergency_stopped)
        add_warning(_L("emergency stop pressed"));
    m_warnings->SetLabel(warnings.empty() ? wxString("-") : warnings);

    m_z_text->SetLabel(GUI::format_wxstr(_L("sent by Orca in this job: %1% mm (assumes 0 at the start of the job)"), proto::format_mm(m_z_offset_mm)));

    // Spin boxes start from the machine's current targets once per job / connection.
    if (show && !m_spins_loaded) {
        for (int i = 0; i < 2; ++i)
            if (i < s.nozzles && s.nozzle_target_c[i] > 0)
                m_nozzle_spin[i]->SetValue(int(std::lround(s.nozzle_target_c[i])));
        if (s.has_bed && s.bed_target_c > 0)
            m_bed_spin->SetValue(int(std::lround(s.bed_target_c)));
        m_speed_spin->SetValue(m_work_speed);
        m_spins_loaded = true;
    }

    m_pause_btn->Enable(c.pause);
    m_resume_btn->Enable(c.resume);
    m_stop_btn->Enable(c.stop);
    for (int i = 0; i < 2; ++i) {
        m_nozzle_spin[i]->Enable(c.nozzle);
        m_nozzle_btn[i]->Enable(c.nozzle);
    }
    m_bed_spin->Enable(c.bed);
    m_bed_btn->Enable(c.bed);
    m_z_down_btn->Enable(c.z_offset);
    m_z_up_btn->Enable(c.z_offset);
    m_speed_spin->Enable(c.work_speed);
    m_speed_btn->Enable(c.work_speed);
    for (wxButton* b : { m_light_on_btn, m_light_off_btn, m_fan_on_btn, m_fan_off_btn })
        b->Enable(c.enclosure);
    m_purifier_on_btn->Enable(c.purifier);
    m_purifier_off_btn->Enable(c.purifier);

    // Optional rows: second nozzle (Dual), enclosure, purifier. Layout only when the set changes.
    const bool show_t1        = on && (s.nozzles >= 2 || m_info.head_type == 5);
    const bool show_enclosure = on && (m_info.has_enclosure || s.enclosure_module);
    const bool show_purifier  = on && s.purifier_module;
    const int  key            = (show_t1 ? 1 : 0) | (show_enclosure ? 2 : 0) | (show_purifier ? 4 : 0);
    if (key != m_layout_key) {
        m_layout_key = key;
        m_nozzle_row[1]->ShowItems(show_t1);
        m_enclosure_row->ShowItems(show_enclosure);
        m_purifier_row->ShowItems(show_purifier);
        Layout();
    }
}

void Snapmaker2MonitorPanel::stop_worker()
{
    if (!m_worker.joinable())
        return;
    m_cancel = true; // run_command() notices it within ~1 s through the curl progress callback
    m_worker.join();
}

} // namespace GUI
} // namespace Slic3r
