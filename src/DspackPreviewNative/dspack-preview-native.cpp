// dspack-preview-native.cpp
// Native COM preview handler for .dspack files (Windows Preview Handler).
// Implements IPreviewHandler + IInitializeWithFile; parses the .dspack and renders
// a summary card with GDI. Logs to %USERPROFILE%\AppData\LocalLow\dspack-preview-native.log.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <new>
#include <wchar.h>
#include <cstring>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <gdiplus.h>
#include "dspack-read.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

// {7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d}
const CLSID CLSID_DspackPreviewHandler = { 0x7f3c5a1e, 0x2b4d, 0x4e6a, { 0x9c,0x8b,0x1d,0x5f,0x7a,0x3e,0x9c,0x2d } };

namespace
{
const wchar_t kProgId[] = L"DspackPreview.PreviewHandler";
const wchar_t kPreviewCat[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
const wchar_t kHostClass[] = L"DspackPreview.Host";
// 两条导入协议,url 均为「本地 .dspack 文件的路径」:
// 官方目录:dspack://install?url=<URL编码后的本地路径>
// 启动器:dsh-launcher://pack?url=<URL编码后的本地路径>
const wchar_t kCatalogScheme[] = L"dspack://install?url=";
const wchar_t kLauncherScheme[] = L"dsh-launcher://pack?url=";

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
static bool IsDarkColor(COLORREF c)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    return (r * 299 + g * 587 + b * 114) / 1000 < 128;
}

// GDI+ 惰性初始化：不能在 DllMain(DLL_PROCESS_ATTACH) 里 GdiplusStartup，
// 否则会触发 loader-lock 死锁。改为首次渲染时启动（STA，单线程调用即可）。
static ULONG_PTR g_gdipToken = 0;
static bool EnsureGdip()
{
    if (g_gdipToken) return true;
    Gdiplus::GdiplusStartupInput si;
    return Gdiplus::GdiplusStartup(&g_gdipToken, &si, NULL) == Gdiplus::Ok;
}

struct PreviewData
{
    bool valid = false;
    uint32_t headerVersion = 0;   // 旧 8 字节 DSPK 魔数头里的版本（兼容用）
    int containerVersion = 0;     // dspack.json 的 version（2/3；旧魔数头=1）
    ManifestInfo info;
    std::wstring error;
    RECT btnCatalog = { 0, 0, 0, 0 };
    RECT btnLauncher = { 0, 0, 0, 0 };
    bool hasButtons = false;
    std::wstring sourcePath;
    int scrollY = 0;              // 垂直滚动偏移（设备像素）
    int contentHeight = 0;        // 内容总高（用于滚动条范围）
    int hoverBtn = 0;             // 0=无 1=官方目录 2=启动器
    bool dark = false;            // 深色模式
};

void LoadAndParseBuf(const BYTE* data, size_t size, PreviewData& d)
{
    // .dspack = 标准 ZIP（根目录含 manifest.json + dspack.json），无前导魔数字节。
    // 仅为兼容早期的 8 字节 "DSPK" 头做一次剥头（新文件不会再带）。
    const BYTE* zip = data;
    size_t zipSize = size;
    bool legacy = false;
    if (size >= 8 && memcmp(data, "DSPK", 4) == 0)
    {
        d.headerVersion = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
        zip = data + 8;
        zipSize = size - 8;
        legacy = true;
    }

    // 1) 容器标记 dspack.json（pack-structure v2/v3）：判定身份 + 版本。
    std::vector<BYTE> cj;
    ContainerInfo ci;
    if (ReadZipEntry(zip, zipSize, "dspack.json", cj))
    {
        if (!ParseContainerJson(cj.data(), cj.size(), ci) || !ci.valid)
        {
            d.valid = false;
            d.error = L"dspack.json 无效（format 应为 \"dspack\"）";
            return;
        }
        if (ci.version < 2 || ci.version > 3)
        {
            d.valid = false;
            d.error = L"不支持的 .dspack 容器版本（支持 v2 / v3）";
            return;
        }
        d.containerVersion = ci.version;
    }
    else if (!legacy)
    {
        d.valid = false;
        d.error = L"不是有效的 .dspack（缺少 dspack.json）";
        return;
    }
    else
    {
        d.containerVersion = 1; // 旧 8 字节魔数头格式
    }

    // 2) manifest.json
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

// ---------- GDI+ rendering ----------
struct Palette
{
    bool dark = false;
    Gdiplus::Color canvas, card, cardBorder, shadow;
    Gdiplus::Color headerA, headerB, headerText, headerSub, headerCaption;
    Gdiplus::Color chipFill, chipText;
    Gdiplus::Color text, sub;
    Gdiplus::Color statCard, statCardBorder, statNum, statLbl;
    Gdiplus::Color btnA, btnB, btnText, btnHoverA, btnHoverB;
    Gdiplus::Color btnSecFill, btnSecBorder, btnSecText, btnSecHover;
    Gdiplus::Color divider, accent;
    Gdiplus::Color errA, errB;
};

static Palette MakePalette(bool dark)
{
    Palette P;
    P.dark = dark;
    if (!dark)
    {
        P.canvas        = Gdiplus::Color(238, 240, 247);
        P.card          = Gdiplus::Color(255, 255, 255);
        P.cardBorder    = Gdiplus::Color(227, 230, 240);
        P.shadow        = Gdiplus::Color(20, 24, 34, 60);
        P.headerA       = Gdiplus::Color(74, 80, 168);
        P.headerB       = Gdiplus::Color(91, 95, 190);
        P.headerText    = Gdiplus::Color(255, 255, 255);
        P.headerSub     = Gdiplus::Color(214, 217, 245);
        P.headerCaption = Gdiplus::Color(203, 207, 240);
        P.chipFill      = Gdiplus::Color(34, 255, 255, 255);
        P.chipText      = Gdiplus::Color(237, 239, 251);
        P.text          = Gdiplus::Color(32, 35, 58);
        P.sub           = Gdiplus::Color(106, 112, 136);
        P.statCard      = Gdiplus::Color(255, 255, 255);
        P.statCardBorder = Gdiplus::Color(236, 239, 246);
        P.statNum       = Gdiplus::Color(74, 80, 168);
        P.statLbl       = Gdiplus::Color(106, 112, 136);
        P.btnA          = Gdiplus::Color(74, 80, 168);
        P.btnB          = Gdiplus::Color(91, 95, 190);
        P.btnText       = Gdiplus::Color(255, 255, 255);
        P.btnHoverA     = Gdiplus::Color(86, 93, 184);
        P.btnHoverB     = Gdiplus::Color(104, 109, 206);
        P.btnSecFill    = Gdiplus::Color(255, 255, 255);
        P.btnSecBorder  = Gdiplus::Color(217, 221, 236);
        P.btnSecText    = Gdiplus::Color(74, 80, 168);
        P.btnSecHover   = Gdiplus::Color(241, 243, 251);
        P.divider       = Gdiplus::Color(238, 240, 246);
        P.accent        = Gdiplus::Color(74, 80, 168);
        P.errA          = Gdiplus::Color(168, 74, 84);
        P.errB          = Gdiplus::Color(186, 90, 102);
    }
    else
    {
        P.canvas        = Gdiplus::Color(30, 32, 40);
        P.card          = Gdiplus::Color(38, 41, 51);
        P.cardBorder    = Gdiplus::Color(51, 55, 66);
        P.shadow        = Gdiplus::Color(70, 0, 0, 0);
        P.headerA       = Gdiplus::Color(51, 56, 94);
        P.headerB       = Gdiplus::Color(61, 67, 112);
        P.headerText    = Gdiplus::Color(242, 243, 250);
        P.headerSub     = Gdiplus::Color(169, 174, 203);
        P.headerCaption = Gdiplus::Color(166, 171, 205);
        P.chipFill      = Gdiplus::Color(26, 255, 255, 255);
        P.chipText      = Gdiplus::Color(232, 234, 246);
        P.text          = Gdiplus::Color(231, 233, 242);
        P.sub           = Gdiplus::Color(154, 160, 180);
        P.statCard      = Gdiplus::Color(46, 51, 64);
        P.statCardBorder = Gdiplus::Color(58, 64, 80);
        P.statNum       = Gdiplus::Color(166, 171, 224);
        P.statLbl       = Gdiplus::Color(154, 160, 180);
        P.btnA          = Gdiplus::Color(74, 80, 168);
        P.btnB          = Gdiplus::Color(91, 95, 190);
        P.btnText       = Gdiplus::Color(255, 255, 255);
        P.btnHoverA     = Gdiplus::Color(86, 93, 184);
        P.btnHoverB     = Gdiplus::Color(104, 109, 206);
        P.btnSecFill    = Gdiplus::Color(38, 41, 51);
        P.btnSecBorder  = Gdiplus::Color(58, 64, 80);
        P.btnSecText    = Gdiplus::Color(166, 171, 224);
        P.btnSecHover   = Gdiplus::Color(48, 54, 72);
        P.divider       = Gdiplus::Color(51, 55, 66);
        P.accent        = Gdiplus::Color(120, 126, 200);
        P.errA          = Gdiplus::Color(120, 52, 62);
        P.errB          = Gdiplus::Color(140, 66, 78);
    }
    return P;
}

static const wchar_t* UiFontFamily()
{
    static bool checked = false;
    static const wchar_t* fam = L"Segoe UI";
    if (!checked)
    {
        checked = true;
        Gdiplus::FontFamily ff(L"Segoe UI Variable");
        if (ff.IsAvailable()) fam = L"Segoe UI Variable";
    }
    return fam;
}

static int UiPx(double pt, double sc) { return (int)(pt * sc + 0.5); }

static void RoundedRectPath(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float radius)
{
    float d = radius * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static void RoundedTopRectPath(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float radius)
{
    float d = radius * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddLine(r.X + r.Width, r.Y + r.Height, r.X, r.Y + r.Height);
    p.CloseFigure();
}

static void FillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Gdiplus::Brush& b)
{
    Gdiplus::GraphicsPath p;
    RoundedRectPath(p, r, radius);
    g.FillPath(&b, &p);
}

static void StrokeRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Gdiplus::Pen& pen)
{
    Gdiplus::GraphicsPath p;
    RoundedRectPath(p, r, radius);
    g.DrawPath(&pen, &p);
}

static void FillHeaderGradient(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::Color a, Gdiplus::Color b)
{
    Gdiplus::GraphicsPath p;
    RoundedTopRectPath(p, r, radius);
    Gdiplus::LinearGradientBrush br(r, a, b, Gdiplus::LinearGradientModeVertical);
    g.FillPath(&br, &p);
}

static void DrawTextF(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font& f,
    const Gdiplus::Color& c, const Gdiplus::RectF& rc, bool center, bool vcenter, bool wrap)
{
    Gdiplus::SolidBrush br(c);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(center ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(vcenter ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
    if (wrap) fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
    else
    {
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip | Gdiplus::StringFormatFlagsNoWrap);
        fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    }
    g.DrawString(s.c_str(), (INT)s.size(), &f, rc, &fmt, &br);
}

static int MeasureHeightF(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font& f, int widthPx)
{
    Gdiplus::StringFormat fmt;
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
    Gdiplus::RectF box(0.0f, 0.0f, (float)widthPx, 100000.0f);
    Gdiplus::RectF out;
    g.MeasureString(s.c_str(), (INT)s.size(), &f, box, &fmt, &out);
    return (int)(out.Height + 0.5f);
}

static int MeasureWidthF(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font& f)
{
    Gdiplus::StringFormat fmt(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::PointF org(0.0f, 0.0f);
    Gdiplus::RectF out;
    g.MeasureString(s.c_str(), (INT)s.size(), &f, org, &fmt, &out);
    return (int)(out.Width + 0.5f);
}

static int LineHeightF(Gdiplus::Graphics& g, Gdiplus::Font& f)
{
    Gdiplus::StringFormat fmt(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::PointF org(0.0f, 0.0f);
    Gdiplus::RectF out;
    g.MeasureString(L"Ag", 2, &f, org, &fmt, &out);
    return (int)(out.Height + 0.5f);
}

static std::string UrlEncodeUtf8(const std::wstring& s)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), 0, 0, 0, 0);
    std::string u8(len, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, 0, 0);
    static const char* hx = "0123456789ABCDEF";
    std::string out;
    out.reserve(u8.size() * 3);
    for (unsigned char c : u8)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~')
            out += (char)c;
        else { out += '%'; out += hx[c >> 4]; out += hx[c & 15]; }
    }
    return out;
}

