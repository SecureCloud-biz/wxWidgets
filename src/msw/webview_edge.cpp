/////////////////////////////////////////////////////////////////////////////
// Name:        src/msw/webview_edge.cpp
// Purpose:     wxMSW Edge Chromium wxWebView backend implementation
// Author:      Markus Pingel
// Created:     2019-12-15
// Copyright:   (c) 2019 wxWidgets development team
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#if defined(__BORLANDC__)
#pragma hdrstop
#endif

#include "wx/msw/webview_edge.h"

#if wxUSE_WEBVIEW && wxUSE_WEBVIEW_EDGE

#include "wx/filename.h"
#include "wx/module.h"
#include "wx/stdpaths.h"
#include "wx/thread.h"
#include "wx/private/jsscriptwrapper.h"
#include "wx/msw/private/webview_edge.h"

#include <wrl/event.h>
#include <Objbase.h>

using namespace Microsoft::WRL;

wxIMPLEMENT_DYNAMIC_CLASS(wxWebViewEdge, wxWebView);

#define WX_ERROR2_CASE(error, wxerror) \
        case error: \
            event.SetString(#error); \
            event.SetInt(wxerror); \
            break;

// WebView2Loader typedefs
typedef HRESULT (__stdcall *CreateWebView2EnvironmentWithDetails_t)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    PCWSTR additionalBrowserArguments,
    IWebView2CreateWebView2EnvironmentCompletedHandler* environment_created_handler);
typedef HRESULT(__stdcall *GetWebView2BrowserVersionInfo_t)(
    PCWSTR browserExecutableFolder,
    LPWSTR* versionInfo);

CreateWebView2EnvironmentWithDetails_t wxCreateWebView2EnvironmentWithDetails = NULL;
GetWebView2BrowserVersionInfo_t wxGetWebView2BrowserVersionInfo = NULL;

int wxWebViewEdgeImpl::ms_isAvailable = -1;
wxDynamicLibrary wxWebViewEdgeImpl::ms_loaderDll;

wxWebViewEdgeImpl::wxWebViewEdgeImpl(wxWebViewEdge* webview):
    m_ctrl(webview)
{

}

wxWebViewEdgeImpl::~wxWebViewEdgeImpl()
{
    if (m_webView)
    {
        m_webView->remove_NavigationCompleted(m_navigationCompletedToken);
        m_webView->remove_NavigationStarting(m_navigationStartingToken);
        m_webView->remove_NewWindowRequested(m_newWindowRequestedToken);
    }
}

bool wxWebViewEdgeImpl::Create()
{
    m_initialized = false;
    m_isBusy = false;

    m_historyLoadingFromList = false;
    m_historyEnabled = true;
    m_historyPosition = -1;

    wxString userDataPath = wxStandardPaths::Get().GetUserLocalDataDir();

    HRESULT hr = wxCreateWebView2EnvironmentWithDetails(
        nullptr,
        userDataPath.wc_str(),
        nullptr,
        Callback<IWebView2CreateWebView2EnvironmentCompletedHandler>(this,
            &wxWebViewEdgeImpl::OnEnvironmentCreated).Get());
    if (FAILED(hr))
    {
        wxLogApiError("CreateWebView2EnvironmentWithDetails", hr);
        return false;
    }
    else
        return true;
}

HRESULT wxWebViewEdgeImpl::OnEnvironmentCreated(
    HRESULT WXUNUSED(result), IWebView2Environment* environment)
{
    environment->QueryInterface(IID_PPV_ARGS(&m_webViewEnvironment));
    m_webViewEnvironment->CreateWebView(
        m_ctrl->GetHWND(),
        Callback<IWebView2CreateWebViewCompletedHandler>(
            this, &wxWebViewEdgeImpl::OnWebViewCreated).Get());
    return S_OK;
}

