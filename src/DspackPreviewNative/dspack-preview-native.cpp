// dspack-preview-native.cpp
// Native COM preview handler for .dspack files (Windows Preview Handler).
// Implements IPreviewHandler + IInitializeWithFile; parses the .dspack and renders
// a summary card with GDI. Logs to %USERPROFILE%\AppData\LocalLow\dspack-preview-native.log.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <new>
#include <wchar.h>
#include <cstring>
#include <string>
#include <vector>
#include "dspack-read.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// {7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d}
const CLSID CLSID_DspackPreviewHandler = { 0x7f3c5a1e, 0x2b4d, 0x4e6a, { 0x9c,0x8b,0x1d,0x5f,0x7a,0x3e,0x9c,0x2d } };

namespace
{
const wchar_t kProgId[] = L"DspackPreview.PreviewHandler";
const wchar_t kPreviewCat[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
const wchar_t kHostClass[] = L"DspackPreview.Host";

HINSTANCE g_hinst = 0;
volatile LONG g_cLocks = 0;
volatile LONG g_cObjects = 0;

void Log(const char* msg)
{
    wchar_t p[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wcscat_s(p, L"\\AppData\\LocalLow\\dspack-preview-native.log");
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(h, msg, (DWORD)strlen(msg), &w, 0);
        WriteFile(h, "\r\n", 2, &w, 0);
        CloseHandle(h);
    }
}

void ClsidStr(wchar_t* out)
{
    LPOLESTR s = 0;
    if (SUCCEEDED(StringFromCLSID(CLSID_DspackPreviewHandler, &s)))
    {
        wcscpy(out, s);
        CoTaskMemFree(s);
    }
}

// ---------- preview data ----------
struct PreviewData
{
    bool valid = false;
    uint32_t headerVersion = 0;
    ManifestInfo info;
    std::wstring error;
};

void LoadAndParse(const wchar_t* path, PreviewData& d)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { d.valid = false; d.error = L"无法打开文件"; return; }
    DWORD size = GetFileSize(h, 0);
    if (size < 8 || size > (64u << 20)) { CloseHandle(h); d.valid = false; d.error = L"文件大小异常"; return; }
    std::vector<BYTE> buf(size);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf.data(), size, &rd, 0) != 0;
    CloseHandle(h);
    if (!ok || rd != size) { d.valid = false; d.error = L"读取文件失败"; return; }
    if (memcmp(buf.data(), "DSPK", 4) != 0) { d.valid = false; d.error = L"不是有效的 .dspack 文件"; return; }
    d.headerVersion = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);

    std::vector<BYTE> entry;
    if (!ReadZipEntry(buf.data() + 8, rd - 8, "manifest.json", entry))
    {
        d.valid = false;
        d.error = L"压缩包中缺少 manifest.json";
        return;
    }
    if (!ParseManifest(entry.data(), entry.size(), d.info))
    {
        d.valid = false;
        d.error = L"manifest.json 解析失败";
        return;
    }
    d.valid = true;
}

// ---------- GDI rendering ----------
struct Palette
{
    COLORREF bg = RGB(243, 244, 248);
    COLORREF accent = RGB(63, 68, 148);
    COLORREF card = RGB(255, 255, 255);
    COLORREF text = RGB(33, 36, 48);
    COLORREF sub = RGB(106, 111, 130);
    COLORREF chip = RGB(235, 236, 245);
    COLORREF chipNum = RGB(53, 58, 138);
};

