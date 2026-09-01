using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace DspackPreview
{
    /// <summary>
    /// .dspack 预览处理器：实现 IPreviewHandler + IInitializeWithFile，由资源管理器宿主（prevhost.exe）加载。
    /// 只实现 IInitializeWithFile（本地文件）；Explorer 优先走 IInitializeWithStream 时 QueryInterface 会失败并回退。
    /// 调试：所有入口写日志到 %TEMP%\dspack-preview.log。
    /// </summary>
    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    [Guid("7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d")]
    [ProgId("DspackPreview.PreviewHandler")]
    public sealed class DspackPreviewHandler : IPreviewHandler, IInitializeWithFile, IObjectWithSite
    {
        private const string HandlerClsid = "{7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d}";
        private const string PreviewHandlerCategory = "{8895b1c6-b41f-4c1c-a562-0d564250836f}";

        private string _filePath;
        private object _site;
        private IntPtr _parentHwnd;
        private RECT _rect;
        private PreviewForm _form;

        public DspackPreviewHandler()
        {
            Log.Write("ctor");
        }

        #region IInitializeWithFile
        public void Initialize(string pszFilePath, uint grfMode)
        {
            _filePath = pszFilePath;
            Log.Write("Initialize(file) path=" + pszFilePath);
        }
        #endregion

        #region IObjectWithSite
        public void SetSite(object pUnkSite)
        {
            _site = pUnkSite;
            Log.Write("SetSite");
        }

        public void GetSite(ref Guid riid, out object ppvSite)
        {
            ppvSite = _site;
            Log.Write("GetSite");
        }
        #endregion

        #region IPreviewHandler
        public void SetWindow(IntPtr hwnd, ref RECT prc)
        {
            _parentHwnd = hwnd;
            _rect = prc;
            Log.Write("SetWindow hwnd=0x" + hwnd.ToString("X") + " rect=" + prc.Left + "," + prc.Top + " " + prc.Right + "," + prc.Bottom);
        }

        public void SetRect(ref RECT prc)
        {
            _rect = prc;
            Log.Write("SetRect rect=" + prc.Left + "," + prc.Top + " " + prc.Right + "," + prc.Bottom);
        }

        public void DoPreview()
        {
            Log.Write("DoPreview enter, file=" + _filePath);
            try
            {
                if (_form == null)
                {
                    ManifestInfo info;
                    try { info = ManifestReader.Read(_filePath); }
                    catch (Exception ex)
                    {
                        Log.Write("ManifestReader failed: " + ex);
                        info = new ManifestInfo { DisplayName = "预览失败", Description = ex.Message };
                    }
                    Log.Write("manifest name=" + info.Name + " version=" + info.Version);

                    _form = new PreviewForm(info)
                    {
                        TopLevel = false,
                        FormBorderStyle = System.Windows.Forms.FormBorderStyle.None,
                        ShowInTaskbar = false
                    };
                    _form.CreateControl();
                    Log.Write("preview form handle=0x" + _form.Handle.ToString("X"));
                }

                Native.SetParent(_form.Handle, _parentHwnd);
                Native.SetWindowLong(_form.Handle, Native.GWL_STYLE, Native.GetWindowLong(_form.Handle, Native.GWL_STYLE) | Native.WS_CHILD);
                Native.MoveWindow(_form.Handle, _rect.Left, _rect.Top, Math.Max(1, _rect.Right - _rect.Left), Math.Max(1, _rect.Bottom - _rect.Top), true);
                _form.Show();
                Log.Write("DoPreview ok");
            }
            catch (Exception ex)
            {
                Log.Write("DoPreview EXCEPTION: " + ex);
            }
        }

        public void Unload()
        {
            Log.Write("Unload");
            if (_form == null) return;
            _form.Hide();
            _form.Dispose();
            _form = null;
        }

        public void SetFocus()
        {
            Log.Write("SetFocus");
            _form?.Focus();
        }

        public void QueryFocus(out IntPtr phwnd)
        {
            phwnd = _form != null ? _form.Handle : IntPtr.Zero;
            Log.Write("QueryFocus hwnd=0x" + phwnd.ToString("X"));
        }

        public void TranslateAccelerator(ref MSG pmsg)
        {
            // 骨架：不处理快捷键。
        }
        #endregion

        #region COM 注册（regasm 调用，写每用户 .dspack 映射与托管类注册）
        [ComRegisterFunction]
        public static void Register(Type t)
        {
            // 机器级（HKLM\Software\Classes）：预览处理器由 Explorer 的 prevhost.exe 加载，每用户注册不会被采信。
            using (var key = Registry.ClassesRoot.CreateSubKey($@".dspack\shellex\{PreviewHandlerCategory}"))
                key.SetValue(null, HandlerClsid);

            using (var key = Registry.ClassesRoot.CreateSubKey($@"CLSID\{HandlerClsid}\InprocServer32"))
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
            Registry.ClassesRoot.DeleteSubKeyTree($@".dspack\shellex\{PreviewHandlerCategory}", false);
            Registry.ClassesRoot.DeleteSubKeyTree($@"CLSID\{HandlerClsid}", false);
        }
        #endregion
    }

    /// <summary>简单文件日志，写入 %TEMP%\dspack-preview.log，任何失败静默忽略。</summary>
    internal static class Log
    {
        private static readonly object Sync = new object();
        private static readonly string File = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "dspack-preview.log");

        public static void Write(string message)
        {
            try
            {
                lock (Sync)
                {
                    var p = System.Diagnostics.Process.GetCurrentProcess();
                    System.IO.File.AppendAllText(File,
                        DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") +
                        " [" + p.ProcessName + ":" + p.Id + "] " + message + Environment.NewLine);
                }
            }
            catch
            {
            }
        }
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