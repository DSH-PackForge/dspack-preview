#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct ManifestInfo {
    std::wstring name, version, type, author, dshVersion;
    std::wstring displayName, description;
    std::wstring profileName, icon, patch;
    int manifestVersion = 0;        // manifest.json 的 manifestVersion 字段（4 / 5）
    int bundles = 0, dependencies = 0, files = 0;
    // dshhome 形态（type == "dshhome"，manifest v5）
    std::wstring defaultProfile, instructions;
    int profiles = 0, presets = 0, skills = 0;
    bool ok = false;
    std::wstring error;
};

// 容器标记 dspack.json（pack-structure v2/v3）：{ "format":"dspack", "version":2|3 }
struct ContainerInfo {
    int version = 0;    // version 字段
    bool valid = false; // format == "dspack"
};

// 读取 zip 内某个条目（zip 指向纯 ZIP 缓冲区；.dspack 即标准 ZIP）。
// 支持 method 0（stored）与 method 8（deflate，经内置 miniz）。
bool ReadZipEntry(const void* zip, size_t zipSize, const char* name, std::vector<BYTE>& out);

// 解析 dspack.json（UTF-8）为容器标记信息。
bool ParseContainerJson(const BYTE* data, size_t len, ContainerInfo& out);

// 解析 manifest.json（UTF-8）为摘要字段。
bool ParseManifest(const BYTE* data, size_t len, ManifestInfo& info);

// 读取 zip 内某个条目（zip 指向纯 ZIP 缓冲区；.dspack 即标准 ZIP）。
// 支持 method 0（stored）与 method 8（deflate，经内置 miniz）。
bool ReadZipEntry(const void* zip, size_t zipSize, const char* name, std::vector<BYTE>& out);

// 解析 manifest.json（UTF-8）为摘要字段。
bool ParseManifest(const BYTE* data, size_t len, ManifestInfo& info);