HFONT MakeFont(HDC hdc, int pt, int weight)
{
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void CardText(HDC hdc, RECT* rc, const std::wstring& s, HFONT f, COLORREF c, UINT fmt = 0)
{
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    ::DrawTextW(hdc, s.c_str(), (int)s.size(), rc, fmt | DT_NOPREFIX);
    SelectObject(hdc, old);
}

void RenderPreview(HDC hdc, const RECT& rc, const PreviewData& d)
{
    const Palette P;
    int W = rc.right - rc.left;
    int bottom = rc.bottom;

    // background
    HBRUSH bg = CreateSolidBrush(P.bg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    HFONT fTitle = MakeFont(hdc, 20, FW_SEMIBOLD);
    HFONT fHeadSub = MakeFont(hdc, 11, FW_NORMAL);
    HFONT fNum = MakeFont(hdc, 16, FW_BOLD);
    HFONT fChipLbl = MakeFont(hdc, 10, FW_NORMAL);
    HFONT fBody = MakeFont(hdc, 11, FW_NORMAL);
    HFONT fLbl = MakeFont(hdc, 10, FW_SEMIBOLD);

    const int pad = 18;

    if (!d.valid)
    {
        // error card
        RECT hd = rc; hd.bottom = rc.top + 88;
        HBRUSH acc = CreateSolidBrush(RGB(172, 60, 60));
        FillRect(hdc, &hd, acc); DeleteObject(acc);
        RECT htr = { hd.left + pad, hd.top + 16, hd.right - pad, hd.top + 46 };
        CardText(hdc, &htr, L"无法预览", fTitle, RGB(255,255,255));
        RECT hsr = { hd.left + pad, hd.top + 48, hd.right - pad, hd.bottom - 8 };
        CardText(hdc, &hsr, d.error, fHeadSub, RGB(255, 235, 235));
        RECT br = { rc.left + pad, hd.bottom + 16, rc.right - pad, bottom - pad };
        std::wstring hint = L"该文件可能不是有效的 DSHPack 整合包。\n要求:8 字节 \"DSPK\" 头 + 标准 ZIP,根目录含 manifest.json。";
        CardText(hdc, &br, hint, fBody, P.sub, DT_WORDBREAK);
        goto done;
    }

    {
        const ManifestInfo& m = d.info;
        // header band
        RECT hd = rc; hd.bottom = rc.top + 92;
        HBRUSH acc = CreateSolidBrush(P.accent);
        FillRect(hdc, &hd, acc); DeleteObject(acc);

        std::wstring title = m.displayName.empty() ? m.name : m.displayName;
        RECT tr = { hd.left + pad, hd.top + 14, hd.right - pad, hd.top + 48 };
        CardText(hdc, &tr, title, fTitle, RGB(255, 255, 255), DT_END_ELLIPSIS | DT_SINGLELINE);

        // subtitle: version · type
        std::wstring sub = L"v" + m.version;
        std::wstring type = m.type.empty() ? L"profile" : m.type;
        sub += L"  ·  " + type;
        if (!m.dshVersion.empty()) sub += L"  ·  DSH " + m.dshVersion;
        RECT sr = { hd.left + pad, hd.top + 50, hd.right - pad, hd.top + 74 };
        CardText(hdc, &sr, sub, fHeadSub, RGB(226, 229, 246), DT_END_ELLIPSIS | DT_SINGLELINE);

        // stats chips
        int cy = hd.bottom + 16;
        int chipH = 52;
        int chipW = (W - pad * 2 - 12 * 2) / 3;
        struct Chip { std::wstring num; std::wstring lbl; };
        Chip chips[3] = {
            { std::to_wstring(m.bundles), L"整合包" },
            { std::to_wstring(m.dependencies), L"依赖" },
            { std::to_wstring(m.files), L"文件" }
        };
        for (int i = 0; i < 3; ++i)
        {
            RECT cr = { rc.left + pad + i * (chipW + 12), cy, rc.left + pad + i * (chipW + 12) + chipW, cy + chipH };
            HBRUSH cb = CreateSolidBrush(P.chip);
            FillRect(hdc, &cr, cb); DeleteObject(cb);
            RECT nr = { cr.left + 8, cr.top + 6, cr.right - 8, cr.top + 26 };
            CardText(hdc, &nr, chips[i].num, fNum, P.chipNum);
            RECT lr = { cr.left + 8, cr.top + 28, cr.right - 8, cr.bottom - 6 };
            CardText(hdc, &lr, chips[i].lbl, fChipLbl, P.sub);
        }

        // description block
        int dy = cy + chipH + 14;
        if (!m.description.empty())
        {
            RECT dr = { rc.left + pad, dy, rc.right - pad, dy + 90 };
            CardText(hdc, &dr, m.description, fBody, P.sub, DT_WORDBREAK | DT_END_ELLIPSIS);
            // measure wrapped height for the next section
            RECT mr = dr;
            CardText(hdc, &mr, m.description, fBody, P.sub, DT_WORDBREAK | DT_CALCRECT);
            dy = mr.bottom + 14;
        }

        // metadata lines
        RECT lr = { rc.left + pad, dy, rc.left + pad + 90, bottom - pad };
        RECT vr = { rc.left + pad + 92, dy, rc.right - pad, bottom - pad };
        std::wstring labels = L"名称:\n作者:\nDSH 版本:";
        std::wstring values = m.name + L"\n" + (m.author.empty() ? L"——" : m.author) + L"\n" + (m.dshVersion.empty() ? L"——" : m.dshVersion);
        CardText(hdc, &lr, labels, fLbl, P.text);
        CardText(hdc, &vr, values, fBody, P.sub, DT_END_ELLIPSIS);
    }

done:
    DeleteObject(fTitle);
    DeleteObject(fHeadSub);
    DeleteObject(fNum);
    DeleteObject(fChipLbl);
    DeleteObject(fBody);
    DeleteObject(fLbl);
}

// ---------- host child window ----------
bool EnsureHostClass()
{
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM w, LPARAM l) -> LRESULT
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d) RenderPreview(hdc, ps.rcPaint, *d);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINT:
        case WM_PRINTCLIENT:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d) { RECT cr; GetClientRect(hwnd, &cr); RenderPreview((HDC)w, cr, *d); }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); delete d; }
            break;
        }
        }
        return DefWindowProcW(hwnd, msg, w, l);
    };
    wc.lpszClassName = kHostClass;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}
} // namespace

