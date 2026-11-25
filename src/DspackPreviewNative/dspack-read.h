#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct ManifestInfo {
    std::wstring name, version, type, author, dshVersion;
    std::wstring displayName, description;
    std::wstring profileName, icon, patch;
    int manifestVersion = 0;
    int bundles = 0, dependencies = 0, files = 0;
    // dshhome 形态（type == "dshhome"，manifest v5）
    std::wstring defaultProfile, instructions;
    int profiles = 0, presets = 0, skills = 0;
    std::vector<std::pair<std::wstring, std::wstring>> details;
    bool ok = false;
    std::wstring error;
};

// 容器标记 dspack.json 保存独立于清单的结构版本。
struct ContainerInfo {
    int version = 0;    // version 字段
    bool valid = false; // format == "dspack"
};

// 预览器读取归档和解压单个元数据条目的大小预算。
constexpr size_t kPreviewByteLimit = 64u << 20;
enum class ZipReadStatus { Ok, Missing, Invalid, TooLarge };
ZipReadStatus ReadZipEntry(const void* zip, size_t zipSize, const char* name, std::vector<BYTE>& out);

// 解析 dspack.json（UTF-8）为容器标记信息。
bool ParseContainerJson(const BYTE* data, size_t len, ContainerInfo& out);

// 解析 manifest.json（UTF-8）为摘要字段。
bool ParseManifest(const BYTE* data, size_t len, ManifestInfo& info);

// 读取归档清单，v6 技能数量取自可选 skill.json 中的实际条目。
bool ReadPackManifest(const void* zip, size_t zipSize, ManifestInfo& info);
