# dspack-preview

`.dspack` 整合包的 **Windows 资源管理器预览处理器（Preview Handler）**。

选中 `.dspack` 文件后，在资源管理器「预览窗格」（`Alt+P`）里直接展示整合包摘要（名称 / 版本 / 类型 / 描述 / 依赖计数），无需解包、无需打开市场页。

> 状态：**C# 骨架已验证透彻，但确认无法用于生产——见下方「实测结论」。下一步改用原生 C++ 重写。**
> 格式契约见 [DSH-PackForge](https://github.com/DSH-PackForge/DSH-PackForge) 的 `specs/pack-structure/v2.md` 与 `specs/manifest/v4.md`。

## 实测结论（2026-09）

C# 代码链路全部验证通过：

- ✅ 编译（`net472`）；
- ✅ 读取 `.dspack`：剥 8 字节 `DSPK` 头 → 解 zip → 解析 `manifest.json`（中文 UTF-8 正确、`pickLang` 正确）；
- ✅ 普通进程 COM 创建（`powershell` 与原生 `cscript` 都成功）；
- ✅ 注册表两层键 + `AssocQueryString` 关联解析。

**但无法投入使用的唯一原因：`prevhost.exe`（预览处理器宿主）以「低完整性级别」运行，无法承载 .NET Framework 托管 COM 组件。** 即使把 DLL 装到 `C:\Program Files\` 并做完整 HKLM 注册，`prevhost` 也从未执行到构造函数——这是「不要用托管代码写 shell 扩展」这条准则的根源。

因此本仓库 C# 版仅作参考留存，预览处理器将改用**原生 C++（ATL）**重写（见 [DSH-PackForge 开发备忘](../DSH-PackForge/notes/windows-preview-handler.md)）。

## 原理

Windows 按扩展名查注册表 `HKLM\Software\Classes\.dspack\shellex\{8895b1c6-b41f-4c1c-a562-0d564250836f}`，加载实现 `IPreviewHandler` 的 COM 对象；预览处理器运行在独立宿主 **`prevhost.exe`**。

`.dspack` = 8 字节 `DSPK` 头 + 标准 ZIP（内含 `manifest.json`）。读取时**先剥 8 字节**，再把余下字节交给 zip 库。

## 环境要求

- Windows 10 / 11（64 位）
- Visual Studio 2022（或 `msbuild`）
- .NET Framework 4.7.2（C# 版）；后续 C++ 版改用 ATL / Win32

## 构建（C# 版）

```bat
msbuild src\DspackPreview\DspackPreview.csproj /p:Configuration=Release /p:Platform=AnyCPU
```

## 注册 / 卸载（C# 版，参考）

用 `register-admin.cmd`（右键「以管理员身份运行」）：把 DLL 装到 `C:\Program Files\DSH-PackForge\dspack-preview\` 并做 HKLM 注册。

> 实测要点：预览处理器必须**机器级 HKLM** 注册（每用户 `HKCU` 不被 `prevhost` 采信）；`regasm /codebase` 单独跑会漏掉 `.dspack\shellex` 映射键，因此脚本用显式 `reg add` 完整写两层键。

调试时刷新宿主缓存：`taskkill /f /im prevhost.exe`（或重启资源管理器）。

## 目录

```
src/DspackPreview/
├── DspackPreview.csproj      # net472 类库（COM 可见）
├── Properties/AssemblyInfo.cs
├── Interop.cs                # COM 接口/结构声明（IPreviewHandler 等）
├── ManifestReader.cs         # 剥 DSPK 头 + 读 manifest.json → 摘要
├── PreviewForm.cs            # 预览卡片窗口（WinForms）
└── DspackPreviewHandler.cs   # 处理器入口（COM 类 + 注册 + 日志）
register-admin.cmd            # 管理员注册脚本（HKLM + Program Files）
test/
├── make-test-pack.ps1        # 生成 test-preview-1.0.0.dspack
├── fixtures/                 # manifest.json / package.json / overrides/
├── smoke-test.cs             # 剥离+解析冒烟测试（验证读取链路）
├── register-test.ps1         # 验证 COM 可创建
└── com-test.vbs              # 用原生 cscript 验证 COM 创建
```

## TODO / 路线

- [ ] **用原生 C++（ATL）重写**（C# 托管版在 prevhost 不可用，见「实测结论」）。
- [ ] 流式读取（避免整包 `ReadAllBytes`），大文件自动跳过。
- [ ] 丰富卡片排版，或用 WebView2 渲染 HTML。
- [ ] `displayName/description` 多语言 `pickLang` 逻辑对齐市场页。
- [ ] 异常兜底（zip 解析失败显示「无效 .dspack」，而非空窗格）。
- [ ] 深色模式（`IPreviewHandlerVisuals`）。
- [ ] 补缩略图 `IThumbnailProvider`（本仓库当前只做预览）。
- [ ] CI 构建 + 发布发布物。

## License

[MIT](LICENSE) © 2026 DSH-PackForge