// capture-test.cpp - load the freshly-built DLL by path and capture the rendered
// preview card to BMP (light + dark), so the visual result can be reviewed.
// usage: capture-test.exe <DspackPreviewNative.dll> <file.dspack>
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <stdio.h>
#include <vector>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND MakeHost(int w, int h)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"CaptureTestHost";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, L"CaptureTestHost", L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        -2000, -2000, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void SaveBmp(HWND child, const char* path)
{
    RECT crc; GetClientRect(child, &crc);
    int w = crc.right > 0 ? crc.right : 600;
    int h = crc.bottom > 0 ? crc.bottom : 760;
    HDC cdc = GetDC(child);
    HDC mdc = CreateCompatibleDC(cdc);
    HBITMAP bmp = CreateCompatibleBitmap(cdc, w, h);
    HBITMAP ob = (HBITMAP)SelectObject(mdc, bmp);
    RECT fc = { 0, 0, w, h };
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255)); FillRect(mdc, &fc, wb); DeleteObject(wb);
    SendMessageW(child, WM_PRINT, (WPARAM)mdc, PRF_CLIENT);
    SelectObject(mdc, ob);

    BITMAPINFOHEADER bi; ZeroMemory(&bi, sizeof(bi));
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    int rowbytes = ((w * 32 + 31) / 32) * 4;
    BITMAPFILEHEADER bf; ZeroMemory(&bf, sizeof(bf));
    bf.bfType = 0x4D42;
    bf.bfSize = sizeof(bf) + sizeof(bi) + rowbytes * h;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    std::vector<char> px((size_t)rowbytes * h);
    GetDIBits(mdc, bmp, 0, h, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    FILE* f = 0; fopen_s(&f, path, "wb");
    if (f) { fwrite(&bf, 1, sizeof(bf), f); fwrite(&bi, 1, sizeof(bi), f); fwrite(px.data(), 1, px.size(), f); fclose(f); printf("saved %s (%dx%d)\n", path, w, h); }
    else printf("save failed: %s\n", path);
    DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(child, cdc);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { printf("usage: capture-test <dll> <file.dspack>\n"); return 2; }
    CoInitialize(NULL);

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { printf("LoadLibrary failed %u\n", GetLastError()); return 1; }
    typedef HRESULT(__stdcall* PFN)(REFCLSID, REFIID, void**);
    PFN DllGetClassObject = (PFN)GetProcAddress(dll, "DllGetClassObject");
    if (!DllGetClassObject) { printf("DllGetClassObject missing\n"); return 1; }

    CLSID clsid = { 0x7f3c5a1e,0x2b4d,0x4e6a,{ 0x9c,0x8b,0x1d,0x5f,0x7a,0x3e,0x9c,0x2d } };
    IClassFactory* cf = NULL;
    if (FAILED(DllGetClassObject(clsid, IID_IClassFactory, (void**)&cf))) { printf("class object failed\n"); return 1; }
    IPreviewHandler* ph = NULL;
    HRESULT hr = cf->CreateInstance(NULL, IID_IPreviewHandler, (void**)&ph);
    cf->Release();
    printf("CreateInstance = 0x%08X\n", (unsigned)hr);

    IInitializeWithFile* init = NULL;
    ph->QueryInterface(IID_IInitializeWithFile, (void**)&init);

    const int W = 600, H = 780;
    HWND host = MakeHost(W, H);
    RECT r = { 0, 0, W, H };
    ph->SetWindow(host, &r);
    if (init) init->Initialize(argv[2], STGM_READ);
    hr = ph->DoPreview();
    printf("DoPreview = 0x%08X\n", (unsigned)hr);

    HWND child = FindWindowExW(host, NULL, L"DspackPreview.Host", NULL);
    printf("child = %p\n", child);
    if (child) SaveBmp(child, "capture-light.bmp");

    // dark mode via IPreviewHandlerVisuals
    IPreviewHandlerVisuals* vis = NULL;
    if (SUCCEEDED(ph->QueryInterface(IID_IPreviewHandlerVisuals, (void**)&vis)) && vis)
    {
        vis->SetBackgroundColor(RGB(30, 32, 40));
        printf("SetBackgroundColor(dark) ok\n");
        if (child) SaveBmp(child, "capture-dark.bmp");
        vis->Release();
    }

    ph->Unload();
    if (init) init->Release();
    ph->Release();
    FreeLibrary(dll);
    DestroyWindow(host);
    CoUninitialize();
    printf("CAPTURE OK (no crash)\n");
    return 0;
}