static std::wstring MakeInstallUrl(const wchar_t* scheme, const std::wstring& dspackAddress)
{
    std::string enc = UrlEncodeUtf8(dspackAddress);
    std::wstring u = scheme;
    u.reserve(wcslen(scheme) + enc.size());
    for (char c : enc) u += (wchar_t)(unsigned char)c;
    return u;
}

static void LaunchImport(const wchar_t* scheme, const std::wstring& dspackAddress)
{
    std::wstring u = MakeInstallUrl(scheme, dspackAddress);
    char ub[1536];
    WideCharToMultiByte(CP_UTF8, 0, u.c_str(), -1, ub, (int)sizeof(ub), 0, 0);
    LogF("button click -> %s", ub);
    HINSTANCE r = ShellExecuteW(0, L"open", u.c_str(), 0, 0, SW_SHOWNORMAL);
    LogF("ShellExecute => 0x%p", r);
}

// 导入目标 = 本地 .dspack 文件路径;流模式取不到路径时用 file:/// 占位兜底。
static std::wstring PackUrl(const PreviewData& d)
{
    if (!d.sourcePath.empty()) return d.sourcePath;
    return std::wstring(L"file:///") + d.info.name + L"-" + d.info.version + L".dspack";
}

static void DrawButton(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const std::wstring& s,
    Gdiplus::Font& f, const Palette& P, bool primary, bool hover, float radius)
{
    if (primary)
    {
        Gdiplus::Color a = hover ? P.btnHoverA : P.btnA;
        Gdiplus::Color b = hover ? P.btnHoverB : P.btnB;
        Gdiplus::LinearGradientBrush br(r, a, b, Gdiplus::LinearGradientModeVertical);
        Gdiplus::GraphicsPath p;
        RoundedRectPath(p, r, radius);
        g.FillPath(&br, &p);
        DrawTextF(g, s, f, P.btnText, r, true, true, false);
    }
    else
    {
        Gdiplus::SolidBrush fill(hover ? P.btnSecHover : P.btnSecFill);
        FillRounded(g, r, radius, fill);
        Gdiplus::Pen pen(P.btnSecBorder, 1.0f);
        StrokeRounded(g, r, radius, pen);
        DrawTextF(g, s, f, P.btnSecText, r, true, true, false);
    }
}

