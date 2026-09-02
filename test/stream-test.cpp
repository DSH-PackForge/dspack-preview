// stream-test.cpp - verify IInitializeWithStream path loads + parses + renders.
// usage: stream-test.exe <DspackPreviewNative.dll> <file.dspack>
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <vector>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { printf("usage: stream-test <dll> <file.dspack>\n"); return 2; }
    CoInitialize(NULL);

    HMODULE dll = LoadLibraryW(argv[1]);
    printf("LoadLibrary = %p\n", dll);
    if (!dll) { printf("load failed %u\n", GetLastError()); return 1; }
    typedef HRESULT(__stdcall* PFN)(REFCLSID, REFIID, void**);
    PFN DllGetClassObject = (PFN)GetProcAddress(dll, "DllGetClassObject");
    if (!DllGetClassObject) { printf("DllGetClassObject missing\n"); return 1; }

    CLSID clsid = { 0x7f3c5a1e,0x2b4d,0x4e6a,{ 0x9c,0x8b,0x1d,0x5f,0x7a,0x3e,0x9c,0x2d } };
    IClassFactory* cf = NULL;
    HRESULT hr = DllGetClassObject(clsid, IID_IClassFactory, (void**)&cf);
    printf("DllGetClassObject = 0x%08X\n", (unsigned)hr);
    IPreviewHandler* ph = NULL;
    hr = cf->CreateInstance(NULL, IID_IPreviewHandler, (void**)&ph);
    cf->Release();
    printf("CreateInstance(IPreviewHandler)=0x%08X\n", (unsigned)hr);

    // read dspack into a memory stream
    HANDLE h = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    DWORD sz = GetFileSize(h, 0);
    std::vector<BYTE> buf(sz);
    DWORD rd = 0; ReadFile(h, buf.data(), sz, &rd, 0); CloseHandle(h);
    IStream* stream = SHCreateMemStream(buf.data(), rd);
    printf("SHCreateMemStream = %p (size %u)\n", stream, rd);

    IInitializeWithStream* init = NULL;
    hr = ph->QueryInterface(IID_IInitializeWithStream, (void**)&init);
    printf("QI(IInitializeWithStream)=0x%08X\n", (unsigned)hr);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"StreamTestHost";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"StreamTestHost", L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        -2000, -2000, 520, 420, NULL, NULL, GetModuleHandleW(NULL), NULL);

    RECT r = { 0, 0, 500, 400 };
    if (SUCCEEDED(hr)) { ph->SetWindow(host, &r); hr = init->Initialize(stream, STGM_READ); printf("Initialize(stream)=0x%08X\n", (unsigned)hr); }
    else ph->SetWindow(host, &r);
    hr = ph->DoPreview();
    printf("DoPreview = 0x%08X\n", (unsigned)hr);

    HWND child = FindWindowExW(host, NULL, L"DspackPreview.Host", NULL);
    printf("preview child = %p\n", child);
    int ok = 0;
    if (child)
    {
        RECT crc; GetClientRect(child, &crc);
        int w = crc.right > 0 ? crc.right : 500, hh = crc.bottom > 0 ? crc.bottom : 400;
        HDC cdc = GetDC(child);
        HDC mdc = CreateCompatibleDC(cdc);
        HBITMAP bmp = CreateCompatibleBitmap(cdc, w, hh);
        HBITMAP ob = (HBITMAP)SelectObject(mdc, bmp);
        RECT fc = { 0, 0, w, hh };
        HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255)); FillRect(mdc, &fc, wb); DeleteObject(wb);
        SendMessageW(child, WM_PRINT, (WPARAM)mdc, PRF_CLIENT);
        // 新卡片：头带在卡片顶部中央（含外边距），取样 (250, 26) 应为 indigo 渐变
        COLORREF c = GetPixel(mdc, 250, 26);
        printf("header pixel = R%d G%d B%d\n", GetRValue(c), GetGValue(c), GetBValue(c));
        // indigo header gradient (74,80,168 -> 91,95,190) => parsed + rendered the real card
        ok = (GetRValue(c) >= 40 && GetRValue(c) <= 110 && GetBValue(c) >= 120) ? 1 : 0;
        SelectObject(mdc, ob);
        DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(child, cdc);
    }

    ph->Unload();
    if (init) init->Release();
    ph->Release();
    stream->Release();
    FreeLibrary(dll);
    DestroyWindow(host);
    CoUninitialize();
    printf(ok ? "STREAM OK\n" : "STREAM FAIL (header not indigo => parse failed)\n");
    return ok ? 0 : 1;
}