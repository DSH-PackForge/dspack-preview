// render-test.cpp - in-process runtime test: create handler, DoPreview, force WM_PAINT.
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <stdio.h>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: render-test <file.dspack>\n"); return 2; }

    CoInitialize(NULL);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"RenderTestHost";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"RenderTestHost", L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        -2000, -2000, 520, 420, NULL, NULL, GetModuleHandleA(NULL), NULL);
    printf("host hwnd = %p\n", host);

    CLSID clsid = { 0x7f3c5a1e,0x2b4d,0x4e6a,{ 0x9c,0x8b,0x1d,0x5f,0x7a,0x3e,0x9c,0x2d } };
    IPreviewHandler* ph = NULL;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IPreviewHandler, (void**)&ph);
    printf("CoCreateInstance(IPreviewHandler) = 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { DestroyWindow(host); CoUninitialize(); return 1; }

    IInitializeWithFile* init = NULL;
    hr = ph->QueryInterface(IID_IInitializeWithFile, (void**)&init);
    printf("QI(IInitializeWithFile) = 0x%08X\n", (unsigned)hr);

    RECT r = { 0, 0, 500, 400 };
    if (SUCCEEDED(hr)) {
        ph->SetWindow(host, &r);
        wchar_t wf[1024];
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wf, 1024);
        hr = init->Initialize(wf, STGM_READ);
        printf("Initialize = 0x%08X\n", (unsigned)hr);
    }
    else ph->SetWindow(host, &r);

    hr = ph->DoPreview();
    printf("DoPreview = 0x%08X\n", (unsigned)hr);

    // force a paint synchronously (child was invalidated in DoPreview)
    UpdateWindow(host);
    MSG m; int n = 0;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE) && n < 200) { TranslateMessage(&m); DispatchMessageW(&m); ++n; }
    Sleep(50);
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE) && n < 400) { TranslateMessage(&m); DispatchMessageW(&m); ++n; }
    printf("pumped %d messages\n", n);

    // capture the preview child window to a BMP for visual verification
    HWND child = FindWindowExW(host, NULL, L"DspackPreview.Host", NULL);
    if (child)
    {
        InvalidateRect(child, NULL, TRUE);
        UpdateWindow(child); // force synchronous WM_PAINT on the preview card
        RECT crc; GetClientRect(child, &crc);
        int w = crc.right > 0 ? crc.right : 500;
        int h = crc.bottom > 0 ? crc.bottom : 400;
        HDC cdc = GetDC(child);
        HDC mdc = CreateCompatibleDC(cdc);
        HBITMAP bmp = CreateCompatibleBitmap(cdc, w, h);
        HBITMAP oldb = (HBITMAP)SelectObject(mdc, bmp);
        RECT fc = { 0, 0, w, h };
        HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255)); FillRect(mdc, &fc, wb); DeleteObject(wb);
        SendMessageW(child, WM_PRINT, (WPARAM)mdc, PRF_CLIENT);
        SelectObject(mdc, oldb);
        BITMAPINFOHEADER bi; ZeroMemory(&bi, sizeof(bi));
        bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
        int rowbytes = ((w * 32 + 31) / 32) * 4;
        BITMAPFILEHEADER bf; ZeroMemory(&bf, sizeof(bf));
        bf.bfType = 0x4D42;
        bf.bfSize = sizeof(bf) + sizeof(bi) + rowbytes * h;
        bf.bfOffBits = sizeof(bf) + sizeof(bi);
        std::vector<char> pixels((size_t)rowbytes * h);
        GetDIBits(mdc, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        FILE* f = 0; fopen_s(&f, "render-capture.bmp", "wb");
        if (f) { fwrite(&bf, 1, sizeof(bf), f); fwrite(&bi, 1, sizeof(bi), f); fwrite(pixels.data(), 1, pixels.size(), f); fclose(f); printf("saved render-capture.bmp (%dx%d)\n", w, h); }
        DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(child, cdc);
    }

    ph->Unload();
    if (init) init->Release();
    ph->Release();
    DestroyWindow(host);
    CoUninitialize();
    printf("RENDER OK (no crash)\n");
    return 0;
}