// 统计卡（白底 + 描边 + 微阴影，数字 + 标签）
static void DrawStatCard(Gdiplus::Graphics& g, const Gdiplus::RectF& r, int n, const wchar_t* lbl,
    Gdiplus::Font& fNum, Gdiplus::Font& fLbl, const Palette& P, float radius)
{
    Gdiplus::SolidBrush sh(Gdiplus::Color(P.dark ? 40 : 12, 20, 24, 50));
    FillRounded(g, Gdiplus::RectF(r.X, r.Y + 1.0f, r.Width, r.Height + 1.0f), radius, sh);
    Gdiplus::SolidBrush card(P.statCard);
    FillRounded(g, r, radius, card);
    Gdiplus::Pen pen(P.statCardBorder, 1.0f);
    StrokeRounded(g, r, radius, pen);

    int numH = LineHeightF(g, fNum);
    int lblH = LineHeightF(g, fLbl);
    float totalH = (float)(numH + lblH + UiPx(3, 1.0));
    float top = r.Y + (r.Height - totalH) / 2.0f;
    Gdiplus::RectF nr(r.X, top, r.Width, (float)numH);
    DrawTextF(g, std::to_wstring(n), fNum, P.statNum, nr, true, true, false);
    Gdiplus::RectF lr(r.X, top + numH + UiPx(3, 1.0), r.Width, (float)lblH);
    DrawTextF(g, lbl, fLbl, P.statLbl, lr, true, true, false);
}

