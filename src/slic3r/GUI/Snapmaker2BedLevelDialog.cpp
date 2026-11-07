#include "Snapmaker2BedLevelDialog.hpp"

#include <algorithm>
#include <cstdlib>

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/utils.h>

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

constexpr const char* CONFIG_SECTION  = "snapmaker2_bed_level";
constexpr long        SHORT_LINE_TIMEOUT_S = 30;  // G91 / G1 Z / G90 / G1029 S0 answer in well under a second
constexpr int         TIMER_MS        = 1000;

int config_int(const char* key, int fallback)
{
    const std::string value = wxGetApp().app_config->get(CONFIG_SECTION, key);
    if (value.empty())
        return fallback;
    char*      end = nullptr;
    const long v   = std::strtol(value.c_str(), &end, 10);
    return (end == value.c_str()) ? fallback : int(v);
}

wxString mm_ss(long seconds)
{
    return wxString::Format("%02ld:%02ld", seconds / 60, seconds % 60);
}

// The controller keeps the bed at the levelling temperature after a heat soak (it switches it off only when the soak
// itself is aborted), so every way out of the dialog after heating must say so.
wxString bed_still_hot(int bed_temp_c)
{
    return bed_temp_c > 0 ? "\n" + GUI::format_wxstr(_L("The bed may still be at %1% °C. Switch it off on the touchscreen if you are not "
                                                        "printing now."),
                                                     bed_temp_c) :
                            wxString();
}

} // namespace

Snapmaker2BedLevelDialog::Snapmaker2BedLevelDialog(wxWindow* parent, DynamicPrintConfig* config)
    : wxDialog(parent, wxID_ANY, _L("Snapmaker 2.0 bed levelling"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_host(std::make_unique<Snapmaker2>(config))
    , m_timer(this)
{
    build_ui();
    load_settings();
    update_estimate();
    set_stage(Stage::Setup);

    Bind(wxEVT_TIMER, &Snapmaker2BedLevelDialog::on_timer, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { on_close_request(); });

    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

Snapmaker2BedLevelDialog::~Snapmaker2BedLevelDialog()
{
    m_timer.Stop();
    stop_worker();
    m_host->end_interactive();
}

nlohmann::json Snapmaker2BedLevelDialog::auth_info() { return m_host->get_auth_info(); }

void Snapmaker2BedLevelDialog::build_ui()
{
    const int  gap   = FromDIP(10);
    const int  wrap  = FromDIP(460);
    auto*      top   = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY,
        _L("Probes a grid of points on the bed with the 3D printing module (single extruder), optionally after heating the bed "
           "and letting it soak, then helps you set the nozzle height with the calibration card. "
           "Needs controller firmware SM2-5.2.1-a250t.3 or newer."));
    intro->Wrap(wrap);
    top->Add(intro, 0, wxEXPAND | wxALL, gap);

    // --- setup ---
    m_setup_panel = new wxPanel(this);
    auto* grid    = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(0, 1);
    auto add_row = [&](const wxString& label, wxSpinCtrl*& ctrl, int min_v, int max_v) {
        grid->Add(new wxStaticText(m_setup_panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        ctrl = new wxSpinCtrl(m_setup_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS,
                              min_v, max_v, min_v);
        grid->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
    };
    add_row(_L("Grid size (points per side)"), m_grid, proto::LEVEL_GRID_MIN, proto::LEVEL_GRID_MAX);
    add_row(_L("Bed temperature (°C, 0 = do not heat)"), m_bed_temp, 0, proto::LEVEL_BED_MAX_C);
    add_row(_L("Soak time after the bed reaches temperature (minutes)"), m_soak_min, 0, proto::LEVEL_SOAK_MAX_MIN);
    auto* setup_sizer = new wxBoxSizer(wxVERTICAL);
    setup_sizer->Add(grid, 0, wxEXPAND);
    m_estimate = new wxStaticText(m_setup_panel, wxID_ANY, wxEmptyString);
    setup_sizer->Add(m_estimate, 0, wxEXPAND | wxTOP, gap);
    m_setup_panel->SetSizer(setup_sizer);
    top->Add(m_setup_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);

    for (wxSpinCtrl* ctrl : { m_grid, m_bed_temp, m_soak_min })
        ctrl->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { update_estimate(); });

    // --- progress / status ---
    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    top->Add(m_status, 0, wxEXPAND | wxALL, gap);

    // --- nozzle zero ---
    m_zero_panel     = new wxPanel(this);
    auto* zero_sizer = new wxBoxSizer(wxVERTICAL);
    auto* how        = new wxStaticText(m_zero_panel, wxID_ANY,
        _L("Put the calibration card on the bed under the nozzle. Lower the nozzle step by step until the card drags slightly "
           "when you slide it, then press Save. Use the large steps first and finish with 0.05 mm."));
    how->Wrap(wrap);
    zero_sizer->Add(how, 0, wxEXPAND | wxBOTTOM, gap);
    auto* jog = new wxBoxSizer(wxHORIZONTAL);
    for (double step : { 0.5, 0.1, 0.05, -0.05, -0.1, -0.5 }) {
        auto* btn = new wxButton(m_zero_panel, wxID_ANY, "Z" + wxString(step > 0 ? "+" : "") + wxString::FromUTF8(proto::format_mm(step)),
                                 wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        btn->SetToolTip(step > 0 ? _L("Raise the nozzle") : _L("Lower the nozzle"));
        btn->Bind(wxEVT_BUTTON, [this, step](wxCommandEvent&) { on_jog(step); });
        jog->Add(btn, 0, wxRIGHT, FromDIP(4));
    }
    zero_sizer->Add(jog, 0);
    m_z_label = new wxStaticText(m_zero_panel, wxID_ANY, wxEmptyString);
    zero_sizer->Add(m_z_label, 0, wxTOP, gap / 2);
    m_zero_panel->SetSizer(zero_sizer);
    top->Add(m_zero_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);

    // --- buttons ---
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    m_start_btn = new wxButton(this, wxID_ANY, _L("Start levelling"));
    m_save_btn  = new wxButton(this, wxID_ANY, _L("Save"));
    m_close_btn = new wxButton(this, wxID_ANY, _L("Cancel"));
    buttons->Add(m_start_btn, 0, wxRIGHT, FromDIP(6));
    buttons->Add(m_save_btn, 0, wxRIGHT, FromDIP(6));
    buttons->Add(m_close_btn, 0);
    top->Add(buttons, 0, wxEXPAND | wxALL, gap);

    m_start_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_start(); });
    m_save_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_save(); });
    m_close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_close_request(); });

    SetSizer(top);
}

