using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using Microsoft.Win32;

namespace DspackPreview
{
    /// <summary>
    /// .dspack 预览处理器：实现 IPreviewHandler + 初始化接口，由资源管理器宿主（prevhost.exe）加载。
    /// </summary>
    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    [Guid("7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d")]
    [ProgId("DspackPreview.PreviewHandler")]
    public sealed class DspackPreviewHandler : IPreviewHandler, IInitializeWithFile, IInitializeWithStream, IObjectWithSite
    {
        private const string HandlerClsid = "{7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d}";
        private const string PreviewHandlerCategory = "{8895b1c6-b41f-4c1c-a562-0d564250836f}";

        private string _filePath;
        private object _site;
        private IntPtr _parentHwnd;
        private RECT _rect;
        private PreviewForm _form;

        #region IInitializeWithFile
        public void Initialize(string pszFilePath, uint grfMode) => _filePath = pszFilePath;
        #endregion

        #region IInitializeWithStream
        public void Initialize(IStream pstream, uint grfMode)
        {
            // 骨架：流式初始化暂不支持，抛 E_ACCESSDENIED 让宿主回退到 IInitializeWithFile。
            // TODO：从 IStream 读入内存/临时文件再解析。
            Marshal.ThrowExceptionForHR(unchecked((int)0x80070005));
        }
        #endregion

        #region IObjectWithSite
        public void SetSite(object pUnkSite) => _site = pUnkSite;
        public void GetSite(ref Guid riid, out object ppvSite) => ppvSite = _site;
        #endregion

        #region IPreviewHandler
        public void SetWindow(IntPtr hwnd, ref RECT prc)
        {
            _parentHwnd = hwnd;
            _rect = prc;
        }

        public void SetRect(ref RECT prc) => _rect = prc;

        public void DoPreview()
        {
            if (_form == null)
            {
                ManifestInfo info;
                try { info = ManifestReader.Read(_filePath); }
                catch (Exception ex)
                {
                    info = new ManifestInfo { DisplayName = "预览失败", Description = ex.Message };
                }

                _form = new PreviewForm(info)
                {
                    TopLevel = false,
                    BorderStyle = System.Windows.Forms.FormBorderStyle.None,
                    ShowInTaskbar = false
                };
                _form.CreateControl();
            }

            Native.SetParent(_form.Handle, _parentHwnd);
            Native.SetWindowLong(_form.Handle, Native.GWL_STYLE, Native.GetWindowLong(_form.Handle, Native.GWL_STYLE) | Native.WS_CHILD);
            Native.MoveWindow(_form.Handle, _rect.Left, _rect.Top, Math.Max(1, _rect.Right - _rect.Left), Math.Max(1, _rect.Bottom - _rect.Top), true);
            _form.Show();
        }

        public void Unload()
        {
            if (_form == null) return;
            _form.Hide();
            _form.Dispose();
            _form = null;
        }

        public void SetFocus() => _form?.Focus();

        public void QueryFocus(out IntPtr phwnd) => phwnd = _form != null ? _form.Handle : IntPtr.Zero;

        public void TranslateAccelerator(ref MSG pmsg)
        {
            // 骨架：不处理快捷键。
        }
        #endregion

        #region COM 注册（regasm 调用，写每用户 .dspack 映射与托管类注册）
        [ComRegisterFunction]
        public static void Register(Type t)
        {
            using (var key = Registry.CurrentUser.CreateSubKey($@"Software\Classes\.dspack\shellex\{PreviewHandlerCategory}"))
                key.SetValue(null, HandlerClsid);

            using (var key = Registry.CurrentUser.CreateSubKey($@"Software\Classes\CLSID\{HandlerClsid}\InprocServer32"))
            {
                key.SetValue(null, "mscoree.dll");
                key.SetValue("ThreadingModel", "Both");
                key.SetValue("Class", t.FullName);
                key.SetValue("Assembly", t.Assembly.FullName);
                key.SetValue("RuntimeVersion", "v4.0.30319");
                key.SetValue("CodeBase", t.Assembly.CodeBase);
            }
        }

        [ComUnregisterFunction]
        public static void Unregister(Type t)
        {
            Registry.CurrentUser.DeleteSubKeyTree($@"Software\Classes\.dspack\shellex\{PreviewHandlerCategory}", false);
            Registry.CurrentUser.DeleteSubKeyTree($@"Software\Classes\CLSID\{HandlerClsid}", false);
        }
        #endregion
    }

    internal static class Native
    {
        public const int GWL_STYLE = -16;
        public const int WS_CHILD = 0x40000000;

        [DllImport("user32.dll", SetLastError = true)]
        public static extern IntPtr SetParent(IntPtr hWndChild, IntPtr hWndNewParent);

        [DllImport("user32.dll")]
        public static extern int GetWindowLong(IntPtr hWnd, int nIndex);

        [DllImport("user32.dll")]
        public static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);

        [DllImport("user32.dll")]
        public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);
    }
}