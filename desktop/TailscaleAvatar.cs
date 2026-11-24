using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Media.Imaging;
using SkiaSharp;

namespace Aurora;

internal static class TailscaleAvatar
{
    private const int MaxBytes = 256 * 1024;
    private const int MaxDimension = 1024;
    private static readonly TimeSpan RequestLimit = TimeSpan.FromSeconds(4);
    private static readonly SemaphoreSlim cacheGate = new(1, 1);
    // Both views share the encoded bytes. Each view owns and disposes its bitmap.
    private static (Uri Uri, byte[] Bytes, long Expires) cache;
    private static readonly HttpClient client = new(new SocketsHttpHandler
    {
        AllowAutoRedirect = false,
        UseCookies = false,
        UseProxy = false,
        Credentials = null,
        ActivityHeadersPropagator = null,
        MaxResponseHeadersLength = 16,
        MaxResponseDrainSize = 0
    });

    internal static Uri VettedUri(string url) =>
        Uri.TryCreate(url, UriKind.Absolute, out var uri) && IsVetted(uri) ? uri : null;

    private static bool IsVetted(Uri uri)
    {
        if (uri is not { IsAbsoluteUri: true, Scheme: "https", IsDefaultPort: true } ||
            uri.UserInfo.Length != 0 || uri.Fragment.Length != 0 || uri.AbsoluteUri.Length > 2048) return false;
        var path = uri.AbsolutePath;
        return uri.IdnHost switch
        {
            "lh3.googleusercontent.com" or "lh4.googleusercontent.com" => path.Length > 1 && uri.Query.Length == 0,
            "avatars.githubusercontent.com" => path.StartsWith("/u/", StringComparison.Ordinal) &&
                path.Length > 3 && path.AsSpan(3).IndexOfAnyExceptInRange('0', '9') < 0 &&
                (uri.Query.Length == 0 || uri.Query == "?v=4"),
            _ => false
        };
    }

    /// <summary>Loads an owned bitmap for a visible, connected account view using its visibility token.</summary>
    internal static async Task<Bitmap> LoadAsync(TailscaleStatus status, CancellationToken cancellationToken)
    {
        if (status is not { State: TailscaleState.Connected, OwnerId: > 0, AvatarUri: { } uri } || !IsVetted(uri))
            return null;

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(RequestLimit);
        try
        {
            await cacheGate.WaitAsync(timeout.Token).ConfigureAwait(false);
            try
            {
                if (cache.Uri == uri && Environment.TickCount64 < cache.Expires)
                {
                    timeout.Token.ThrowIfCancellationRequested();
                    return Decode(cache.Bytes);
                }

                byte[] bytes = null;
                Bitmap image = null;
                try
                {
                    bytes = await Download(uri, timeout.Token).ConfigureAwait(false);
                    timeout.Token.ThrowIfCancellationRequested();
                    image = Decode(bytes);
                }
                catch (HttpRequestException) { }
                catch (IOException) { }
                catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { }
                if (cancellationToken.IsCancellationRequested)
                {
                    image?.Dispose();
                    cancellationToken.ThrowIfCancellationRequested();
                }
                cache = image == null
                    ? (uri, null, Environment.TickCount64 + 30_000)
                    : (uri, bytes, Environment.TickCount64 + 600_000);
                return image;
            }
            finally { cacheGate.Release(); }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { return null; }
    }

    private static async Task<byte[]> Download(Uri uri, CancellationToken token)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, uri);
        request.Headers.Accept.ParseAdd("image/png, image/jpeg, image/webp");
        using var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token).ConfigureAwait(false);
        var headers = response.Content.Headers;
        if (response.StatusCode != HttpStatusCode.OK || headers.ContentLength > MaxBytes ||
            headers.ContentEncoding.Count != 0 ||
            headers.ContentType?.MediaType?.ToLowerInvariant() is not ("image/png" or "image/jpeg" or "image/webp"))
            return null;

        using var stream = await response.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
        using var bytes = new MemoryStream();
        var buffer = new byte[8192];
        while (true)
        {
            int remaining = MaxBytes - (int)bytes.Length;
            var count = await stream.ReadAsync(buffer.AsMemory(0, Math.Min(buffer.Length, remaining + 1)), token).ConfigureAwait(false);
            if (count == 0) return bytes.ToArray();
            if (count > remaining) return null;
            bytes.Write(buffer, 0, count);
        }
    }

    // Qt uses a small local PNG and does not fetch the profile image again.
    internal static string EncodeRound(Bitmap image)
    {
        using var stream = new MemoryStream();
        image.Save(stream);
        stream.Position = 0;
        using var bitmap = SKBitmap.Decode(stream);
        if (bitmap == null) return null;

        const int size = 96;
        var side = Math.Min(bitmap.Width, bitmap.Height);
        var left = (bitmap.Width - side) / 2f;
        var top = (bitmap.Height - side) / 2f;
        using var surface = SKSurface.Create(new SKImageInfo(size, size));
        if (surface == null) return null;
        surface.Canvas.Clear(SKColors.Transparent);
        using var circle = new SKPath();
        circle.AddCircle(size / 2f, size / 2f, size / 2f);
        surface.Canvas.ClipPath(circle, antialias: true);
        using var paint = new SKPaint { IsAntialias = true, FilterQuality = SKFilterQuality.Medium };
        surface.Canvas.DrawBitmap(bitmap, new SKRect(left, top, left + side, top + side),
            new SKRect(0, 0, size, size), paint);
        using var avatar = surface.Snapshot();
        using var encoded = avatar.Encode(SKEncodedImageFormat.Png, 100);
        return encoded == null ? null : "data:image/png;base64," + Convert.ToBase64String(encoded.ToArray());
    }

    private static Bitmap Decode(byte[] bytes)
    {
        if (bytes is not { Length: > 0 }) return null;
        try
        {
            using var data = SKData.CreateCopy(bytes);
            using var codec = SKCodec.Create(data);
            if (codec is not { Info.Width: > 0 and <= MaxDimension, Info.Height: > 0 and <= MaxDimension } ||
                codec.EncodedFormat is not (SKEncodedImageFormat.Png or SKEncodedImageFormat.Jpeg or SKEncodedImageFormat.Webp))
                return null;
            using var stream = new MemoryStream(bytes, writable: false);
            return Bitmap.DecodeToWidth(stream, Math.Min(96, codec.Info.Width));
        }
        catch (ArgumentException) { return null; }
        catch (InvalidOperationException) { return null; }
        catch (NotSupportedException) { return null; }
    }
}
