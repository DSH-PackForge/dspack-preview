using System;
using System.Drawing;
using System.Windows.Forms;

namespace DspackPreview
{
    /// <summary>预览卡片窗口（极简版）。TODO：用 WebView2 换成富文本。</summary>
    internal sealed class PreviewForm : Form
    {
        private readonly TextBox _details;

        public PreviewForm(ManifestInfo info)
        {
            Text = info.DisplayName ?? info.Name ?? ".dspack";
            BackColor = SystemColors.Window;
            Padding = new Padding(12);

            var title = new Label
            {
                AutoSize = false,
                Dock = DockStyle.Top,
                Height = 28,
                Font = new Font("Segoe UI", 12, FontStyle.Bold),
                ForeColor = SystemColors.ControlText,
                Text = $"{(info.DisplayName ?? info.Name ?? "未命名")}  ·  v{info.Version ?? "?"}"
            };

            _details = new TextBox
            {
                Dock = DockStyle.Fill,
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                BorderStyle = BorderStyle.None,
                BackColor = SystemColors.Window,
                Font = new Font("Consolas", 9),
                Text = BuildDetails(info)
            };

            Controls.Add(_details);
            Controls.Add(title);
        }

        private static string BuildDetails(ManifestInfo info)
        {
            var sb = new System.Text.StringBuilder();
            sb.AppendLine($"type        : {info.Type}");
            sb.AppendLine($"name        : {info.Name}");
            sb.AppendLine($"version     : {info.Version}");
            sb.AppendLine($"author      : {info.Author}");
            sb.AppendLine($"dshVersion  : {info.DshVersion}");
            sb.AppendLine($"bundles     : {info.Bundles}");
            sb.AppendLine($"dependencies: {info.Dependencies}");
            sb.AppendLine($"files       : {info.Files}");
            sb.AppendLine();
            sb.AppendLine(info.Description ?? "(无描述)");
            return sb.ToString();
        }
    }
}