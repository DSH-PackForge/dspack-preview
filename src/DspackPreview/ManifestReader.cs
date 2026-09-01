using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Web.Script.Serialization;

namespace DspackPreview
{
    /// <summary>从 .dspack 里读出的 manifest 摘要，用于预览卡片。</summary>
    public sealed class ManifestInfo
    {
        public string DisplayName;
        public string Description;
        public string Name;
        public string Version;
        public string Type = "profile";
        public string Author;
        public string DshVersion;
        public int Bundles;
        public int Dependencies;
        public int Files;
        public string RawJson;

        public static ManifestInfo FromJson(string json)
        {
            var info = new ManifestInfo { RawJson = json };
            try
            {
                var root = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(json);
                if (root == null) return info;

                info.Name = GetString(root, "name");
                info.Version = GetString(root, "version");
                info.Type = GetString(root, "type") ?? "profile";
                info.Author = GetString(root, "author");
                info.DshVersion = GetString(root, "dshVersion");
                info.DisplayName = PickLang(root, "displayName");
                info.Description = PickLang(root, "description");

                info.Bundles      = GetCount(root, "bundles");
                info.Dependencies = GetCount(root, "dependencies");
                info.Files        = GetCount(root, "files");
            }
            catch
            {
                // 解析失败时保留 RawJson，由 UI 直接展示原文
            }
            return info;
        }

        private static string GetString(Dictionary<string, object> root, string key)
        {
            return root.TryGetValue(key, out var v) ? v as string : null;
        }

        /// <summary>JavaScriptSerializer 把 JSON 数组反序列化为 ArrayList、对象为 Dictionary，统一按 ICollection 取 Count。</summary>
        private static int GetCount(Dictionary<string, object> root, string key)
        {
            return root.TryGetValue(key, out var v) && v is System.Collections.ICollection c ? c.Count : 0;
        }

        /// <summary>displayName/description 可能是字符串，也可能是多语言 map；优先 en-US → zh-CN → 首个值。</summary>
        private static string PickLang(Dictionary<string, object> root, string key)
        {
            if (!root.TryGetValue(key, out var v) || v == null) return null;
            if (v is string s) return s;
            if (v is Dictionary<string, object> map)
            {
                if (map.TryGetValue("en-US", out var en) && en is string enS) return enS;
                if (map.TryGetValue("zh-CN", out var zh) && zh is string zhS) return zhS;
                foreach (var kv in map) if (kv.Value is string kvS) return kvS;
            }
            return null;
        }
    }

    /// <summary>剥掉 8 字节 DSPK 头，把剩余字节当 ZIP 读 manifest.json。</summary>
    public static class ManifestReader
    {
        private const long MaxPreviewBytes = 32L * 1024 * 1024; // 32 MB，超过就跳过，避免预览卡顿

        public static ManifestInfo Read(string path)
        {
            if (new FileInfo(path).Length > MaxPreviewBytes)
                throw new InvalidOperationException("文件过大，跳过预览（> 32 MB）");

            var all = File.ReadAllBytes(path);
            if (all.Length < 8 || all[0] != (byte)'D' || all[1] != (byte)'S' || all[2] != (byte)'P' || all[3] != (byte)'K')
                throw new InvalidDataException("不是有效的 .dspack（缺少 DSPK 魔术字节）");

            var version = BitConverter.ToUInt32(all, 4);
            if (version != 2)
                throw new NotSupportedException("不支持的 .dspack 版本: " + version);

            using (var ms = new MemoryStream(all, 8, all.Length - 8))
            using (var zip = new ZipArchive(ms, ZipArchiveMode.Read))
            {
                var entry = zip.GetEntry("manifest.json");
                if (entry == null)
                    throw new InvalidDataException("归档内缺少 manifest.json");

                using (var reader = new StreamReader(entry.Open(), Encoding.UTF8))
                {
                    return ManifestInfo.FromJson(reader.ReadToEnd());
                }
            }
        }
    }
}