bool wxWebViewEdgeImpl::Initialize()
{
    if (!ms_loaderDll.Load("WebView2Loader.dll", wxDL_DEFAULT | wxDL_QUIET))
        return false;

    // Try to load functions from loader DLL
    wxDL_INIT_FUNC(wx, CreateWebView2EnvironmentWithDetails, ms_loaderDll);
    wxDL_INIT_FUNC(wx, GetWebView2BrowserVersionInfo, ms_loaderDll);
    if (!wxGetWebView2BrowserVersionInfo || !wxCreateWebView2EnvironmentWithDetails)
        return false;

    // Check if a Edge browser can be found by the loader DLL
    LPWSTR versionStr;
    HRESULT hr = wxGetWebView2BrowserVersionInfo(NULL, &versionStr);
    if (SUCCEEDED(hr))
    {
        if (versionStr)
        {
            CoTaskMemFree(versionStr);
            return true;
        }
    }
    else
        wxLogApiError("GetWebView2BrowserVersionInfo", hr);

    return false;
}

void wxWebViewEdgeImpl::Uninitalize()
{
    if (ms_isAvailable == 1)
    {
        ms_loaderDll.Unload();
        ms_isAvailable = -1;
    }
}

void wxWebViewEdgeImpl::UpdateBounds()
{
    RECT r;
    wxCopyRectToRECT(m_ctrl->GetClientRect(), r);
    if (m_webView)
        m_webView->put_Bounds(r);
}

HRESULT wxWebViewEdgeImpl::OnNavigationStarting(IWebView2WebView* WXUNUSED(sender), IWebView2NavigationStartingEventArgs* args)
{
    m_isBusy = true;
    wxString evtURL;
    LPWSTR uri;
    if (SUCCEEDED(args->get_Uri(&uri)))
    {
        evtURL = wxString(uri);
        CoTaskMemFree(uri);
    }
    wxWebViewEvent event(wxEVT_WEBVIEW_NAVIGATING, m_ctrl->GetId(), evtURL, wxString());
    event.SetEventObject(m_ctrl);
    m_ctrl->HandleWindowEvent(event);

    if (!event.IsAllowed())
        args->put_Cancel(true);

    return S_OK;
}

HRESULT wxWebViewEdgeImpl::OnNavigationCompleted(IWebView2WebView* WXUNUSED(sender), IWebView2NavigationCompletedEventArgs* args)
{
    BOOL isSuccess;
    if (FAILED(args->get_IsSuccess(&isSuccess)))
        isSuccess = false;
    m_isBusy = false;
    wxString uri = m_ctrl->GetCurrentURL();

    if (!isSuccess)
    {
        WEBVIEW2_WEB_ERROR_STATUS status;

        wxWebViewEvent event(wxEVT_WEBVIEW_ERROR, m_ctrl->GetId(), uri, wxString());
        event.SetEventObject(m_ctrl);

        if (SUCCEEDED(args->get_WebErrorStatus(&status)))
        {
            switch (status)
            {
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_UNKNOWN, wxWEBVIEW_NAV_ERR_OTHER)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT, wxWEBVIEW_NAV_ERR_CERTIFICATE)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED, wxWEBVIEW_NAV_ERR_CERTIFICATE)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS, wxWEBVIEW_NAV_ERR_CERTIFICATE)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED, wxWEBVIEW_NAV_ERR_CERTIFICATE)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID, wxWEBVIEW_NAV_ERR_CERTIFICATE)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_TIMEOUT, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED, wxWEBVIEW_NAV_ERR_CONNECTION)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED, wxWEBVIEW_NAV_ERR_USER_CANCELLED)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_REDIRECT_FAILED, wxWEBVIEW_NAV_ERR_OTHER)
                WX_ERROR2_CASE(WEBVIEW2_WEB_ERROR_STATUS_UNEXPECTED_ERROR, wxWEBVIEW_NAV_ERR_OTHER)
            }
        }
        m_ctrl->HandleWindowEvent(event);
    }
    else
    {
        wxWebViewEvent evt(wxEVT_WEBVIEW_NAVIGATED, m_ctrl->GetId(), uri, wxString());
        m_ctrl->HandleWindowEvent(evt);
        if (m_historyEnabled && !m_historyLoadingFromList &&
            (uri == m_ctrl->GetCurrentURL()) ||
            (m_ctrl->GetCurrentURL().substr(0, 4) == "file" &&
                wxFileName::URLToFileName(m_ctrl->GetCurrentURL()).GetFullPath() == uri))
        {
            // If we are not at the end of the list, then erase everything
            // between us and the end before adding the new page
            if (m_historyPosition != static_cast<int>(m_historyList.size()) - 1)
            {
                m_historyList.erase(m_historyList.begin() + m_historyPosition + 1,
                    m_historyList.end());
            }
            wxSharedPtr<wxWebViewHistoryItem> item(new wxWebViewHistoryItem(uri, m_ctrl->GetCurrentTitle()));
            m_historyList.push_back(item);
            m_historyPosition++;
        }
        //Reset as we are done now
        m_historyLoadingFromList = false;
    }
    return S_OK;
}

