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
#include <cstdarg>
#include <cstdio>
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

void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    Log(buf);
}

void GuidStr(REFGUID g, char* out, size_t cap)
{
    wchar_t w[64] = { 0 };
    StringFromGUID2(g, w, 64);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, 0, 0);
}

void LogProcess()
{
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, buf, sizeof(buf), 0, 0);
    LogF("DllMain attach pid=%lu exe=%s", GetCurrentProcessId(), buf);
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

void LoadAndParseBuf(const BYTE* data, size_t size, PreviewData& d)
{
    // .dspack = 标准 ZIP（根目录含 manifest.json + dspack.json），无前导魔数字节。
    // 仅为兼容早期的 8 字节 "DSPK" 头做一次剥头（新文件不会再带）。
    const BYTE* zip = data;
    size_t zipSize = size;
    if (size >= 8 && memcmp(data, "DSPK", 4) == 0)
    {
        d.headerVersion = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
        zip = data + 8;
        zipSize = size - 8;
    }

    std::vector<BYTE> entry;
    if (!ReadZipEntry(zip, zipSize, "manifest.json", entry))
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
    LoadAndParseBuf(buf.data(), rd, d);
}

bool ReadStreamToVec(IStream* s, std::vector<BYTE>& out)
{
    if (!s) return false;
    LARGE_INTEGER li = { 0 };
    s->Seek(li, STREAM_SEEK_SET, NULL);
    STATSTG st; ZeroMemory(&st, sizeof(st));
    if (FAILED(s->Stat(&st, STATFLAG_NONAME))) return false;
    ULONGLONG sz = st.cbSize.QuadPart;
    if (sz > (64ull << 20)) sz = (64ull << 20); // cap at 64MB
    out.resize((size_t)sz);
    ULONG got = 0;
    if (FAILED(s->Read(out.data(), (ULONG)out.size(), &got))) { out.clear(); return false; }
    out.resize(got);
    return !out.empty();
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

static int LineHeight(HDC hdc, HFONT f)
{
    TEXTMETRICW tm;
    HFONT old = (HFONT)SelectObject(hdc, f);
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    return tm.tmHeight;
}

static int MeasureHeight(HDC hdc, const std::wstring& s, HFONT f, int widthPx, UINT extra)
{
    RECT r = { 0, 0, widthPx, 0 };
    HFONT old = (HFONT)SelectObject(hdc, f);
    DrawTextW(hdc, s.c_str(), (int)s.size(), &r, extra | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(hdc, old);
    return r.bottom;
}

void RenderPreview(HDC hdc, const RECT& rc, const PreviewData& d)
{
    const Palette P;
    int W = rc.right - rc.left;
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    double sc = (double)dpi / 96.0;
    int pad = (int)(18 * sc + 0.5);
    int gap = (int)(10 * sc + 0.5);

    // background
    HBRUSH bg = CreateSolidBrush(P.bg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    HFONT fTitle = MakeFont(hdc, 20, FW_SEMIBOLD);
    HFONT fSub = MakeFont(hdc, 11, FW_NORMAL);
    HFONT fNum = MakeFont(hdc, 15, FW_BOLD);
    HFONT fChip = MakeFont(hdc, 10, FW_NORMAL);
    HFONT fBody = MakeFont(hdc, 11, FW_NORMAL);
    HFONT fLbl = MakeFont(hdc, 11, FW_SEMIBOLD);

    int y = rc.top;

    if (!d.valid)
    {
        std::wstring hint = L"该文件可能不是有效的 DSHPack 整合包。\n要求:标准 ZIP,根目录含 manifest.json。";
        int titleH = MeasureHeight(hdc, L"无法预览", fTitle, W - 2 * pad, DT_SINGLELINE);
        int errH = MeasureHeight(hdc, d.error, fSub, W - 2 * pad, DT_SINGLELINE | DT_END_ELLIPSIS);
        int hdH = pad + titleH + gap + errH + pad;

        RECT hd = { rc.left, rc.top, rc.right, rc.top + hdH };
        HBRUSH acc = CreateSolidBrush(RGB(172, 60, 60));
        FillRect(hdc, &hd, acc); DeleteObject(acc);

        RECT tr = { hd.left + pad, hd.top + pad, hd.right - pad, hd.top + pad + titleH };
        CardText(hdc, &tr, L"无法预览", fTitle, RGB(255,255,255), DT_SINGLELINE);
        RECT er = { hd.left + pad, tr.bottom + gap, hd.right - pad, tr.bottom + gap + errH };
        CardText(hdc, &er, d.error, fSub, RGB(255,235,235), DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT br = { rc.left + pad, hd.bottom + gap, rc.right - pad, rc.bottom - pad };
        CardText(hdc, &br, hint, fBody, P.sub, DT_WORDBREAK);
        goto done;
    }

    {
        const ManifestInfo& m = d.info;

        // header band (accent) — sized by measured text, DPI-adaptive
        std::wstring title = m.displayName.empty() ? m.name : m.displayName;
        std::wstring sub = L"v" + m.version;
        std::wstring type = m.type.empty() ? L"profile" : m.type;
        sub += L"  ·  " + type;
        if (!m.dshVersion.empty()) sub += L"  ·  DSH " + m.dshVersion;

        int titleH = MeasureHeight(hdc, title, fTitle, W - 2 * pad, DT_SINGLELINE);
        int subH = MeasureHeight(hdc, sub, fSub, W - 2 * pad, DT_SINGLELINE);
        int hdH = pad + titleH + gap + subH + pad;

        RECT hd = { rc.left, rc.top, rc.right, rc.top + hdH };
        HBRUSH acc = CreateSolidBrush(P.accent);
        FillRect(hdc, &hd, acc); DeleteObject(acc);

        RECT tr = { hd.left + pad, hd.top + pad, hd.right - pad, hd.top + pad + titleH };
        CardText(hdc, &tr, title, fTitle, RGB(255,255,255), DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT srt = { hd.left + pad, tr.bottom + gap, hd.right - pad, tr.bottom + gap + subH };
        CardText(hdc, &srt, sub, fSub, RGB(226,229,246), DT_SINGLELINE | DT_END_ELLIPSIS);

        y = hd.bottom + gap;

        // stats chips — height from font metrics
        int numH = LineHeight(hdc, fNum);
        int chipLblH = LineHeight(hdc, fChip);
        int chipH = (int)(6 * sc) + numH + (int)(4 * sc) + chipLblH + (int)(6 * sc);
        int cgap = (int)(12 * sc);
        int chipW = (W - pad * 2 - cgap * 2) / 3;
        struct Chip { std::wstring num; std::wstring lbl; };
        Chip chips[3] = {
            { std::to_wstring(m.bundles), L"整合包" },
            { std::to_wstring(m.dependencies), L"依赖" },
            { std::to_wstring(m.files), L"文件" }
        };
        for (int i = 0; i < 3; ++i)
        {
            RECT cr = { rc.left + pad + i * (chipW + cgap), y, rc.left + pad + i * (chipW + cgap) + chipW, y + chipH };
            HBRUSH cb = CreateSolidBrush(P.chip);
            FillRect(hdc, &cr, cb); DeleteObject(cb);
            RECT nr = { cr.left + 8, cr.top + (int)(6 * sc), cr.right - 8, cr.top + (int)(6 * sc) + numH };
            CardText(hdc, &nr, chips[i].num, fNum, P.chipNum, DT_SINGLELINE);
            RECT lr = { cr.left + 8, nr.bottom + (int)(4 * sc), cr.right - 8, nr.bottom + (int)(4 * sc) + chipLblH };
            CardText(hdc, &lr, chips[i].lbl, fChip, P.sub, DT_SINGLELINE);
        }
        y += chipH + gap;

        // description block — measured, capped at 4 lines
        if (!m.description.empty())
        {
            int lh = LineHeight(hdc, fBody);
            int dw = W - 2 * pad;
            int fullH = MeasureHeight(hdc, m.description, fBody, dw, DT_WORDBREAK);
            int maxH = lh * 4;
            int showH = fullH < maxH ? fullH : maxH;
            RECT dr = { rc.left + pad, y, rc.right - pad, y + showH };
            CardText(hdc, &dr, m.description, fBody, P.sub, DT_WORDBREAK | DT_END_ELLIPSIS);
            y = dr.bottom + gap;
        }

        // metadata rows — label/value aligned on the same baseline/line height
        int lh = LineHeight(hdc, fBody);
        int lblW = (int)(84 * sc + 0.5);
        struct Row { const wchar_t* k; std::wstring v; };
        Row rows[3] = {
            { L"名称", m.name },
            { L"作者", m.author.empty() ? L"——" : m.author },
            { L"DSH 版本", m.dshVersion.empty() ? L"——" : m.dshVersion },
        };
        for (int i = 0; i < 3; ++i)
        {
            RECT lr = { rc.left + pad, y, rc.left + pad + lblW, y + lh };
            CardText(hdc, &lr, rows[i].k, fLbl, P.text, DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT vr = { lr.right + (int)(12 * sc), y, rc.right - pad, y + lh };
            CardText(hdc, &vr, rows[i].v, fBody, P.sub, DT_SINGLELINE | DT_END_ELLIPSIS);
            y += lh + (int)(6 * sc);
        }
    }

done:
    DeleteObject(fTitle);
    DeleteObject(fSub);
    DeleteObject(fNum);
    DeleteObject(fChip);
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

class CDspackPreviewHandler : public IPreviewHandler, public IInitializeWithFile, public IInitializeWithStream
{
    volatile LONG m_ref;
    wchar_t* m_file;
    std::vector<BYTE> m_streamBuf;
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
        char rs[64]; GuidStr(riid, rs, sizeof(rs));
        HRESULT hr = E_NOINTERFACE;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IPreviewHandler))
        { *ppv = static_cast<IPreviewHandler*>(this); hr = S_OK; }
        else if (IsEqualGUID(riid, IID_IInitializeWithFile))
        { *ppv = static_cast<IInitializeWithFile*>(this); hr = S_OK; }
        else if (IsEqualGUID(riid, IID_IInitializeWithStream))
        { *ppv = static_cast<IInitializeWithStream*>(this); hr = S_OK; }
        LogF("handler QI %s => 0x%08X", rs, (unsigned)hr);
        if (FAILED(hr)) return hr;
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

    // IInitializeWithStream
    STDMETHODIMP Initialize(IStream* pstream, DWORD grfMode)
    {
        Log("Initialize(stream)");
        m_streamBuf.clear();
        if (pstream) ReadStreamToVec(pstream, m_streamBuf);
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
        if (!m_streamBuf.empty()) LoadAndParseBuf(m_streamBuf.data(), m_streamBuf.size(), *d);
        else if (m_file) LoadAndParse(m_file, *d);
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
        char rs[64]; GuidStr(riid, rs, sizeof(rs));
        LogF("CreateInstance riid=%s", rs);
        if (ppv) *ppv = 0;
        if (pUnkOuter) { Log("  => CLASS_E_NOAGGREGATION"); return CLASS_E_NOAGGREGATION; }
        CDspackPreviewHandler* p = new (std::nothrow) CDspackPreviewHandler();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        LogF("  => hr=0x%08X", (unsigned)hr);
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

static HRESULT SetRegDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD val)
{
    HKEY k = 0;
    LONG lr = RegCreateKeyExW(root, sub, 0, 0, 0, KEY_WRITE, 0, &k, 0);
    if (lr != ERROR_SUCCESS) return HRESULT_FROM_WIN32(lr);
    lr = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
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
    // ★ 托管到 prevhost.exe 代理(决定性:缺 AppID 则不进 prevhost,预览报"无法预览")
    SetRegValue(HKEY_CLASSES_ROOT, sub, L"AppID", L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}");
    // 允许预览未受信任文件(与系统预览器一致)
    SetRegDword(HKEY_CLASSES_ROOT, sub, L"AutomaticallyPreviewUntrustedFiles", 1);
    swprintf_s(sub, L"CLSID\\%s\\InprocServer32", clsid);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, dll);
    SetRegValue(HKEY_CLASSES_ROOT, sub, L"ThreadingModel", L"Apartment");
    swprintf_s(sub, L".dspack\\shellex\\%s", kPreviewCat);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, clsid);
    swprintf_s(sub, L"%s\\CLSID", kProgId);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, clsid);

    // .dspack 文件类型 → ProgId(让 shell 识别该扩展为已注册类型)
    SetRegValue(HKEY_CLASSES_ROOT, L".dspack", 0, kProgId);
    // ProgId 下也挂预览处理器(部分 shell 从 ProgId 而非扩展名查找 shellex)
    swprintf_s(sub, L"%s\\shellex\\%s", kProgId, kPreviewCat);
    SetRegValue(HKEY_CLASSES_ROOT, sub, 0, clsid);
    // 登记到系统预览处理器清单(shell 据此确认可托管的处理器)
    swprintf_s(sub, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers");
    SetRegValue(HKEY_LOCAL_MACHINE, sub, clsid, L"DSH-PackForge .dspack Preview Handler");
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
    swprintf_s(sub, L"%s\\shellex\\%s", kProgId, kPreviewCat);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    swprintf_s(sub, L"%s\\CLSID", kProgId);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, sub);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, kProgId);
    // 从系统预览处理器清单移除
    HKEY hk = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hk, clsid);
        RegCloseKey(hk);
    }
    return S_OK;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    char cs[64], rs[64];
    GuidStr(rclsid, cs, sizeof(cs)); GuidStr(riid, rs, sizeof(rs));
    LogF("DllGetClassObject cls=%s riid=%s", cs, rs);
    if (!IsEqualGUID(rclsid, CLSID_DspackPreviewHandler)) { Log("  => CLASS_E_CLASSNOTAVAILABLE"); return CLASS_E_CLASSNOTAVAILABLE; }
    CDspackClassFactory* f = new (std::nothrow) CDspackClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    LogF("  => hr=0x%08X", (unsigned)hr);
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
        LogProcess();
    }
    return TRUE;
}