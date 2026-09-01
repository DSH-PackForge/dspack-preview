#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#pragma comment(lib, "shlwapi.lib")

int wmain()
{
    for (int mode = 0; mode < 2; ++mode)
    {
        wchar_t buf[512] = {0};
        DWORD len = (DWORD)(sizeof(buf) / sizeof(wchar_t));
        // mode 0: 查扩展名本身的 shellex；mode 1: 先带上 ProgId(模拟 shell 完整解析)
        const wchar_t* assoc = (mode == 0) ? L".dspack" : L"DspackPreview.PreviewHandler";
        HRESULT hr = AssocQueryStringW(0, ASSOCSTR_SHELLEXTENSION, assoc,
            L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", buf, &len);
        wprintf(L"[mode %d] assoc='%s'  hr=0x%08X  len=%u  value='%s'\n",
            mode, assoc, (unsigned)hr, len, buf);
    }
    return 0;
}