void Snapmaker2BedLevelDialog::load_settings()
{
    const proto::LevelParams p = proto::clamp_level_params(config_int("grid", proto::LEVEL_GRID_DEFAULT), config_int("bed_temp", 0),
                                                           config_int("soak_min", 0) * 60);
    m_grid->SetValue(p.grid);
    m_bed_temp->SetValue(p.bed_temp_c);
    // A soak without heating is dropped by clamp_level_params; keep the remembered minutes in the field anyway.
    m_soak_min->SetValue(std::min(std::max(config_int("soak_min", 0), 0), proto::LEVEL_SOAK_MAX_MIN));
}

void Snapmaker2BedLevelDialog::store_settings() const
{
    AppConfig* cfg = wxGetApp().app_config;
    cfg->set(CONFIG_SECTION, "grid", std::to_string(m_grid->GetValue()));
    cfg->set(CONFIG_SECTION, "bed_temp", std::to_string(m_bed_temp->GetValue()));
    cfg->set(CONFIG_SECTION, "soak_min", std::to_string(m_soak_min->GetValue()));
}

void Snapmaker2BedLevelDialog::update_estimate()
{
    const int grid = m_grid->GetValue();
    m_soak_min->Enable(m_bed_temp->GetValue() > 0);
    wxString text = GUI::format_wxstr(_L("%1% x %1% = %2% probe points, about %3% minutes of probing."), grid, grid * grid,
                                      std::max(1, (60 + 15 * grid * grid) / 60));
    if (m_bed_temp->GetValue() > 0)
        text += "\n" + GUI::format_wxstr(_L("The bed heats to %1% °C first, soaks for %2% minutes and stays hot afterwards, "
                                            "so you can set the nozzle height at printing temperature."),
                                         m_bed_temp->GetValue(), m_soak_min->GetValue());
    if (grid > 6)
        text += "\n" + _L("Note: with more than 6 x 6 points the firmware compensates long moves on the back/right of the bed "
                          "only at their end points.");
    m_estimate->SetLabel(text);
    m_estimate->Wrap(FromDIP(460));
    Layout();
    Fit();
}