HRESULT wxWebViewEdgeImpl::OnNewWindowRequested(IWebView2WebView* WXUNUSED(sender), IWebView2NewWindowRequestedEventArgs* args)
{
    LPWSTR uri;
    wxString evtURL;
    if (SUCCEEDED(args->get_Uri(&uri)))
    {
        evtURL = wxString(uri);
        CoTaskMemFree(uri);
    }
    wxWebViewEvent evt(wxEVT_WEBVIEW_NEWWINDOW, m_ctrl->GetId(), evtURL, wxString());
    m_ctrl->HandleWindowEvent(evt);
    args->put_Handled(true);
    return S_OK;
}

HRESULT wxWebViewEdgeImpl::OnWebViewCreated(HRESULT result, IWebView2WebView* webview)
{
    if (FAILED(result))
    {
        wxLogApiError("WebView2::WebViewCreated", result);
        return result;
    }

    webview->QueryInterface(IID_PPV_ARGS(&m_webView));

    m_initialized = true;
    UpdateBounds();

    // Connect and handle the various WebView events
    m_webView->add_NavigationStarting(
        Callback<IWebView2NavigationStartingEventHandler>(
            this, &wxWebViewEdgeImpl::OnNavigationStarting).Get(),
        &m_navigationStartingToken);
    m_webView->add_NavigationCompleted(
        Callback<IWebView2NavigationCompletedEventHandler>(
            this, &wxWebViewEdgeImpl::OnNavigationCompleted).Get(),
        &m_navigationCompletedToken);
    m_webView->add_NewWindowRequested(
        Callback<IWebView2NewWindowRequestedEventHandler>(
            this, &wxWebViewEdgeImpl::OnNewWindowRequested).Get(),
        &m_newWindowRequestedToken);

    if (!m_pendingURL.empty())
    {
        m_ctrl->LoadURL(m_pendingURL);
        m_pendingURL.clear();
    }

    return S_OK;
}

IWebView2Settings2* wxWebViewEdgeImpl::GetSettings()
{
    IWebView2Settings* settings;
    HRESULT hr = m_webView->get_Settings(&settings);
    if (FAILED(hr))
    {
        wxLogApiError("WebView2::get_Settings", hr);
        return NULL;
    }

    IWebView2Settings2* settings2;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings2));
    if (FAILED(hr))
        return NULL;

    return settings2;
}

bool wxWebViewEdge::IsAvailable()
{
    if (wxWebViewEdgeImpl::ms_isAvailable == -1)
    {
        if (!wxWebViewEdgeImpl::Initialize())
            wxWebViewEdgeImpl::ms_isAvailable = 0;
        else
            wxWebViewEdgeImpl::ms_isAvailable = 1;
    }

    return wxWebViewEdgeImpl::ms_isAvailable == 1;
}

wxWebViewEdge::~wxWebViewEdge()
{
    delete m_impl;
}

bool wxWebViewEdge::Create(wxWindow* parent,
    wxWindowID id,
    const wxString& url,
    const wxPoint& pos,
    const wxSize& size,
    long style,
    const wxString& name)
{
    if (!IsAvailable())
        return false;

    if (!wxControl::Create(parent, id, pos, size, style,
        wxDefaultValidator, name))
    {
        return false;
    }

    m_impl = new wxWebViewEdgeImpl(this);
    if (!m_impl->Create())
        return false;
    Bind(wxEVT_SIZE, &wxWebViewEdge::OnSize, this);

    LoadURL(url);
    return true;
}

