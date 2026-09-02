// dspack-read.cpp - ZIP reading (miniz) + JSON parser + manifest extraction.
#include "dspack-read.h"
#include "third_party/miniz/miniz.h"
#include <cstring>
#include <cmath>

// ---------- ZIP reader (miniz; supports stored + deflate + zip64) ----------
bool ReadZipEntry(const void* zip, size_t zipSize, const char* name, std::vector<BYTE>& out)
{
    mz_zip_archive arc;
    memset(&arc, 0, sizeof(arc));
    if (!mz_zip_reader_init_mem(&arc, zip, zipSize, 0)) return false;

    size_t outSize = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&arc, name, &outSize, 0);
    mz_zip_reader_end(&arc);
    if (!p) return false;

    out.assign((const BYTE*)p, (const BYTE*)p + outSize);
    mz_free(p);
    return true;
}

// ---------- minimal JSON parser (UTF-8 in -> UTF-16 out) ----------
namespace
{
struct JNode {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double num = 0;
    std::wstring str;
    std::vector<std::pair<std::wstring, JNode>> obj;
    std::vector<JNode> arr;
};

void utf8Encode(unsigned cp, std::string& out)
{
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
}

std::wstring toWide(const std::string& utf8)
{
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    return w;
}

struct Parser {
    const char* p;
    const char* end;
    Parser(const BYTE* d, size_t n) : p((const char*)d), end((const char*)d + n) {}

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    bool hex4(unsigned& v)
    {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool string(std::wstring& out)
    {
        ws();
        if (p >= end || *p != '"') return false;
        ++p;
        std::string raw;
        while (p < end && *p != '"')
        {
            unsigned char c = (unsigned char)*p;
            if (c == '\\')
            {
                ++p;
                if (p >= end) return false;
                char e = *p++;
                switch (e)
                {
                case '"': raw += '"'; break;
                case '\\': raw += '\\'; break;
                case '/': raw += '/'; break;
                case 'n': raw += '\n'; break;
                case 't': raw += '\t'; break;
                case 'r': raw += '\r'; break;
                case 'b': raw += '\b'; break;
                case 'f': raw += '\f'; break;
                case 'u': {
                    unsigned u = 0;
                    if (!hex4(u)) return false;
                    if (u >= 0xD800 && u <= 0xDBFF) // high surrogate
                    {
                        if (end - p >= 2 && p[0] == '\\' && p[1] == 'u')
                        {
                            p += 2; unsigned l = 0;
                            if (!hex4(l)) return false;
                            if (l >= 0xDC00 && l <= 0xDFFF)
                                u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
                        }
                    }
                    utf8Encode(u, raw);
                    break;
                }
                default: return false;
                }
            }
            else
            {
                raw += (char)c;
                ++p;
            }
        }
        if (p >= end) return false;
        ++p; // closing quote
        out = toWide(raw);
        return true;
    }

    bool value(JNode& v)
    {
        ws();
        if (p >= end) return false;
        char c = *p;
        if (c == '{')
        {
            v.t = JNode::Obj;
            ++p; ws();
            if (p < end && *p == '}') { ++p; return true; }
            while (true)
            {
                std::wstring k;
                if (!string(k)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                ++p;
                JNode child;
                if (!value(child)) return false;
                v.obj.push_back(std::make_pair(k, std::move(child)));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[')
        {
            v.t = JNode::Arr;
            ++p; ws();
            if (p < end && *p == ']') { ++p; return true; }
            while (true)
            {
                JNode child;
                if (!value(child)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JNode::Str; return string(v.str); }
        if (c == 't' && end - p >= 4 && memcmp(p, "true", 4) == 0) { p += 4; v.t = JNode::Bool; v.b = true; return true; }
        if (c == 'f' && end - p >= 5 && memcmp(p, "false", 5) == 0) { p += 5; v.t = JNode::Bool; v.b = false; return true; }
        if (c == 'n' && end - p >= 4 && memcmp(p, "null", 4) == 0) { p += 4; v.t = JNode::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            char* ep = 0;
            v.num = strtod(p, &ep);
            if (ep == p) return false;
            p = ep;
            v.t = JNode::Num;
            return true;
        }
        return false;
    }
};

const JNode* Get(const JNode& o, const wchar_t* key)
{
    if (o.t != JNode::Obj) return nullptr;
    for (auto& kv : o.obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

bool AsString(const JNode* n, std::wstring& out)
{
    if (n && n->t == JNode::Str) { out = n->str; return true; }
    return false;
}

bool AsNumber(const JNode* n, int& out)
{
    if (n && n->t == JNode::Num) { out = (int)n->num; return true; }
    return false;
}

// pickLang: en-US -> zh-CN -> first
bool GetLocalized(const JNode& o, const wchar_t* key, std::wstring& out)
{
    const JNode* n = Get(o, key);
    if (!n) return false;
    if (n->t == JNode::Str) { out = n->str; return true; }
    if (n->t == JNode::Obj)
    {
        if (AsString(Get(*n, L"en-US"), out)) return true;
        if (AsString(Get(*n, L"zh-CN"), out)) return true;
        if (!n->obj.empty()) return AsString(&n->obj[0].second, out);
    }
    return false;
}

int Count(const JNode& o, const wchar_t* key, JNode::T want)
{
    const JNode* n = Get(o, key);
    if (!n || n->t != want) return 0;
    return (int)(want == JNode::Arr ? n->arr.size() : n->obj.size());
}
} // namespace

bool ParseContainerJson(const BYTE* data, size_t len, ContainerInfo& out)
{
    Parser parser(data, len);
    JNode root;
    if (!parser.value(root) || root.t != JNode::Obj) return false;
    std::wstring format;
    if (!AsString(Get(root, L"format"), format) || format != L"dspack") return false;
    AsNumber(Get(root, L"version"), out.version);
    out.valid = true;
    return true;
}

bool ParseManifest(const BYTE* data, size_t len, ManifestInfo& info)
{
    Parser parser(data, len);
    JNode root;
    if (!parser.value(root) || root.t != JNode::Obj)
    {
        info.ok = false;
        info.error = L"not a JSON object";
        return false;
    }
    info.ok = true;
    AsNumber(Get(root, L"manifestVersion"), info.manifestVersion);
    AsString(Get(root, L"name"), info.name);
    AsString(Get(root, L"version"), info.version);
    AsString(Get(root, L"type"), info.type);
    AsString(Get(root, L"author"), info.author);
    AsString(Get(root, L"dshVersion"), info.dshVersion);
    AsString(Get(root, L"profileName"), info.profileName);
    AsString(Get(root, L"icon"), info.icon);
    AsString(Get(root, L"patch"), info.patch);
    GetLocalized(root, L"displayName", info.displayName);
    GetLocalized(root, L"description", info.description);
    info.bundles = Count(root, L"bundles", JNode::Arr);
    info.dependencies = Count(root, L"dependencies", JNode::Obj);
    info.files = Count(root, L"files", JNode::Arr);
    // dshhome 形态（manifest v5）
    AsString(Get(root, L"defaultProfile"), info.defaultProfile);
    AsString(Get(root, L"instructions"), info.instructions);
    info.profiles = Count(root, L"profiles", JNode::Obj);
    info.presets = Count(root, L"presets", JNode::Obj);
    info.skills = Count(root, L"skills", JNode::Arr);
    return true;
}