void RenderPreview(HDC hdc, const RECT& rc, PreviewData& d)
{
    if (!EnsureGdip()) return;
    double sc = GetDeviceCaps(hdc, LOGPIXELSY) / 96.0;
    Palette P = MakePalette(d.dark);

    // 裁剪到客户区（设备坐标）；绘制时手动减 scrollY 实现滚动。
    IntersectClipRect(hdc, rc.left, rc.top, rc.right, rc.bottom);

    Gdiplus::Graphics g(hdc);
    g.SetPageUnit(Gdiplus::UnitPixel);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    Gdiplus::SolidBrush canvas(P.canvas);
    g.FillRectangle(&canvas, (float)rc.left, (float)rc.top, (float)W, (float)H);

    Gdiplus::Font fTitle(UiFontFamily(), (float)UiPx(22, sc), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fSub(UiFontFamily(), (float)UiPx(12, sc), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fNum(UiFontFamily(), (float)UiPx(20, sc), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fLbl(UiFontFamily(), (float)UiPx(12, sc), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fBody(UiFontFamily(), (float)UiPx(12, sc), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fChip(UiFontFamily(), (float)UiPx(10.5, sc), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fBadge(UiFontFamily(), (float)UiPx(10, sc), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

    float m = (float)UiPx(16, sc);          // 卡片外边距
    float radius = (float)UiPx(14, sc);     // 卡片圆角
    float padX = (float)UiPx(20, sc);       // 卡片内水平留白
    float cardX = rc.left + m;
    float cardW = W - 2 * m;
    float cw = cardW - 2 * padX;

    if (!d.valid)
    {
        d.scrollY = 0;
        d.contentHeight = H;
        float eTop = rc.top + m;
        int titleH = LineHeightF(g, fTitle);
        int errH = LineHeightF(g, fSub);
        float hpadV = (float)UiPx(18, sc);
        float hdH = hpadV + titleH + UiPx(8, sc) + errH + hpadV;
        float cardH = (float)H;

        Gdiplus::SolidBrush sh(P.shadow);
        FillRounded(g, Gdiplus::RectF(cardX + 2, eTop + 4, cardW, cardH), radius, sh);
        Gdiplus::SolidBrush cardFill(P.card);
        FillRounded(g, Gdiplus::RectF(cardX, eTop, cardW, cardH), radius, cardFill);
        FillHeaderGradient(g, Gdiplus::RectF(cardX, eTop, cardW, hdH), radius, P.errA, P.errB);
        Gdiplus::Pen border(P.cardBorder, 1.0f);
        StrokeRounded(g, Gdiplus::RectF(cardX, eTop, cardW, cardH), radius, border);

        Gdiplus::RectF tr(cardX + padX, eTop + hpadV, cw, (float)titleH);
        DrawTextF(g, L"无法预览", fTitle, P.headerText, tr, false, true, false);
        Gdiplus::RectF er(cardX + padX, tr.GetBottom() + UiPx(8, sc), cw, (float)errH);
        DrawTextF(g, d.error, fSub, P.headerSub, er, false, true, false);

        std::wstring hint = L"这不是有效的 DSHPack 整合包。\n要求：标准 ZIP，根目录含 dspack.json 与 manifest.json。";
        float hy = eTop + hdH + UiPx(16, sc);
        Gdiplus::RectF hr(cardX + padX, hy, cw, eTop + cardH - padX - hy);
        DrawTextF(g, hint, fBody, P.sub, hr, false, false, true);
        return;
    }

    const ManifestInfo& mf = d.info;
    bool isHome = (mf.type == L"dshhome");
    std::wstring title = mf.displayName.empty() ? mf.name : mf.displayName;
    if (title.empty()) title = L"未命名整合包";
    std::wstring typeLabel = isHome ? L"工作台"
        : ((mf.type.empty() || mf.type == L"profile") ? L"整合包" : mf.type);

    std::wstring mvCap = mf.manifestVersion > 0
        ? (L"Manifest v" + std::to_wstring(mf.manifestVersion)) : L"Manifest v?";
    std::wstring cvCap = d.containerVersion >= 2
        ? (L"容器 v" + std::to_wstring(d.containerVersion))
        : (d.containerVersion == 1 ? L"旧容器" : L"容器 v?");
    std::wstring cap = mvCap + L"  ·  " + cvCap;

    // ---------- 阶段一：测量，算总高 ----------
    int titleH = LineHeightF(g, fTitle);
    int subH = LineHeightF(g, fSub);
    int badgeH = LineHeightF(g, fBadge);
    int capH = LineHeightF(g, fChip);
    int sectLh = LineHeightF(g, fLbl);
    int bodyLh = LineHeightF(g, fBody);

    float hpadV = (float)UiPx(18, sc);
    float bt = (float)UiPx(40, sc);         // 品牌瓦片
    float hdH = hpadV + bt + UiPx(14, sc) + titleH + UiPx(6, sc) + subH + hpadV;

    int statH = UiPx(66, sc);
    int btnH = UiPx(40, sc);
    int rowGap = UiPx(6, sc);
    int sectGap = UiPx(18, sc);
    int cgap = UiPx(12, sc);

    int descH = 0;
    if (!mf.description.empty())
    {
        int fullH = MeasureHeightF(g, mf.description, fBody, (int)cw);
        int maxH = bodyLh * 4;
        descH = fullH < maxH ? fullH : maxH;
    }

    int metaH = 4 * bodyLh + 3 * rowGap;

    float bodyH = statH
        + sectGap + sectLh + UiPx(12, sc)
        + btnH
        + sectGap
        + (descH ? descH + sectGap : 0.0f)
        + metaH
        + padX;                            // 底部留白

    float cardH = hdH + bodyH;
    d.contentHeight = (int)(2.0 * m + cardH + 0.5);

    int maxScroll = d.contentHeight - H;
    if (maxScroll < 0) maxScroll = 0;
    if (d.scrollY > maxScroll) d.scrollY = maxScroll;
    if (d.scrollY < 0) d.scrollY = 0;

    float base = rc.top + m - d.scrollY;
    Gdiplus::RectF cardRect(cardX, base, cardW, cardH);

    // ---------- 阶段二：绘制 ----------
    // 卡片阴影 + 白底
    Gdiplus::SolidBrush sh(P.shadow);
    FillRounded(g, Gdiplus::RectF(cardRect.X + 2, cardRect.Y + 4, cardRect.Width, cardRect.Height + 2), radius, sh);
    Gdiplus::SolidBrush cardFill(P.card);
    FillRounded(g, cardRect, radius, cardFill);

    // 头带渐变 + 右上装饰光斑（裁剪到头带内）
    FillHeaderGradient(g, Gdiplus::RectF(cardX, base, cardW, hdH), radius, P.headerA, P.headerB);
    {
        Gdiplus::GraphicsPath hp;
        RoundedTopRectPath(hp, Gdiplus::RectF(cardX, base, cardW, hdH), radius);
        Gdiplus::Region hreg(&hp);
        Gdiplus::GraphicsState st = g.Save();
        g.SetClip(&hreg, Gdiplus::CombineModeIntersect);
        Gdiplus::SolidBrush c1(Gdiplus::Color(16, 255, 255, 255));
        g.FillEllipse(&c1, cardX + cardW - 110, base - 36, 150.0f, 150.0f);
        Gdiplus::SolidBrush c2(Gdiplus::Color(12, 255, 255, 255));
        g.FillEllipse(&c2, cardX + cardW - 46, base + hdH - 72, 104.0f, 104.0f);
        g.Restore(st);
    }

    // 品牌瓦片 + 徽标胶囊 + 版本说明
    float tileX = cardX + padX;
    float tileY = base + hpadV;
    Gdiplus::RectF tile(tileX, tileY, bt, bt);
    Gdiplus::SolidBrush tileFill(P.chipFill);
    FillRounded(g, tile, bt * 0.28f, tileFill);
    DrawTextF(g, L"DSH", fBadge, P.headerText, tile, true, true, false);

    float badgeX = tileX + bt + UiPx(12, sc);
    int pillH = badgeH + UiPx(8, sc);
    float pillW = (float)(MeasureWidthF(g, L"DSH 整合包", fBadge) + UiPx(18, sc));
    Gdiplus::RectF pill(badgeX, tileY + (bt - pillH) / 2.0f, pillW, (float)pillH);
    Gdiplus::SolidBrush pillFill(P.chipFill);
    FillRounded(g, pill, (float)pillH / 2.0f, pillFill);
    DrawTextF(g, L"DSH 整合包", fBadge, P.headerText, pill, true, true, false);

    Gdiplus::RectF capR(badgeX, pill.GetBottom() + UiPx(2, sc), cw - (badgeX - cardX), (float)capH);
    DrawTextF(g, cap, fChip, P.headerCaption, capR, false, true, false);

    // 标题 + 版本
    Gdiplus::RectF tr(cardX + padX, tileY + bt + UiPx(14, sc), cw, (float)titleH);
    DrawTextF(g, title, fTitle, P.headerText, tr, false, true, false);
    Gdiplus::RectF srt(cardX + padX, tr.GetBottom() + UiPx(6, sc), cw, (float)subH);
    DrawTextF(g, L"v" + mf.version, fSub, P.headerSub, srt, false, true, false);

    // 正文
    float bx = cardX + padX;
    float y = base + hdH;

    // 统计卡
    float statW = (cw - 2 * cgap) / 3.0f;
    int statVals[3]; const wchar_t* statLbls[3];
    if (isHome) { statVals[0] = mf.profiles; statLbls[0] = L"环境"; statVals[1] = mf.presets; statLbls[1] = L"预设"; statVals[2] = mf.skills; statLbls[2] = L"技能"; }
    else        { statVals[0] = mf.bundles; statLbls[0] = L"层栈"; statVals[1] = mf.dependencies; statLbls[1] = L"依赖"; statVals[2] = mf.files; statLbls[2] = L"下载清单"; }
    for (int i = 0; i < 3; ++i)
    {
        Gdiplus::RectF sr(bx + i * (statW + cgap), y, statW, (float)statH);
        DrawStatCard(g, sr, statVals[i], statLbls[i], fNum, fChip, P, (float)UiPx(10, sc));
    }
    y += statH + sectGap;

    // 一键导入
    Gdiplus::RectF sect(bx, y, cw, (float)sectLh);
    DrawTextF(g, L"一键导入", fLbl, P.text, sect, false, true, false);
    y += sectLh + UiPx(12, sc);

    float btnGap = UiPx(10, sc);
    float btnW = (cw - btnGap) / 2.0f;
    Gdiplus::RectF bcat(bx, y, btnW, (float)btnH);
    Gdiplus::RectF blch(bx + btnW + btnGap, y, btnW, (float)btnH);
    DrawButton(g, bcat, L"官方目录", fLbl, P, true, d.hoverBtn == 1, (float)UiPx(9, sc));
    DrawButton(g, blch, L"启动器", fLbl, P, false, d.hoverBtn == 2, (float)UiPx(9, sc));
    d.btnCatalog.left = (LONG)bcat.X; d.btnCatalog.top = (LONG)bcat.Y;
    d.btnCatalog.right = (LONG)(bcat.X + bcat.Width); d.btnCatalog.bottom = (LONG)(bcat.Y + bcat.Height);
    d.btnLauncher.left = (LONG)blch.X; d.btnLauncher.top = (LONG)blch.Y;
    d.btnLauncher.right = (LONG)(blch.X + blch.Width); d.btnLauncher.bottom = (LONG)(blch.Y + blch.Height);
    d.hasButtons = true;
    y += btnH + sectGap;

    // 分隔线
    Gdiplus::Pen divPen(P.divider, 1.0f);
    g.DrawLine(&divPen, bx, y, bx + cw, y);
    y += sectGap;

    // 描述
    if (descH)
    {
        Gdiplus::RectF dr(bx, y, cw, (float)descH);
        DrawTextF(g, mf.description, fBody, P.sub, dr, false, false, true);
        y += descH + sectGap;
    }

    // 元数据行
    float lblW = (float)UiPx(96, sc);
    const wchar_t* rk[4]; std::wstring rv[4];
    rk[0] = L"标识"; rv[0] = mf.name;
    rk[1] = L"类型"; rv[1] = typeLabel;
    if (isHome) { rk[2] = L"默认环境"; rv[2] = mf.defaultProfile.empty() ? L"——" : mf.defaultProfile; }
    else        { rk[2] = L"作者";     rv[2] = mf.author.empty() ? L"——" : mf.author; }
    rk[3] = L"DSH 版本"; rv[3] = mf.dshVersion.empty() ? L"——" : mf.dshVersion;
    for (int i = 0; i < 4; ++i)
    {
        Gdiplus::RectF lr(bx, y, lblW, (float)bodyLh);
        DrawTextF(g, rk[i], fChip, P.sub, lr, false, true, false);
        Gdiplus::RectF vr(bx + lblW + UiPx(10, sc), y, cw - lblW - UiPx(10, sc), (float)bodyLh);
        DrawTextF(g, rv[i], fBody, P.text, vr, false, true, false);
        y += bodyLh + rowGap;
    }

    // 卡片描边（最上层，保持清晰）
    Gdiplus::Pen border(P.cardBorder, 1.0f);
    StrokeRounded(g, cardRect, radius, border);
}

// ---------- 滚动 ----------
static void SyncScrollbar(HWND hwnd, PreviewData* d)
{
    RECT cr; GetClientRect(hwnd, &cr);
    int H = cr.bottom - cr.top;
    int max = d->contentHeight - H;
    if (max < 0) max = 0;
    if (d->scrollY > max) d->scrollY = max;
    if (d->scrollY < 0) d->scrollY = 0;
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = d->contentHeight; si.nPage = (UINT)H; si.nPos = d->scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static void ScrollBy(HWND hwnd, PreviewData* d, int delta)
{
    int old = d->scrollY;
    d->scrollY += delta;
    SyncScrollbar(hwnd, d);
    if (d->scrollY != old) InvalidateRect(hwnd, NULL, FALSE);
}

static int WheelStep(HWND hwnd)
{
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    return MulDiv(48, dpi, 96);
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
            RECT cr; GetClientRect(hwnd, &cr);
            int w = cr.right - cr.left, h = cr.bottom - cr.top;
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d && w > 0 && h > 0)
            {
                // 双缓冲：先画到离屏位图再一次性上屏，消除悬停/滚动时的闪烁。
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
                HBITMAP oldb = (HBITMAP)SelectObject(mem, bmp);
                RenderPreview(mem, cr, *d);
                BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, oldb);
                DeleteObject(bmp);
                DeleteDC(mem);
                SyncScrollbar(hwnd, d);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONUP:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d && d->hasButtons && d->valid)
            {
                POINT pt = { (int)(short)LOWORD(l), (int)(short)HIWORD(l) };
                if (PtInRect(&d->btnCatalog, pt))
                    LaunchImport(kCatalogScheme, PackUrl(*d));
                else if (PtInRect(&d->btnLauncher, pt))
                    LaunchImport(kLauncherScheme, PackUrl(*d));
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d)
            {
                int z = GET_WHEEL_DELTA_WPARAM(w);
                ScrollBy(hwnd, d, -(z / WHEEL_DELTA) * WheelStep(hwnd));
            }
            return 0;
        }
        case WM_VSCROLL:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d)
            {
                RECT cr; GetClientRect(hwnd, &cr);
                int H = cr.bottom - cr.top;
                int page = H > 0 ? H : 1;
                int step = WheelStep(hwnd);
                switch (LOWORD(w))
                {
                case SB_LINEUP:   ScrollBy(hwnd, d, -step); break;
                case SB_LINEDOWN: ScrollBy(hwnd, d,  step); break;
                case SB_PAGEUP:   ScrollBy(hwnd, d, -page); break;
                case SB_PAGEDOWN: ScrollBy(hwnd, d,  page); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:
                {
                    SCROLLINFO si = { sizeof(si) };
                    si.fMask = SIF_TRACKPOS;
                    GetScrollInfo(hwnd, SB_VERT, &si);
                    ScrollBy(hwnd, d, si.nTrackPos - d->scrollY);
                    break;
                }
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d && d->hasButtons)
            {
                POINT pt = { (int)(short)LOWORD(l), (int)(short)HIWORD(l) };
                int hv = 0;
                if (PtInRect(&d->btnCatalog, pt)) hv = 1;
                else if (PtInRect(&d->btnLauncher, pt)) hv = 2;
                if (hv != d->hoverBtn)
                {
                    d->hoverBtn = hv;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                TRACKMOUSEEVENT tme = { sizeof(tme) };
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d && d->hoverBtn != 0)
            {
                d->hoverBtn = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d && d->hoverBtn != 0 && LOWORD(l) == HTCLIENT)
            {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            break;
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

class CDspackPreviewHandler : public IPreviewHandler, public IInitializeWithFile, public IInitializeWithStream, public IPreviewHandlerVisuals
{
    volatile LONG m_ref;
    wchar_t* m_file;
    std::vector<BYTE> m_streamBuf;
    std::wstring m_sourcePath;
    HWND m_hwnd;
    HWND m_host;
    RECT m_rect;
    COLORREF m_bgColor;
public:
    CDspackPreviewHandler() : m_ref(1), m_file(0), m_hwnd(0), m_host(0), m_bgColor(RGB(255, 255, 255))
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
        else if (IsEqualGUID(riid, IID_IPreviewHandlerVisuals))
        { *ppv = static_cast<IPreviewHandlerVisuals*>(this); hr = S_OK; }
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
        m_sourcePath = pszFilePath;
        return S_OK;
    }

    // IInitializeWithStream
    STDMETHODIMP Initialize(IStream* pstream, DWORD grfMode)
    {
        Log("Initialize(stream)");
        m_streamBuf.clear();
        m_sourcePath.clear();
        if (pstream)
        {
            ReadStreamToVec(pstream, m_streamBuf);
            // 文件流会带名字(完整路径);内存流没有。
            STATSTG st; ZeroMemory(&st, sizeof(st));
            if (SUCCEEDED(pstream->Stat(&st, STATFLAG_DEFAULT)))
            {
                if (st.pwcsName) { m_sourcePath = st.pwcsName; CoTaskMemFree(st.pwcsName); }
            }
        }
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
        d->sourcePath = m_sourcePath;
        d->dark = IsDarkColor(m_bgColor);
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

    // IPreviewHandlerVisuals（深色模式通知）
    STDMETHODIMP SetBackgroundColor(COLORREF color)
    {
        LogF("SetBackgroundColor 0x%06X", (unsigned)color);
        m_bgColor = color;
        if (m_host)
        {
            PreviewData* d = (PreviewData*)GetWindowLongPtrW(m_host, GWLP_USERDATA);
            if (d) { d->dark = IsDarkColor(color); InvalidateRect(m_host, NULL, TRUE); }
        }
        return S_OK;
    }
    STDMETHODIMP SetFont(const LOGFONTW* plf) { (void)plf; return S_OK; }
    STDMETHODIMP SetTextColor(COLORREF color) { (void)color; return S_OK; }
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