void wxWebViewEdge::OnSize(wxSizeEvent& event)
{
    m_impl->UpdateBounds();
    event.Skip();
}

void wxWebViewEdge::LoadURL(const wxString& url)
{
    if (!m_impl->m_webView)
    {
        m_impl->m_pendingURL = url;
        return;
    }
    HRESULT hr = m_impl->m_webView->Navigate(url.wc_str());
    if (FAILED(hr))
        wxLogApiError("WebView2::Navigate", hr);
}

void wxWebViewEdge::LoadHistoryItem(wxSharedPtr<wxWebViewHistoryItem> item)
{
    int pos = -1;
    for (unsigned int i = 0; i < m_impl->m_historyList.size(); i++)
    {
        //We compare the actual pointers to find the correct item
        if (m_impl->m_historyList[i].get() == item.get())
            pos = i;
    }
    wxASSERT_MSG(pos != static_cast<int>(m_impl->m_historyList.size()), "invalid history item");
    m_impl->m_historyLoadingFromList = true;
    LoadURL(item->GetUrl());
    m_impl->m_historyPosition = pos;
}

wxVector<wxSharedPtr<wxWebViewHistoryItem> > wxWebViewEdge::GetBackwardHistory()
{
    wxVector<wxSharedPtr<wxWebViewHistoryItem> > backhist;
    //As we don't have std::copy or an iterator constructor in the wxwidgets
    //native vector we construct it by hand
    for (int i = 0; i < m_impl->m_historyPosition; i++)
    {
        backhist.push_back(m_impl->m_historyList[i]);
    }
    return backhist;
}

wxVector<wxSharedPtr<wxWebViewHistoryItem> > wxWebViewEdge::GetForwardHistory()
{
    wxVector<wxSharedPtr<wxWebViewHistoryItem> > forwardhist;
    //As we don't have std::copy or an iterator constructor in the wxwidgets
    //native vector we construct it by hand
    for (int i = m_impl->m_historyPosition + 1; i < static_cast<int>(m_impl->m_historyList.size()); i++)
    {
        forwardhist.push_back(m_impl->m_historyList[i]);
    }
    return forwardhist;
}

bool wxWebViewEdge::CanGoForward() const
{
    if (m_impl->m_historyEnabled)
        return m_impl->m_historyPosition != static_cast<int>(m_impl->m_historyList.size()) - 1;
    else
        return false;
}

bool wxWebViewEdge::CanGoBack() const
{
    if (m_impl->m_historyEnabled)
        return m_impl->m_historyPosition > 0;
    else
        return false;
}

void wxWebViewEdge::GoBack()
{
    LoadHistoryItem(m_impl->m_historyList[m_impl->m_historyPosition - 1]);
}

void wxWebViewEdge::GoForward()
{
    LoadHistoryItem(m_impl->m_historyList[m_impl->m_historyPosition + 1]);
}

void wxWebViewEdge::ClearHistory()
{
    m_impl->m_historyList.clear();
    m_impl->m_historyPosition = -1;
}

void wxWebViewEdge::EnableHistory(bool enable)
{
    m_impl->m_historyEnabled = enable;
    m_impl->m_historyList.clear();
    m_impl->m_historyPosition = -1;
}

void wxWebViewEdge::Stop()
{
    if (m_impl->m_webView)
        m_impl->m_webView->Stop();
}

void wxWebViewEdge::Reload(wxWebViewReloadFlags WXUNUSED(flags))
{
    if (m_impl->m_webView)
        m_impl->m_webView->Reload();
}

wxString wxWebViewEdge::GetPageSource() const
{
    return wxString();
}

wxString wxWebViewEdge::GetPageText() const
{
    return wxString();
}

bool wxWebViewEdge::IsBusy() const
{
    return m_impl->m_isBusy;
}

