using System;
using System.Text;

/// <summary>
/// 冒烟测试：直接调用 DspackPreview.ManifestReader.Read() 解析 .dspack，
/// 打印摘要字段（只输出 ASCII，中文用码点校验，避免控制台编码干扰）。
/// 用法： smoke-test.exe path\to\xxx.dspack
/// </summary>
class SmokeTest
{
    static int Main(string[] args)
    {
        try
        {
            Console.OutputEncoding = Encoding.UTF8;
            if (args.Length == 0) { Console.WriteLine("usage: smoke-test.exe <file.dspack>"); return 2; }

            var info = DspackPreview.ManifestReader.Read(args[0]);

            Console.WriteLine("name          = " + info.Name);
            Console.WriteLine("version       = " + info.Version);
            Console.WriteLine("type          = " + info.Type);
            Console.WriteLine("author        = " + info.Author);
            Console.WriteLine("dshVersion    = " + info.DshVersion);
            Console.WriteLine("bundles       = " + info.Bundles);
            Console.WriteLine("dependencies  = " + info.Dependencies);
            Console.WriteLine("files         = " + info.Files);
            Console.WriteLine("displayName   = " + info.DisplayName); // 应为 en-US 的 "Test Preview Pack"

            // 校验原始 JSON 里中文是"测试预览"而非乱码（码点比对，ASCII 输出）
            var zh = new string(new[] { (char)0x6D4B, (char)0x8BD5, (char)0x9884, (char)0x89C8 });
            Console.WriteLine("rawJson_zh_ok = " + (info.RawJson != null && info.RawJson.Contains(zh)));
            Console.WriteLine("description   = non-null: " + (info.Description != null));

            Console.WriteLine("SMOKE OK");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine("SMOKE FAIL: " + ex.GetType().Name + ": " + ex.Message);
            return 1;
        }
    }
}