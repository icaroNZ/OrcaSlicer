#ifndef slic3r_GUI_Snapmaker2BedLevelDialog_hpp_
#define slic3r_GUI_Snapmaker2BedLevelDialog_hpp_

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <wx/dialog.h>
#include <wx/timer.h>

#include "nlohmann/json.hpp"
#include "slic3r/Utils/Snapmaker2.hpp"

class wxButton;
class wxPanel;
class wxSpinCtrl;
class wxSpinEvent;
class wxStaticText;
class wxCloseEvent;
class wxTimerEvent;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// "Bed levelling..." for the Snapmaker 2.0 (HTTP) print host: asks grid size, bed temperature and soak time,
// runs "G1029 A P<grid> H<bed C> K<soak s>" through the touchscreen's api/v1/execute_code, shows progress from
// api/v1/status, then guides the nozzle-zero step (Z jog with G91 / G1 Z<step> F300 / G90) and saves with G1029 S0.
// Needs controller firmware SM2-5.2.1-a250t.3 or newer and the single-extruder 3D printing module.
// Contract: _snapmaker2-review/features/bed-levelling-heat-soak.md.
class Snapmaker2BedLevelDialog : public wxDialog
{
public:
    Snapmaker2BedLevelDialog(wxWindow* parent, DynamicPrintConfig* config);
    ~Snapmaker2BedLevelDialog() override;

    // {"printhost_apikey": token} once the touchscreen approved a session in this dialog, otherwise {}.
    nlohmann::json auth_info();

private:
    enum class Stage { Setup, Levelling, Zeroing, Saved };

    struct WorkerResult
    {
        bool                 done { false };
        Snapmaker2::Response response;
    };

    std::unique_ptr<Snapmaker2> m_host;
    Snapmaker2Protocol::LevelParams m_params;
    Stage                       m_stage { Stage::Setup };
    std::chrono::steady_clock::time_point m_started;
    std::chrono::steady_clock::time_point m_bed_reached;
    bool                        m_bed_reached_seen { false };

    std::thread                 m_worker;
    std::atomic<bool>           m_cancel { false };
    std::mutex                  m_result_mutex;
    WorkerResult                m_result;

    wxTimer                     m_timer;

    wxPanel*      m_setup_panel { nullptr };
    wxSpinCtrl*   m_grid { nullptr };
    wxSpinCtrl*   m_bed_temp { nullptr };
    wxSpinCtrl*   m_soak_min { nullptr };
    wxStaticText* m_estimate { nullptr };
    wxStaticText* m_status { nullptr };
    wxPanel*      m_zero_panel { nullptr };
    wxStaticText* m_z_label { nullptr };
    wxButton*     m_start_btn { nullptr };
    wxButton*     m_save_btn { nullptr };
    wxButton*     m_close_btn { nullptr };

    void build_ui();
    void load_settings();
    void store_settings() const;
    void update_estimate();
    void set_stage(Stage stage);

    void on_start();
    void on_timer(wxTimerEvent& event);
    void on_levelling_finished(const Snapmaker2::Response& response);
    void on_jog(double step_mm);
    void on_save();
    void on_close_request();

    // Runs one line on the GUI thread (short commands only); reconnects once if the touchscreen dropped the session.
    bool run_line(const std::string& line, wxString& error);
    void stop_worker();
    void finish(int return_code);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Snapmaker2BedLevelDialog_hpp_