wxString wxWebViewEdge::GetCurrentURL() const
{
    LPWSTR uri;
    if (m_impl->m_webView && SUCCEEDED(m_impl->m_webView->get_Source(&uri)))
    {
        wxString uriStr(uri);
        CoTaskMemFree(uri);
        return uriStr;
    }
    else
        return wxString();
}

wxString wxWebViewEdge::GetCurrentTitle() const
{
    LPWSTR title;
    if (m_impl->m_webView && SUCCEEDED(m_impl->m_webView->get_DocumentTitle(&title)))
    {
        wxString titleStr(title);
        CoTaskMemFree(title);
        return titleStr;
    }
    else
        return wxString();
}

void wxWebViewEdge::SetZoomType(wxWebViewZoomType)
{

}

wxWebViewZoomType wxWebViewEdge::GetZoomType() const
{
    return wxWEBVIEW_ZOOM_TYPE_LAYOUT;
}

bool wxWebViewEdge::CanSetZoomType(wxWebViewZoomType type) const
{
    return (type == wxWEBVIEW_ZOOM_TYPE_LAYOUT);
}

void wxWebViewEdge::Print()
{
    RunScript("window.print();");
}

wxWebViewZoom wxWebViewEdge::GetZoom() const
{
    double old_zoom_factor = 0.0f;
    m_impl->m_webView->get_ZoomFactor(&old_zoom_factor);
    if (old_zoom_factor > 1.7f)
        return wxWEBVIEW_ZOOM_LARGEST;
    if (old_zoom_factor > 1.3f)
        return wxWEBVIEW_ZOOM_LARGE;
    if (old_zoom_factor > 0.8f)
        return wxWEBVIEW_ZOOM_MEDIUM;
    if (old_zoom_factor > 0.6f)
        return wxWEBVIEW_ZOOM_SMALL;
    return wxWEBVIEW_ZOOM_TINY;
}

void wxWebViewEdge::SetZoom(wxWebViewZoom zoom)
{
    double old_zoom_factor = 0.0f;
    m_impl->m_webView->get_ZoomFactor(&old_zoom_factor);
    double zoom_factor = 1.0f;
    switch (zoom)
    {
    case wxWEBVIEW_ZOOM_LARGEST:
        zoom_factor = 2.0f;
        break;
    case wxWEBVIEW_ZOOM_LARGE:
        zoom_factor = 1.5f;
        break;
    case wxWEBVIEW_ZOOM_MEDIUM:
        zoom_factor = 1.0f;
        break;
    case wxWEBVIEW_ZOOM_SMALL:
        zoom_factor = 0.75f;
        break;
    case wxWEBVIEW_ZOOM_TINY:
        zoom_factor = 0.5f;
        break;
    default:
        break;
    }
    m_impl->m_webView->put_ZoomFactor(zoom_factor);
}

bool wxWebViewEdge::CanCut() const
{
    return false;
}

bool wxWebViewEdge::CanCopy() const
{
    return false;
}

bool wxWebViewEdge::CanPaste() const
{
    return false;
}

void wxWebViewEdge::Cut()
{

}

void wxWebViewEdge::Copy()
{

}

void wxWebViewEdge::Paste()
{

}

bool wxWebViewEdge::CanUndo() const
{
    return false;
}

bool wxWebViewEdge::CanRedo() const
{
    return false;
}

void wxWebViewEdge::Undo()
{

}

void wxWebViewEdge::Redo()
{

}

long wxWebViewEdge::Find(const wxString& WXUNUSED(text), int WXUNUSED(flags))
{
    return -1;
}

//Editing functions
void wxWebViewEdge::SetEditable(bool WXUNUSED(enable))
{

}

bool wxWebViewEdge::IsEditable() const
{
    return false;
}

void wxWebViewEdge::SelectAll()
{

}

bool wxWebViewEdge::HasSelection() const
{
    return false;
}

void wxWebViewEdge::DeleteSelection()
{

}

wxString wxWebViewEdge::GetSelectedText() const
{
    return wxString();
}

wxString wxWebViewEdge::GetSelectedSource() const
{
    return wxString();
}