class CDspackPreviewHandler : public IPreviewHandler, public IInitializeWithFile
{
    volatile LONG m_ref;
    wchar_t* m_file;
    HWND m_hwnd;
    HWND m_host;
    RECT m_rect;
public:
    CDspackPreviewHandler() : m_ref(1), m_file(0), m_hwnd(0), m_host(0)
    {
        ZeroMemory(&m_rect, sizeof(m_rect));
        InterlockedIncrement(&g_cObjects);
        Log("ctor");
    }
    ~CDspackPreviewHandler()
    {
        Log("dtor");
        InterlockedDecrement(&g_cObjects);
        if (m_file) CoTaskMemFree(m_file);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = 0;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IPreviewHandler))
            *ppv = static_cast<IPreviewHandler*>(this);
        else if (IsEqualGUID(riid, IID_IInitializeWithFile))
            *ppv = static_cast<IInitializeWithFile*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // IInitializeWithFile
    STDMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode)
    {
        Log("Initialize(file)");
        if (m_file) CoTaskMemFree(m_file);
        m_file = (wchar_t*)CoTaskMemAlloc((wcslen(pszFilePath) + 1) * sizeof(wchar_t));
        if (!m_file) return E_OUTOFMEMORY;
        wcscpy(m_file, pszFilePath);
        return S_OK;
    }

    // IPreviewHandler
    STDMETHODIMP SetWindow(HWND hwnd, const RECT* prc)
    {
        Log("SetWindow");
        m_hwnd = hwnd;
        if (prc) m_rect = *prc;
        return S_OK;
    }
    STDMETHODIMP SetRect(const RECT* prc)
    {
        Log("SetRect");
        if (prc) m_rect = *prc;
        if (m_host && m_hwnd)
            MoveWindow(m_host, m_rect.left, m_rect.top, m_rect.right - m_rect.left, m_rect.bottom - m_rect.top, TRUE);
        return S_OK;
    }
    STDMETHODIMP DoPreview()
    {
        Log("DoPreview");
        if (m_host || !m_hwnd) return S_OK;

        PreviewData* d = new (std::nothrow) PreviewData();
        if (!d) return E_OUTOFMEMORY;
        if (m_file) LoadAndParse(m_file, *d);
        else { d->valid = false; d->error = L"没有文件路径"; }

        if (!EnsureHostClass()) { delete d; return E_FAIL; }

        m_host = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            m_rect.left, m_rect.top,
            (m_rect.right - m_rect.left > 0) ? m_rect.right - m_rect.left : 400,
            (m_rect.bottom - m_rect.top > 0) ? m_rect.bottom - m_rect.top : 300,
            m_hwnd, NULL, g_hinst, NULL);
        if (m_host)
        {
            SetWindowLongPtrW(m_host, GWLP_USERDATA, (LONG_PTR)d);
            InvalidateRect(m_host, NULL, TRUE);
        }
        else
        {
            delete d;
        }
        return S_OK;
    }
    STDMETHODIMP Unload()
    {
        Log("Unload");
        if (m_host)
        {
            DestroyWindow(m_host); // WM_NCDESTROY deletes PreviewData
            m_host = 0;
        }
        m_hwnd = 0;
        return S_OK;
    }
    STDMETHODIMP SetFocus() { Log("SetFocus"); return S_OK; }
    STDMETHODIMP QueryFocus(HWND* phwnd)
    {
        Log("QueryFocus");
        if (phwnd) *phwnd = m_host;
        return S_OK;
    }
    STDMETHODIMP TranslateAccelerator(MSG* pmsg) { return S_FALSE; }
};

