#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/webview.h>
#include <wx/string.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>


namespace Slic3r {
namespace GUI {

class Snapmaker2MonitorPanel;

class PrinterWebView : public wxPanel{
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void reload();
    void update_mode();
    bool isSnapmakerPage();
    bool is_u1_device_page();
    void sendMessage(const std::string& msg);
    wxWebView* get_browser() const { return m_browser; }
    // The Device tab was left: the Snapmaker 2.0 monitor releases the touchscreen (no-op otherwise).
    void on_page_hidden();

private:
    void SendAPIKey();
    // "Snapmaker 2.0 (HTTP)" print host: show the native monitor panel instead of the browser (the touchscreen serves
    // JSON, not a web page). Returns true when the monitor is shown; otherwise hides it (disconnecting) and returns false.
    bool show_snapmaker2_monitor(const wxString& url);
    Snapmaker2MonitorPanel* m_sm2_monitor { nullptr };

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
