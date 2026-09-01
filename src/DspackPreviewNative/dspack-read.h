#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct ManifestInfo {
    std::wstring name, version, type, author, dshVersion;
    std::wstring displayName, description;
    int bundles = 0, dependencies = 0, files = 0;
    bool ok = false;
    std::wstring error;
};

// 读取 zip 内某个条目（zip 指向纯 ZIP 缓冲区；.dspack 即标准 ZIP）。
// 支持 method 0（stored）与 method 8（deflate，经内置 miniz）。
bool ReadZipEntry(const void* zip, size_t zipSize, const char* name, std::vector<BYTE>& out);

// 解析 manifest.json（UTF-8）为摘要字段。
bool ParseManifest(const BYTE* data, size_t len, ManifestInfo& info);