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

// "Bed levelling..." for the Snapmaker 2.0 (HTTP) print host: asks grid size, bed temperature and soak time and
// offers two actions through the touchscreen's api/v1/execute_code, with progress from api/v1/status:
//  - Measure bed mesh: "G1029 A P<grid> H<bed C> K<soak s>". With a valid stored Z offset (firmware a250t.4) the
//    controller applies it and finishes; otherwise the nozzle-zero step follows (Z jog with G91 / G1 Z<step> F300 /
//    G90, save with G1029 S0), which on a250t.4 also stores the Z offset.
//  - Calibrate Z offset: "G1029 Z H<bed C> K<soak s>" (probe the bed centre), then the same nozzle-zero step.
// "G1029" (no arguments) reads the stored Z offset. Needs the single-extruder 3D printing module and controller
// firmware SM2-5.2.1-a250t.3 (mesh) / SM2-5.2.1-a250t.4 (Z offset) or newer.
// Contracts: _snapmaker2-review/features/bed-levelling-heat-soak.md, separate-mesh-and-z-offset.md.
class Snapmaker2BedLevelDialog : public wxDialog
{
public:
    Snapmaker2BedLevelDialog(wxWindow* parent, DynamicPrintConfig* config);
    ~Snapmaker2BedLevelDialog() override;

    // {"printhost_apikey": token} once the touchscreen approved a session in this dialog, otherwise {}.
    nlohmann::json auth_info();

private:
    enum class Stage { Setup, Levelling, Zeroing, Saved };
    enum class Mode { Mesh, ZOffset }; // what Levelling / Zeroing are doing: G1029 A or G1029 Z

    struct WorkerResult
    {
        bool                 done { false };
        Snapmaker2::Response response;
    };

    std::unique_ptr<Snapmaker2> m_host;
    Snapmaker2Protocol::LevelParams m_params;
    Stage                       m_stage { Stage::Setup };
    Mode                        m_mode { Mode::Mesh };
    // Mesh mode, a250t.4 said "Z offset not calibrated": the G1029 S0 of this card step also stores the Z offset.
    bool                        m_card_calibrates { false };
    // Card step: a G-code line is running on the GUI thread (jog or save); clicks meanwhile are ignored.
    bool                        m_line_busy { false };
    // Sum of the jogs since probing left the nozzle over the bed (reset when the card step starts).
    double                      m_jog_total_mm { 0. };
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
    wxStaticText* m_zoffset_label { nullptr };
    wxButton*     m_query_btn { nullptr };
    wxStaticText* m_status { nullptr };
    wxPanel*      m_zero_panel { nullptr };
    wxStaticText* m_zero_how { nullptr };
    wxStaticText* m_z_label { nullptr };
    wxButton*     m_start_btn { nullptr };
    wxButton*     m_zcal_btn { nullptr };
    wxButton*     m_save_btn { nullptr };
    wxButton*     m_close_btn { nullptr };

    void build_ui();
    void load_settings();
    void store_settings() const;
    void update_estimate();
    void set_stage(Stage stage);

    void show_zoffset(const Snapmaker2Protocol::ZOffsetReport& report);
    void set_status(const wxString& text);

    void on_start(Mode mode);
    void on_query();
    void on_timer(wxTimerEvent& event);
    void on_levelling_finished(const Snapmaker2::Response& response);
    void on_mesh_finished(const Snapmaker2::Response& response);
    void on_zoffset_probed(const Snapmaker2::Response& response);
    void back_to_setup(const wxString& error);
    void on_jog(double step_mm);
    void on_save();
    void on_close_request();

    // Runs one line on the GUI thread (short commands only); reconnects once if the touchscreen dropped the session.
    // `body` (optional) receives the controller's output for the line.
    bool run_line(const std::string& line, wxString& error, std::string* body = nullptr);
    // Disables the card-step controls while lines run and swallows the clicks queued meanwhile (REV-SM2-JOG).
    class LineLock;
    void stop_worker();
    void finish(int return_code);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Snapmaker2BedLevelDialog_hpp_
