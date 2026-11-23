using System;
using System.Globalization;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Aurora;

internal sealed record StreamConnectionSample(int Width, int Height, int Codec, int FrameRate, int VideoKbps,
    int Loss, int DecodeTime, int RoundTrip, bool Poor)
{
    private const string Prefix = "aurora-connection/1,";

    internal static StreamConnectionSample Read(string line)
    {
        if (line == null || !line.StartsWith(Prefix, StringComparison.Ordinal) || line.Length > 160) return null;
        var fields = line[Prefix.Length..].Split(',');
        if (fields.Length != 9) return null;

        Span<int> values = stackalloc int[9];
        for (int i = 0; i < fields.Length; i++)
            if (!int.TryParse(fields[i], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out values[i]))
                return null;

        return new StreamConnectionSample(values[0], values[1], values[2], values[3], values[4], values[5],
            values[6], values[7], values[8] == 1);
    }
}

internal sealed class StreamConnectionDetails : StackPanel
{
    private readonly TextBlock empty = new() { Text = "暂无连接参数", Classes = { "muted" }, FontSize = 13 };
    private readonly StackPanel metrics = new() { Spacing = 8 };

    internal StreamConnectionDetails()
    {
        Width = 266;
        Spacing = 8;
        Children.Add(new TextBlock { Text = "连接详情", FontWeight = FontWeight.SemiBold, FontSize = 14 });
        Children.Add(empty);
        Children.Add(metrics);
    }

    internal void Update(StreamConnectionSample sample)
    {
        metrics.Children.Clear();
        if (sample != null)
        {
            if (sample.RoundTrip > 0) Add("网络延迟", $"{sample.RoundTrip} ms");
            if (sample.Width > 0 && sample.Height > 0) Add("画面分辨率", $"{sample.Width} × {sample.Height}");
            if (sample.FrameRate > 0) Add("接收帧率", $"{sample.FrameRate / 100d:0.0} FPS");
            if (sample.VideoKbps > 0) Add("接收码率", $"{sample.VideoKbps / 1000d:0.0} Mbps");
            if (sample.Loss is >= 0 and <= 10000) Add("网络丢帧", $"{sample.Loss / 100d:0.0}%");
            var codec = sample.Codec switch { 1 => "H.264", 2 => "HEVC", 3 => "AV1", _ => null };
            if (codec != null) Add("视频编码", codec);
            if (sample.DecodeTime >= 0) Add("平均解码耗时", $"{sample.DecodeTime / 1000d:0.0} ms");
        }
        empty.IsVisible = metrics.Children.Count == 0;
        metrics.IsVisible = !empty.IsVisible;
    }

    private void Add(string label, string value)
    {
        var reading = new TextBlock { Text = value, FontSize = 13, HorizontalAlignment = HorizontalAlignment.Right };
        Grid.SetColumn(reading, 1);
        metrics.Children.Add(new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 24,
            Children = { new TextBlock { Text = label, Classes = { "muted" }, FontSize = 13 }, reading }
        });
    }
}