void Snapmaker2BedLevelDialog::set_stage(Stage stage)
{
    m_stage = stage;
    m_setup_panel->Show(stage == Stage::Setup);
    m_zero_panel->Show(stage == Stage::Zeroing);
    m_start_btn->Show(stage == Stage::Setup);
    m_save_btn->Show(stage == Stage::Zeroing);
    m_close_btn->SetLabel(stage == Stage::Saved ? _L("Close") : _L("Cancel"));
    if (stage == Stage::Setup)
        m_status->SetLabel(wxEmptyString);
    Layout();
    Fit();
}

void Snapmaker2BedLevelDialog::on_start()
{
    store_settings();
    m_params = proto::clamp_level_params(m_grid->GetValue(), m_bed_temp->GetValue(), m_soak_min->GetValue() * 60);
    const std::string line = proto::level_gcode(m_params);
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: bed levelling requested: grid " << m_params.grid << ", bed " << m_params.bed_temp_c
                            << " C, soak " << m_params.soak_s << " s -> '" << line << "'";

    wxString           msg;
    proto::ConnectInfo info;
    {
        wxBusyCursor busy;
        m_status->SetLabel(_L("Connecting to the touchscreen..."));
        if (!m_host->begin_interactive(msg, info)) {
            m_status->SetLabel(wxEmptyString);
            show_error(this, m_host->get_test_failed_msg(msg));
            return;
        }
    }
    // G1029 A refuses everything but the single-extruder module (MODULE_TOOLHEAD_3DP), dual extruder included.
    if (info.head_type != 1) {
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: bed levelling refused, mounted toolhead is headType " << info.head_type << " ("
                                   << proto::head_type_name(info.head_type) << ")";
        m_host->end_interactive();
        m_status->SetLabel(wxEmptyString);
        show_error(this, info.head_type == 5 ?
                             _L("Bed levelling from Snapmaker Orca needs the single-extruder 3D printing module. "
                                "With the dual extruder module, use the calibration on the touchscreen.") :
                             GUI::format_wxstr(_L("Bed levelling needs the single-extruder 3D printing module, but the machine has the %1% mounted."),
                                               proto::head_type_name(info.head_type)));
        return;
    }

    m_cancel = false;
    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_result = WorkerResult();
    }
    m_started          = std::chrono::steady_clock::now();
    m_bed_reached_seen = false;
    const long timeout = proto::level_timeout_s(m_params);
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: starting bed levelling, waiting up to " << timeout << " s for the controller";
    Snapmaker2* host = m_host.get();
    m_worker = std::thread([this, host, line, timeout]() {
        Snapmaker2::Response r = host->execute_code(line, timeout, &m_cancel);
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_result.response = std::move(r);
        m_result.done     = true;
    });

    set_stage(Stage::Levelling);
    m_status->SetLabel(_L("Starting..."));
    m_timer.Start(TIMER_MS);
}

void Snapmaker2BedLevelDialog::on_timer(wxTimerEvent&)
{
    const proto::StatusSnapshot snap    = proto::parse_status(m_host->last_status_body());
    const auto                  now     = std::chrono::steady_clock::now();
    const long                  elapsed = long(std::chrono::duration_cast<std::chrono::seconds>(now - m_started).count());

    if (m_stage == Stage::Levelling) {
        bool                 done = false;
        Snapmaker2::Response response;
        {
            std::lock_guard<std::mutex> lock(m_result_mutex);
            if (m_result.done) {
                done     = true;
                response = m_result.response;
            }
        }
        if (done) {
            if (m_worker.joinable())
                m_worker.join();
            on_levelling_finished(response);
            return;
        }

        // Progress estimate: the controller reports no phase, so derive it from the bed temperature and the time.
        wxString text;
        if (m_params.bed_temp_c > 0 && !m_bed_reached_seen && snap.has_bed && snap.bed_target_c > 0 &&
            snap.bed_c >= snap.bed_target_c - 1.0) {
            m_bed_reached_seen = true;
            m_bed_reached      = now;
            BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: bed reached " << snap.bed_c << " / " << snap.bed_target_c << " C after " << elapsed
                                    << " s, soaking " << m_params.soak_s << " s";
        }
        if (m_params.bed_temp_c > 0 && !m_bed_reached_seen) {
            text = snap.has_bed ? GUI::format_wxstr(_L("Heating the bed: %1% / %2% °C"), wxString::Format("%.1f", snap.bed_c),
                                                    wxString::Format("%.0f", snap.bed_target_c)) :
                                  _L("Heating the bed...");
        } else if (m_params.bed_temp_c > 0 &&
                   std::chrono::duration_cast<std::chrono::seconds>(now - m_bed_reached).count() < m_params.soak_s) {
            const long left = m_params.soak_s - long(std::chrono::duration_cast<std::chrono::seconds>(now - m_bed_reached).count());
            text = GUI::format_wxstr(_L("Soaking the bed at %1% °C: about %2% left"), m_params.bed_temp_c, mm_ss(left));
        } else {
            text = GUI::format_wxstr(_L("Homing and probing %1% points..."), m_params.grid * m_params.grid);
        }
        text += "\n" + GUI::format_wxstr(_L("Elapsed: %1%"), mm_ss(elapsed));
        if (snap.has_z)
            text += "   Z: " + wxString::Format("%.2f mm", snap.z);
        if (m_host->last_status_http() != 0 && m_host->last_status_http() != 200)
            text += "\n" + GUI::format_wxstr(_L("Warning: the status poll failed (HTTP %1%). Levelling continues on the machine."),
                                             m_host->last_status_http());
        text += "\n" + _L("To stop the levelling, use the emergency stop button or switch the machine off; "
                          "Snapmaker Orca cannot interrupt a running G-code command.");
        // Refit only when the number of lines changes, so the dialog does not resize on every tick.
        const bool refit = m_status->GetLabel().Freq('\n') != text.Freq('\n');
        m_status->SetLabel(text);
        m_status->Wrap(FromDIP(460));
        Layout();
        if (refit)
            Fit();
    } else if (m_stage == Stage::Zeroing) {
        if (snap.has_z)
            m_z_label->SetLabel(GUI::format_wxstr(_L("Nozzle Z: %1% mm"), wxString::Format("%.2f", snap.z)) +
                                (snap.has_bed ? "   " + GUI::format_wxstr(_L("Bed: %1% / %2% °C"), wxString::Format("%.1f", snap.bed_c),
                                                                          wxString::Format("%.0f", snap.bed_target_c)) :
                                                wxString()));
    }
}

