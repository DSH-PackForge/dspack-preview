# dspack-preview

`.dspack` 整合包的 **Windows 资源管理器预览处理器（Preview Handler）**，原生 C++ 实现。

选中 `.dspack` 文件后，在资源管理器「预览窗格」（`Alt+P`）里直接展示整合包摘要卡片（名称 / 版本 / 类型 / 描述 / 依赖计数），无需解包、无需打开市场页。

> 格式契约见 [DSH-PackForge](https://github.com/DSH-PackForge/DSH-PackForge) 的 `specs/pack-structure/v2.md` 与 `specs/manifest/v4.md`。

## 现状（原生 C++ 版，已实现）

原生 C++ 处理器已完整实现并**在本地验证通过**：

- ✅ 编译（MSVC，纯 Win32/COM，`/MT` 静态 CRT，无 .NET 依赖）；
- ✅ COM 装载（类工厂 + `IPreviewHandler` + `IInitializeWithFile`）；
- ✅ 读取 `.dspack`：剥 8 字节 `DSPK` 头 → 解 zip（内嵌 **miniz**，支持 stored/deflate/zip64）→ 解析 `manifest.json`（UTF-8 中文、`pickLang` 正确）；
- ✅ GDI 渲染摘要卡片（头带 + 统计芯片 + 描述 + 元数据，截图逐像素校验）；
- ✅ 进程内运行时冒烟（`CoCreateInstance` → `DoPreview` → `WM_PAINT`/`WM_PRINT` 渲染，无崩溃）。

## 实测结论（2026-09）

C# 托管版把整条读取链路都验证通了，但**无法用于生产**：`prevhost.exe`（预览处理器宿主）以「低完整性级别」运行，无法承载 .NET Framework 托管 COM——即使装到 `C:\Program Files\` 并完整 HKLM 注册也从未执行到构造函数。这是「不要用托管代码写 shell 扩展」的根源。

因此改为**原生 C++**（纯 Win32 COM，不依赖 ATL/MFC/CLR），托管版仅作参考留存于 `src/DspackPreview/`。

## 原理

Windows 按扩展名查注册表 `HKLM\Software\Classes\.dspack\shellex\{8895b1c6-b41f-4c1c-a562-0d564250836f}`，加载实现 `IPreviewHandler` 的 COM 对象；预览处理器运行在独立宿主 **`prevhost.exe`**。

`.dspack` = 8 字节 `DSPK` 头 + 标准 ZIP（根目录含 `manifest.json`）。读取时**先剥 8 字节**，再把余下字节交给 zip 库（miniz）。

## 环境要求

- Windows 10 / 11（64 位）
- Visual Studio 2022/2026，勾选「使用 C++ 的桌面开发」工作负载（含 MSVC + Windows SDK）

## 构建（原生 C++）

```bat
cd src\DspackPreviewNative
build-native.cmd
```

产出：`DspackPreviewNative.dll`（处理器本体）与 `parse-smoke.exe`（解析冒烟测试）。
依赖的 miniz 已内嵌于 `src\DspackPreviewNative\third_party\miniz\`，无需额外下载。

## 测试

```bat
:: 解析冒烟：剥头 → 解 zip → 解析 manifest → 打印字段
parse-smoke.exe test\test-preview-1.0.0.dspack

:: 运行时渲染冒烟：CoCreateInstance → DoPreview → 强制 WM_PRINT 渲染
cl /nologo /O2 /MT /EHsc test\render-test.cpp /link /OUT:render-test.exe ole32.lib user32.lib uuid.lib gdi32.lib
render-test.exe test\test-preview-1.0.0.dspack
```

## 注册 / 卸载

用 `register-admin.cmd`（右键「以管理员身份运行」）：清掉旧注册 → 把 `DspackPreviewNative.dll` 拷到 `C:\Program Files\DSH-PackForge\dspack-preview\` → `regsvr32` 做 HKLM 注册 → 重启资源管理器。

> 实测要点：
> - 必须**机器级 HKLM** 注册（每用户 `HKCU` 不被 `prevhost` 采信）；
> - 原生 DLL 的 `DllRegisterServer` 会写全套键（`CLSID\...\InprocServer32` + `.dspack\shellex` + ProgId），无需手工 `reg add`；
> - DLL 必须放在 `C:\Program Files\`（低完整性级 `prevhost` 可读）；`ThreadingModel=Both`。

调试时刷新宿主缓存：`taskkill /f /im prevhost.exe`（或重启资源管理器）。
处理器日志：`%USERPROFILE%\AppData\LocalLow\dspack-preview-native.log`（低完整性级可写目录）。

## 目录

```
src/DspackPreview/               # C# 托管版（仅参考留存，不用）
└── ...
src/DspackPreviewNative/         # ★ 原生 C++ 版（生产）
├── dspack-preview-native.cpp    # COM 入口 + IPreviewHandler + GDI 渲染
├── dspack-read.cpp / .h         # zip(miniz) 读取 + JSON 解析 + manifest 提取
├── dspack-preview-native.def    # DLL 导出
├── build-native.cmd             # MSVC 构建脚本
└── third_party/miniz/           # 内嵌 miniz（zip 读取库）
register-admin.cmd               # 管理员注册脚本（HKLM + Program Files）
test/
├── make-test-pack.ps1           # 生成 test-preview-1.0.0.dspack
├── fixtures/                    # manifest.json / package.json / overrides/
├── parse-smoke.cpp              # 解析冒烟测试（原生 C++）
├── render-test.cpp              # 运行时渲染冒烟测试（原生 C++）
├── smoke-test.cs                # C# 版冒烟（参考）
├── register-test.ps1 / com-test.vbs  # COM 创建校验
```

## TODO / 路线

- [x] 原生 C++ 重写（解析 + 渲染 + 本地验证）。
- [ ] 机器级注册后在真实资源管理器预览窗格确认显示（`prevhost` 加载）。
- [ ] 流式读取（避免整包 `ReadAllBytes`），大文件自动跳过。
- [ ] 丰富卡片排版，或用 WebView2 渲染 HTML。
- [ ] `displayName/description` 多语言 `pickLang` 对齐市场页。
- [ ] 深色模式（`IPreviewHandlerVisuals`）。
- [ ] 补缩略图 `IThumbnailProvider`（本仓库当前只做预览）。
- [ ] CI 构建 + 发布发布物。

## License

[MIT](LICENSE) © 2026 DSH-PackForge