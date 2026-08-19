using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

internal static class GenerateIconV6
{
    private static readonly int[] IconSizes = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };

    private static int Main(string[] args)
    {
        string outputDirectory = args.Length > 0
            ? Path.GetFullPath(args[0])
            : Environment.CurrentDirectory;

        Directory.CreateDirectory(outputDirectory);
        string frameDirectory = Path.Combine(outputDirectory, "icon-v6-trigger-frames");
        Directory.CreateDirectory(frameDirectory);

        var frames = new List<byte[]>();
        foreach (int size in IconSizes)
        {
            using (Bitmap bitmap = RenderAtSize(size))
            using (var stream = new MemoryStream())
            {
                bitmap.Save(stream, ImageFormat.Png);
                byte[] png = stream.ToArray();
                frames.Add(png);
                File.WriteAllBytes(Path.Combine(frameDirectory, size + ".png"), png);
            }
        }

        string iconPath = Path.Combine(outputDirectory, "IYX-icon-v6-trigger.ico");
        WriteIcon(iconPath, IconSizes, frames);

        using (Bitmap preview = RenderAtSize(512))
            preview.Save(
                Path.Combine(outputDirectory, "IYX-icon-v6-trigger-preview.png"),
                ImageFormat.Png);

        Console.WriteLine(iconPath);
        return 0;
    }

    private static Bitmap RenderAtSize(int size)
    {
        int renderSize = Math.Max(256, size * 4);
        using (Bitmap source = RenderDesign(renderSize))
        {
            var output = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            output.SetResolution(96, 96);
            using (Graphics graphics = Graphics.FromImage(output))
            {
                graphics.Clear(Color.Transparent);
                graphics.CompositingMode = CompositingMode.SourceCopy;
                graphics.CompositingQuality = CompositingQuality.HighQuality;
                graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
                graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
                graphics.SmoothingMode = SmoothingMode.HighQuality;
                graphics.DrawImage(source, new Rectangle(0, 0, size, size));
            }
            return output;
        }
    }

    private static Bitmap RenderDesign(int size)
    {
        var bitmap = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        bitmap.SetResolution(96, 96);

        using (Graphics graphics = Graphics.FromImage(bitmap))
        {
            graphics.Clear(Color.Transparent);
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.CompositingQuality = CompositingQuality.HighQuality;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.ScaleTransform(size / 512f, size / 512f);

            using (GraphicsPath bodyPath = SquirclePath(26, 26, 460, 460, 112))
            using (var body = new SolidBrush(Color.FromArgb(255, 17, 19, 21)))
                graphics.FillPath(body, bodyPath);

            Color white = Color.FromArgb(255, 245, 247, 250);
            Color signal = Color.FromArgb(255, 191, 255, 60);

            using (var keycap = CreateLucidePen(white, 30f))
            using (GraphicsPath keycapPath = CreateOpenKeycapPath())
                graphics.DrawPath(keycap, keycapPath);

            using (var arrow = CreateLucidePen(signal, 36f))
            using (GraphicsPath arrowPath = CreateEnterArrowPath())
                graphics.DrawPath(arrow, arrowPath);
        }

        return bitmap;
    }

    private static GraphicsPath CreateOpenKeycapPath()
    {
        var path = new GraphicsPath();
        path.StartFigure();
        path.AddLine(365, 132, 158, 132);
        path.AddBezier(158, 132, 130, 132, 112, 150, 112, 178);
        path.AddLine(112, 178, 112, 331);
        path.AddBezier(112, 331, 112, 359, 130, 377, 158, 377);
        path.AddLine(158, 377, 359, 377);
        path.AddBezier(359, 377, 387, 377, 405, 359, 405, 331);
        path.AddLine(405, 331, 405, 303);
        return path;
    }

    private static GraphicsPath CreateEnterArrowPath()
    {
        var path = new GraphicsPath();
        path.StartFigure();
        path.AddLine(415, 166, 415, 232);
        path.AddBezier(415, 232, 415, 262, 394, 282, 364, 282);
        path.AddLine(364, 282, 231, 282);

        path.StartFigure();
        path.AddLine(231, 282, 277, 238);
        path.StartFigure();
        path.AddLine(231, 282, 277, 326);
        return path;
    }

    private static Pen CreateLucidePen(Color color, float width)
    {
        var pen = new Pen(color, width);
        pen.StartCap = LineCap.Round;
        pen.EndCap = LineCap.Round;
        pen.LineJoin = LineJoin.Round;
        return pen;
    }

    private static GraphicsPath SquirclePath(
        float x,
        float y,
        float width,
        float height,
        float radius)
    {
        float right = x + width;
        float bottom = y + height;
        float control = radius * 0.56f;
        var path = new GraphicsPath();

        path.StartFigure();
        path.AddLine(x + radius, y, right - radius, y);
        path.AddBezier(
            right - radius, y,
            right - radius + control, y,
            right, y + radius - control,
            right, y + radius);
        path.AddLine(right, y + radius, right, bottom - radius);
        path.AddBezier(
            right, bottom - radius,
            right, bottom - radius + control,
            right - radius + control, bottom,
            right - radius, bottom);
        path.AddLine(right - radius, bottom, x + radius, bottom);
        path.AddBezier(
            x + radius, bottom,
            x + radius - control, bottom,
            x, bottom - radius + control,
            x, bottom - radius);
        path.AddLine(x, bottom - radius, x, y + radius);
        path.AddBezier(
            x, y + radius,
            x, y + radius - control,
            x + radius - control, y,
            x + radius, y);
        path.CloseFigure();

        return path;
    }

    private static void WriteIcon(string path, int[] sizes, IList<byte[]> frames)
    {
        using (var stream = File.Create(path))
        using (var writer = new BinaryWriter(stream))
        {
            writer.Write((ushort)0);
            writer.Write((ushort)1);
            writer.Write((ushort)frames.Count);

            int offset = 6 + (16 * frames.Count);
            for (int index = 0; index < frames.Count; index++)
            {
                int size = sizes[index];
                writer.Write((byte)(size == 256 ? 0 : size));
                writer.Write((byte)(size == 256 ? 0 : size));
                writer.Write((byte)0);
                writer.Write((byte)0);
                writer.Write((ushort)1);
                writer.Write((ushort)32);
                writer.Write(frames[index].Length);
                writer.Write(offset);
                offset += frames[index].Length;
            }

            foreach (byte[] frame in frames)
                writer.Write(frame);
        }
    }
}