void Snapmaker2BedLevelDialog::on_levelling_finished(const Snapmaker2::Response& r)
{
    if (r.cancelled) {
        BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: bed levelling wait cancelled";
        return;
    }
    if (r.kind != proto::Reply::Ok) {
        m_timer.Stop();
        BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: bed levelling request failed, HTTP " << r.status << " (" << proto::reply_name(r.kind)
                                 << ") after " << r.elapsed_s << " s";
        m_host->end_interactive();
        set_stage(Stage::Setup);
        wxString text = m_host->describe_failure("execute_code", r);
        if (r.kind == proto::Reply::NetworkError && r.elapsed_s > 60)
            text += "\n" + _L("The connection was lost while the machine was levelling. Check the touchscreen: the levelling may still "
                              "be running or may have finished.");
        text += bed_still_hot(m_params.bed_temp_c);
        show_error(this, text);
        return;
    }

    const proto::LevelResult result = proto::classify_level_reply(r.body, r.elapsed_s);
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: bed levelling result " << proto::level_result_name(result) << " after " << r.elapsed_s << " s";
    if (result == proto::LevelResult::Done) {
        set_stage(Stage::Zeroing);
        m_status->SetLabel(GUI::format_wxstr(_L("Levelling finished: %1% x %1% points probed in %2%."), m_params.grid,
                                             mm_ss(long(r.elapsed_s))));
        return;
    }

    m_timer.Stop();
    m_host->end_interactive();
    set_stage(Stage::Setup);
    wxString text;
    switch (result) {
    case proto::LevelResult::NoSingleHead:
        text = _L("The controller refused: bed levelling needs the single-extruder 3D printing module.");
        break;
    case proto::LevelResult::BedFault:
        text = _L("The controller refused to heat the bed because it reports a heated bed fault. Check the bed on the touchscreen.");
        break;
    case proto::LevelResult::SoakAborted:
        text = GUI::format_wxstr(_L("The heat soak was stopped (%1%). The bed was switched off and the mesh was not changed."),
                                 proto::soak_abort_reason(r.body));
        break;
    case proto::LevelResult::Cancelled: text = _L("The controller cancelled the levelling. The mesh was not changed."); break;
    case proto::LevelResult::ProbeFailed:
        text = _L("Probing failed. Check that the bed and the probe are clean and that nothing blocks the toolhead, then try again.") +
               bed_still_hot(m_params.bed_temp_c);
        break;
    case proto::LevelResult::InvalidGrid: text = _L("The controller refused the grid size."); break;
    case proto::LevelResult::TooFast:
        text = _L("The controller answered without probing. This needs controller firmware SM2-5.2.1-a250t.3 or newer; older firmware only "
                  "changes the grid size and resets the mesh in memory. Restart the machine to load the saved mesh again.");
        break;
    case proto::LevelResult::Done: break;
    }
    show_error(this, text);
}

bool Snapmaker2BedLevelDialog::run_line(const std::string& line, wxString& error)
{
    Snapmaker2::Response r = m_host->execute_code(line, SHORT_LINE_TIMEOUT_S);
    if (r.kind == proto::Reply::NotAuthorised) {
        // The touchscreen dropped the session (e.g. Wi-Fi hiccup longer than 5 s): reconnect once with the same token.
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: session lost during the nozzle-zero step, reconnecting";
        wxString           msg;
        proto::ConnectInfo info;
        if (!m_host->begin_interactive(msg, info)) {
            error = m_host->get_test_failed_msg(msg);
            return false;
        }
        r = m_host->execute_code(line, SHORT_LINE_TIMEOUT_S);
    }
    if (r.kind != proto::Reply::Ok) {
        error = m_host->describe_failure("execute_code", r);
        return false;
    }
    return true;
}

void Snapmaker2BedLevelDialog::on_jog(double step_mm)
{
    const std::vector<std::string> lines = proto::jog_z_gcodes(step_mm);
    if (lines.empty())
        return;
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: nozzle-zero jog " << proto::format_mm(step_mm) << " mm";
    wxBusyCursor busy;
    wxString     error;
    for (const std::string& line : lines) {
        if (!run_line(line, error)) {
            BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: jog line '" << line << "' failed";
            // Never leave the controller in relative mode after a failed move.
            if (line != "G90") {
                wxString ignored;
                run_line("G90", ignored);
            }
            show_error(this, error);
            return;
        }
    }
}

void Snapmaker2BedLevelDialog::on_save()
{
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: saving the nozzle height (" << proto::save_z_zero_gcode() << ")";
    wxString error;
    {
        wxBusyCursor busy;
        if (!run_line(proto::save_z_zero_gcode(), error)) {
            BOOST_LOG_TRIVIAL(error) << "Snapmaker 2.0: saving the nozzle height failed";
            show_error(this, error);
            return;
        }
    }
    m_timer.Stop();
    m_host->end_interactive();
    set_stage(Stage::Saved);
    wxString text = _L("The mesh and the nozzle height were saved on the machine.");
    if (m_params.bed_temp_c > 0)
        text += "\n" + GUI::format_wxstr(_L("The bed is still at %1% °C. Switch it off on the touchscreen if you are not printing now."),
                                         m_params.bed_temp_c);
    m_status->SetLabel(text);
    m_status->Wrap(FromDIP(460));
    Layout();
    Fit();
    BOOST_LOG_TRIVIAL(info) << "Snapmaker 2.0: bed levelling saved";
}

void Snapmaker2BedLevelDialog::on_close_request()
{
    if (m_stage == Stage::Levelling) {
        MessageDialog dlg(this,
                          _L("The machine keeps levelling: Snapmaker Orca cannot interrupt a running G-code command. "
                             "To stop it, use the emergency stop button or switch the machine off.") +
                              bed_still_hot(m_params.bed_temp_c) + "\n\n" + _L("Stop waiting and close this window?"),
                          _L("Snapmaker 2.0 bed levelling"), wxICON_WARNING | wxYES_NO);
        if (dlg.ShowModal() != wxID_YES)
            return;
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: dialog closed while the machine was levelling";
    } else if (m_stage == Stage::Zeroing) {
        MessageDialog dlg(this,
                          _L("The nozzle height has not been saved. The new mesh is used only until the machine restarts and the nozzle "
                             "height may be wrong.") +
                              bed_still_hot(m_params.bed_temp_c) + "\n\n" + _L("Close without saving?"),
                          _L("Snapmaker 2.0 bed levelling"), wxICON_WARNING | wxYES_NO);
        if (dlg.ShowModal() != wxID_YES)
            return;
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker 2.0: dialog closed without saving the nozzle height";
    }
    finish(m_stage == Stage::Saved ? wxID_OK : wxID_CANCEL);
}

void Snapmaker2BedLevelDialog::stop_worker()
{
    if (!m_worker.joinable())
        return;
    m_cancel = true; // execute_code() notices it within ~1 s through the curl progress callback
    m_worker.join();
}

void Snapmaker2BedLevelDialog::finish(int return_code)
{
    m_timer.Stop();
    stop_worker();
    m_host->end_interactive();
    EndModal(return_code);
}

} // namespace GUI
} // namespace Slic3r
