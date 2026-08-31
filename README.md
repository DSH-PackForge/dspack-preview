# dspack-preview

`.dspack` 整合包的 **Windows 资源管理器预览处理器（Preview Handler）**，C# 实现。

选中 `.dspack` 文件后，在资源管理器「预览窗格」（`Alt+P`）里直接展示整合包摘要（名称 / 版本 / 类型 / 描述 / 依赖计数），无需解包、无需打开市场页。

> 状态：**骨架（skeleton）**，尚未做成生产可用。格式契约见 [DSH-PackForge](https://github.com/DSH-PackForge/DSH-PackForge) 的 `specs/pack-structure/v2.md` 与 `specs/manifest/v4.md`。

## 原理

Windows 按扩展名查注册表 `HKCU\Software\Classes\.dspack\shellex\{8895b1c6-b41f-4c1c-a562-0d564250836f}`，加载实现 `IPreviewHandler` 的 COM 对象；预览处理器运行在独立宿主 **`prevhost.exe`**（崩溃不拖垮资源管理器）。

`.dspack` = 8 字节 `DSPK` 头 + 标准 ZIP（内含 `manifest.json`）。读取时**先剥 8 字节**，再把余下字节交给 zip 库。

## 环境要求

- Windows 10 / 11（64 位）
- Visual Studio 2022（或 `msbuild`）
- .NET Framework 4.7.2

## 构建

```bat
msbuild src\DspackPreview\DspackPreview.csproj /p:Configuration=Release /p:Platform=AnyCPU
```

## 注册 / 卸载

```bat
:: 注册（首次或提示权限时以管理员运行）
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\regasm.exe /codebase src\DspackPreview\bin\Release\DspackPreview.dll

:: 卸载
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\regasm.exe /unregister src\DspackPreview\bin\Release\DspackPreview.dll
```

`[ComRegisterFunction]` 会一并写入 `.dspack` 的 shell 扩展映射（每用户 `HKCU`，无需管理员）；COM 类本身用 `regasm /codebase` 注册。

调试时刷新宿主缓存：`taskkill /f /im prevhost.exe`（或重启资源管理器）。

## 目录

```
src/DspackPreview/
├── DspackPreview.csproj      # net472 类库（COM 可见）
├── Properties/AssemblyInfo.cs
├── Interop.cs                # COM 接口/结构声明（IPreviewHandler 等）
├── ManifestReader.cs         # 剥 DSPK 头 + 读 manifest.json → 摘要
├── PreviewForm.cs            # 预览卡片窗口（WinForms）
└── DspackPreviewHandler.cs   # 处理器入口（COM 类 + 注册）
```

## TODO / 路线

- [ ] 流式读取（避免整包 `ReadAllBytes`），大文件自动跳过。
- [ ] 丰富卡片排版，或用 WebView2 渲染 HTML。
- [ ] `displayName/description` 多语言的 `pickLang` 逻辑对齐市场页。
- [ ] 异常兜底（zip 解析失败显示「无效 .dspack」，而非空窗格）。
- [ ] 深色模式（`IPreviewHandlerVisuals`）。
- [ ] 补缩略图 `IThumbnailProvider`（本仓库当前只做预览）。
- [ ] CI 构建 + 发布发布物。

## License

[MIT](LICENSE) © 2026 DSH-PackForge