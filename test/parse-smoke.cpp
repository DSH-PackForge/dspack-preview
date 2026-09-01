// parse-smoke.cpp - console smoke test for the native .dspack reader.
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>
#include "../src/DspackPreviewNative/dspack-read.h"

static void PrintW(const char* label, const std::wstring& w)
{
    // print wide label->UTF8 for unambiguous ASCII output
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    printf("%s = %s\n", label, s.c_str());
}

static void PrintCp(const char* label, const std::wstring& w)
{
    printf("%s_cp =", label);
    for (wchar_t c : w) printf(" 0x%04X", (unsigned)c);
    printf("\n");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { printf("usage: parse-smoke <file.dspack>\n"); return 2; }

    HANDLE h = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) { printf("ERROR: cannot open file\n"); return 2; }
    DWORD size = GetFileSize(h, 0);
    std::vector<BYTE> buf(size);
    DWORD rd = 0;
    bool okRead = ReadFile(h, buf.data(), size, &rd, 0) != 0;
    CloseHandle(h);

    if (!okRead || rd < 4) { printf("ERROR: read failed / too small\n"); return 2; }

    const BYTE* zip = buf.data();
    size_t zipSize = rd;
    if (rd >= 8 && memcmp(buf.data(), "DSPK", 4) == 0)
    {
        uint32_t ver = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
        printf("legacy DSPK header version = %u (skipping 8 bytes)\n", ver);
        zip = buf.data() + 8;
        zipSize = rd - 8;
    }

    std::vector<BYTE> entry;
    if (!ReadZipEntry(zip, zipSize, "manifest.json", entry))
    {
        printf("ERROR: manifest.json not found in zip\n");
        return 2;
    }
    printf("manifest.json size = %u\n", (unsigned)entry.size());

    ManifestInfo info;
    if (!ParseManifest(entry.data(), entry.size(), info))
    {
        printf("ERROR: manifest parse failed\n");
        return 2;
    }

    PrintW("name", info.name);
    PrintW("version", info.version);
    PrintW("type", info.type);
    PrintW("author", info.author);
    PrintW("dshVersion", info.dshVersion);
    printf("bundles = %d\n", info.bundles);
    printf("dependencies = %d\n", info.dependencies);
    printf("files = %d\n", info.files);
    PrintW("displayName", info.displayName);
    PrintCp("displayName", info.displayName);
    PrintCp("description", info.description);

    bool ok = info.ok
        && info.name == L"test-preview-pack"
        && info.version == L"1.0.0"
        && info.type == L"profile"
        && info.author == L"hxh230802"
        && info.dshVersion == L"0.1.1-rc.2"
        && info.bundles == 3
        && info.dependencies == 2
        && info.files == 1
        && info.displayName == L"Test Preview Pack"
        && !info.description.empty();

    if (ok) { printf("SMOKE OK\n"); return 0; }
    printf("SMOKE FAIL\n");
    return 1;
}