void wxWebViewEdge::ClearSelection()
{

}

void wxWebViewEdge::EnableContextMenu(bool enable)
{
    wxCOMPtr<IWebView2Settings2> settings(m_impl->GetSettings());
    if (settings)
        settings->put_AreDefaultContextMenusEnabled(enable);
}

bool wxWebViewEdge::IsContextMenuEnabled() const
{
    wxCOMPtr<IWebView2Settings2> settings(m_impl->GetSettings());
    if (settings)
    {
        BOOL menusEnabled = TRUE;
        settings->get_AreDefaultContextMenusEnabled(&menusEnabled);

        if (!menusEnabled)
            return false;
    }
    return true;
}

void wxWebViewEdge::EnableDevTools(bool enable)
{
    wxCOMPtr<IWebView2Settings2> settings(m_impl->GetSettings());
    if (settings)
        settings->put_AreDevToolsEnabled(enable);
}

bool wxWebViewEdge::IsAccessToDevToolsEnabled() const
{
    wxCOMPtr<IWebView2Settings2> settings(m_impl->GetSettings());
    if (settings)
    {
        BOOL devToolsEnabled = TRUE;
        settings->get_AreDevToolsEnabled(&devToolsEnabled);

        if (!devToolsEnabled)
            return false;
    }

    return true;
}

void* wxWebViewEdge::GetNativeBackend() const
{
    return m_impl->m_webView;
}

bool wxWebViewEdge::RunScriptSync(const wxString& javascript, wxString* output)
{
    bool scriptExecuted = false;

    HRESULT hr = m_impl->m_webView->ExecuteScript(javascript.wc_str(), Callback<IWebView2ExecuteScriptCompletedHandler>(
        [&scriptExecuted, output](HRESULT error, PCWSTR result) -> HRESULT
    {
        if (error == S_OK)
        {
            if (output)
                output->assign(result);
        }
        else
            wxLogError(_("RunScript failed: %.8x"), error);

        scriptExecuted = true;

        return S_OK;
    }).Get());


    while (!scriptExecuted)
        wxYield();

    if (FAILED(hr))
    {
        wxLogApiError("ExecuteScript", hr);
        return false;
    }
    else
        return true;
}

bool wxWebViewEdge::RunScript(const wxString& javascript, wxString* output)
{
    wxJSScriptWrapper wrapJS(javascript, &m_runScriptCount);

    // This string is also used as an error indicator: it's cleared if there is
    // no error or used in the warning message below if there is one.
    wxString result;
    if (RunScriptSync(wrapJS.GetWrappedCode(), &result)
        && result == wxS("true"))
    {
        if (RunScriptSync(wrapJS.GetOutputCode(), &result))
        {
            if (output)
                *output = result;
            result.clear();
        }

        RunScriptSync(wrapJS.GetCleanUpCode());
    }

    if (!result.empty())
    {
        wxLogWarning(_("Error running JavaScript: %s"), result);
        return false;
    }

    return true;
}

void wxWebViewEdge::RegisterHandler(wxSharedPtr<wxWebViewHandler> handler)
{

}

void wxWebViewEdge::DoSetPage(const wxString& html, const wxString& WXUNUSED(baseUrl))
{
    if (m_impl->m_webView)
        m_impl->m_webView->NavigateToString(html.wc_str());
}

// ----------------------------------------------------------------------------
// Module ensuring all global/singleton objects are destroyed on shutdown.
// ----------------------------------------------------------------------------

class wxWebViewEdgeModule : public wxModule
{
public:
    wxWebViewEdgeModule()
    {
    }

    virtual bool OnInit() wxOVERRIDE
    {
        return true;
    }

    virtual void OnExit() wxOVERRIDE
    {
        wxWebViewEdgeImpl::Uninitalize();
    }

private:
    wxDECLARE_DYNAMIC_CLASS(wxWebViewEdgeModule);
};

wxIMPLEMENT_DYNAMIC_CLASS(wxWebViewEdgeModule, wxModule);

#endif // wxUSE_WEBVIEW && wxUSE_WEBVIEW_EDGE