class CDspackClassFactory : public IClassFactory
{
    volatile LONG m_ref;
public:
    CDspackClassFactory() : m_ref(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = 0;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IClassFactory))
            *ppv = static_cast<IClassFactory*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv)
    {
        if (ppv) *ppv = 0;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        CDspackPreviewHandler* p = new (std::nothrow) CDspackPreviewHandler();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock)
    {
        if (fLock) InterlockedIncrement(&g_cLocks); else InterlockedDecrement(&g_cLocks);
        return S_OK;
    }
};

static HRESULT SetRegValue(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* val)
{
    HKEY k = 0;
    LONG lr = RegCreateKeyExW(root, sub, 0, 0, 0, KEY_WRITE, 0, &k, 0);
    if (lr != ERROR_SUCCESS) return HRESULT_FROM_WIN32(lr);
    lr = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return HRESULT_FROM_WIN32(lr);
}

extern "C" HRESULT __stdcall DllRegisterServer()
{
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(g_hinst, dll, MAX_PATH);
    wchar_t clsid[64]; ClsidStr(clsid);
    wchar_t sub[512];

    swprintf_s(sub, L"CLSID\\%s", clsid);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, kProgId);
    swprintf_s(sub, L"CLSID\\%s\\InprocServer32", clsid);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, dll);
    SetRegValue(HKEY_CLASSES_ROOT, sub, L"ThreadingModel", L"Both");
    swprintf_s(sub, L".dspack\\shellex\\%s", kPreviewCat);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, clsid);
    swprintf_s(sub, L"%s\\CLSID", kProgId);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, clsid);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer()
{
    wchar_t clsid[64]; ClsidStr(clsid);
    wchar_t sub[512];
    swprintf_s(sub, L"CLSID\\%s\\InprocServer32", clsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    swprintf_s(sub, L"CLSID\\%s", clsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    swprintf_s(sub, L".dspack\\shellex\\%s", kPreviewCat);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    swprintf_s(sub, L"%s\\CLSID", kProgId);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, kProgId);
    return S_OK;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!IsEqualGUID(rclsid, CLSID_DspackPreviewHandler)) return CLASS_E_CLASSNOTAVAILABLE;
    CDspackClassFactory* f = new (std::nothrow) CDspackClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
    return (g_cLocks == 0 && g_cObjects == 0) ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hinst = h;
        DisableThreadLibraryCalls(h);
        Log("DllMain attach (dspack-preview-native)");
    }
    return TRUE;
}