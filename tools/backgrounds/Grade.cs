// Day/night grading of the client's AI-generated scenic images, so each scene exists
// as the same place by day (light appearance) and by night (dark appearance).
// Deterministic: the same inputs give the same bytes.
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class TotbGrade
{
    sealed class Img
    {
        public int W, H;
        public float[] R, G, B; // linear light, 0..1
    }

    static float ToLinear(float v) { return v <= 0.04045f ? v / 12.92f : (float)Math.Pow((v + 0.055f) / 1.055f, 2.4f); }
    static float ToSrgb(float v)
    {
        v = Math.Max(0, Math.Min(1, v));
        return v <= 0.0031308f ? v * 12.92f : 1.055f * (float)Math.Pow(v, 1 / 2.4f) - 0.055f;
    }
    static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    static float Smooth(float a, float b, float v) { var t = Clamp01((v - a) / (b - a)); return t * t * (3 - 2 * t); }
    static float Mix(float a, float b, float t) { return a + (b - a) * t; }

    static Img Load(string path)
    {
        using (var src = new Bitmap(path))
        using (var bmp = new Bitmap(src.Width, src.Height, PixelFormat.Format24bppRgb))
        {
            using (var g = Graphics.FromImage(bmp)) g.DrawImage(src, 0, 0, src.Width, src.Height);
            var img = new Img { W = bmp.Width, H = bmp.Height };
            int n = img.W * img.H;
            img.R = new float[n]; img.G = new float[n]; img.B = new float[n];
            var data = bmp.LockBits(new Rectangle(0, 0, img.W, img.H), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            var bytes = new byte[data.Stride * img.H];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            bmp.UnlockBits(data);
            var lut = new float[256];
            for (int i = 0; i < 256; i++) lut[i] = ToLinear(i / 255f);
            for (int y = 0; y < img.H; y++)
                for (int x = 0; x < img.W; x++)
                {
                    int o = y * data.Stride + x * 3, i = y * img.W + x;
                    img.B[i] = lut[bytes[o]]; img.G[i] = lut[bytes[o + 1]]; img.R[i] = lut[bytes[o + 2]];
                }
            return img;
        }
    }

    static void Save(Img img, string path)
    {
        using (var bmp = new Bitmap(img.W, img.H, PixelFormat.Format24bppRgb))
        {
            var data = bmp.LockBits(new Rectangle(0, 0, img.W, img.H), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            var bytes = new byte[data.Stride * img.H];
            for (int y = 0; y < img.H; y++)
                for (int x = 0; x < img.W; x++)
                {
                    int o = y * data.Stride + x * 3, i = y * img.W + x;
                    // Ordered dither keeps the dark gradients free of banding.
                    float d = (((x * 7 + y * 13) % 16) / 16f - 0.5f) / 255f;
                    bytes[o] = (byte)Math.Round(Clamp01(ToSrgb(img.B[i]) + d) * 255);
                    bytes[o + 1] = (byte)Math.Round(Clamp01(ToSrgb(img.G[i]) + d) * 255);
                    bytes[o + 2] = (byte)Math.Round(Clamp01(ToSrgb(img.R[i]) + d) * 255);
                }
            Marshal.Copy(bytes, 0, data.Scan0, bytes.Length);
            bmp.UnlockBits(data);
            var codec = Array.Find(ImageCodecInfo.GetImageEncoders(), c => c.MimeType == "image/jpeg");
            var args = new EncoderParameters(1);
            args.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 92L);
            bmp.Save(path, codec, args);
        }
    }

    // The lowest y (per column) the sky may reach, from a skyline hand-traced
    // as x,y source pixels; below it nothing is treated as sky.
    static float[] Skyline(Img img, float[] points)
    {
        var line = new float[img.W];
        for (int x = 0; x < img.W; x++)
        {
            int k = 0;
            while (k + 4 < points.Length && points[k + 2] < x) k += 2;
            float t = Clamp01((x - points[k]) / Math.Max(1e-6f, points[k + 2] - points[k]));
            line[x] = Mix(points[k + 1], points[k + 3], t);
        }
        return line;
    }

    static float[] Blur(float[] src, int w, int h, int radius)
    {
        var tmp = new float[src.Length];
        var dst = new float[src.Length];
        for (int y = 0; y < h; y++)
        {
            float acc = 0; int count = 0;
            for (int x = -radius; x < w; x++)
            {
                if (x + radius < w) { acc += src[y * w + x + radius]; count++; }
                if (x - radius - 1 >= 0) { acc -= src[y * w + x - radius - 1]; count--; }
                if (x >= 0) tmp[y * w + x] = acc / count;
            }
        }
        for (int x = 0; x < w; x++)
        {
            float acc = 0; int count = 0;
            for (int y = -radius; y < h; y++)
            {
                if (y + radius < h) { acc += tmp[(y + radius) * w + x]; count++; }
                if (y - radius - 1 >= 0) { acc -= tmp[(y - radius - 1) * w + x]; count--; }
                if (y >= 0) dst[y * w + x] = acc / count;
            }
        }
        return dst;
    }

    static uint _seed;
    static float Rand() { _seed = _seed * 1664525u + 1013904223u; return (_seed >> 8) / 16777216f; }

    /// Day image -> the same place under a clear moonlit night.
    public static void Night(string input, string output, float[] skyline, float moonX, float moonY)
    {
        var img = Load(input);
        int w = img.W, h = img.H, n = w * h;
        var limit = Skyline(img, skyline);
        // Soft sky mask: blue and bright, above the traced skyline.
        var sky = new float[n];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = y * w + x;
                float sr = ToSrgb(img.R[i]), sg = ToSrgb(img.G[i]), sb = ToSrgb(img.B[i]);
                float blue = sb - Math.Max(sr, sg * 0.9f);
                float m = Smooth(0.02f, 0.12f, blue) * Smooth(0.45f, 0.7f, sb);
                // White clouds count as sky away from the (snowy) skyline.
                m = Math.Max(m, Smooth(0.8f, 0.92f, Math.Min(sr, Math.Min(sg, sb))) * 0.85f * Smooth(limit[x] - 10, limit[x] - 40, y));
                sky[i] = m * Smooth(limit[x] + 2, limit[x] - 10, y);
            }
        sky = Blur(sky, w, h, 2);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = y * w + x;
                float r = img.R[i], g = img.G[i], b = img.B[i];
                float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                // Ground: moonlight — dim, cool, mostly desaturated, highlights
                // (rock and snow) kept luminous.
                float dim = 0.075f + 0.10f * Smooth(0.15f, 0.8f, lum);
                float gl = lum * dim;
                float gr = Mix(gl * 0.78f, r * dim * 0.8f, 0.22f);
                float gg = Mix(gl * 0.93f, g * dim * 0.95f, 0.22f);
                float gb = Mix(gl * 1.35f, b * dim * 1.25f, 0.22f);
                // Sky: deep navy, darker overhead, with the clouds kept faint.
                float v = y / (float)h;
                float top = Smooth(0f, 0.55f, v);
                float sr = Mix(0.0035f, 0.018f, top), sg = Mix(0.006f, 0.03f, top), sbl = Mix(0.017f, 0.07f, top);
                float cloudy = Smooth(0.55f, 0.85f, Math.Min(ToSrgb(r), ToSrgb(b)));
                sr += cloudy * 0.012f; sg += cloudy * 0.016f; sbl += cloudy * 0.03f;
                float s = sky[i];
                img.R[i] = Mix(gr, sr, s); img.G[i] = Mix(gg, sg, s); img.B[i] = Mix(gb, sbl, s);
            }
        // Moon with a soft halo.
        float mx = moonX * w, my = moonY * h, mr = h * 0.022f;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = y * w + x;
                float s = sky[i];
                if (s <= 0) continue;
                float d = (float)Math.Sqrt((x - mx) * (x - mx) + (y - my) * (y - my));
                float halo = 0.05f * (float)Math.Exp(-d / (h * 0.09f)) + 0.02f * (float)Math.Exp(-d / (h * 0.3f));
                float disc = Smooth(mr + 1.2f, mr - 1.2f, d);
                img.R[i] += s * (halo * 0.75f + disc * 0.82f);
                img.G[i] += s * (halo * 0.85f + disc * 0.86f);
                img.B[i] += s * (halo * 1.1f + disc * 0.8f);
            }
        // Stars only in clear sky, fading towards the horizon and the moon.
        _seed = 20261026;
        int stars = (int)(w * h / 900f);
        for (int k = 0; k < stars; k++)
        {
            float fx = Rand() * w, fy = Rand() * h;
            float size = Rand(), bright = (float)Math.Pow(Rand(), 3);
            int cx = (int)fx, cy = (int)fy;
            if (cy >= h || cx >= w) continue;
            float clear = sky[cy * w + cx];
            float d = (float)Math.Sqrt((fx - mx) * (fx - mx) + (fy - my) * (fy - my));
            float fade = clear * Smooth(limit[cx] - 4, limit[cx] - 60, fy) * Smooth(mr * 2, mr * 9, d);
            if (fade < 0.6f) continue;
            float amp = (0.04f + 0.55f * bright) * fade;
            float rad = 0.55f + size * size * 0.9f;
            float tint = Rand();
            for (int y = cy - 2; y <= cy + 2; y++)
                for (int x = cx - 2; x <= cx + 2; x++)
                {
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    float dd = (float)Math.Sqrt((x + 0.5f - fx) * (x + 0.5f - fx) + (y + 0.5f - fy) * (y + 0.5f - fy));
                    float a = amp * (float)Math.Exp(-(dd * dd) / (rad * rad));
                    int i = y * w + x;
                    img.R[i] += a * (tint < 0.2f ? 1.0f : 0.85f);
                    img.G[i] += a * 0.9f;
                    img.B[i] += a * (tint > 0.8f ? 0.8f : 1.0f);
                }
        }
        Vignette(img, 0.35f);
        Save(img, output);
    }

    /// Evening image -> the same place in afternoon daylight.
    public static void Day(string input, string output, float[] skyline)
    {
        var img = Load(input);
        int w = img.W, h = img.H, n = w * h;
        var limit = Skyline(img, skyline);
        var sky = SkyMatte(img, limit, 12);
        for (int i = 0; i < n; i++)
        {
            int y = i / w;
            float r = img.R[i], g = img.G[i], b = img.B[i];
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float s = sky[i];
            // Ground: open the shadows and turn the low golden sun towards
            // neutral afternoon light.
            float lift = 2.3f;
            float gr = r * lift * 0.84f, gg = g * lift * 0.97f, gb = b * lift * 1.12f;
            float gl = 0.2126f * gr + 0.7152f * gg + 0.0722f * gb;
            float warm = Smooth(0.0f, 0.25f, gr - gb);
            float sat = Mix(1.08f, 0.82f, warm);
            gr = Mix(gl, gr, sat); gg = Mix(gl, gg, sat); gb = Mix(gl, gb, sat);
            gr = Filmic(gr); gg = Filmic(gg); gb = Filmic(gb);
            // Sky: the evening's dark cloud bodies become white cumulus and
            // its bright gaps and glow open onto blue.
            float v = y / (float)h;
            float sv = Smooth(0, 0.5f, v);
            float blueR = Mix(0.10f, 0.40f, sv), blueG = Mix(0.25f, 0.60f, sv), blueB = Mix(0.64f, 0.88f, sv);
            float gap = Smooth(0.03f, 0.22f, lum);
            float shade = Mix(0.62f, 0.95f, Smooth(0.004f, 0.05f, lum));
            float cr = Mix(shade * 0.97f, blueR, gap), cg = Mix(shade * 0.98f, blueG, gap), cb = Mix(shade, blueB, gap);
            img.R[i] = Mix(gr, cr, s); img.G[i] = Mix(gg, cg, s); img.B[i] = Mix(gb, cb, s);
        }
        Save(img, output);
    }

    static float Filmic(float v) { return v / (1 + v * 0.45f) * 1.45f; }

    /// Sky opacity per pixel: 1 above the traced line, 0 below, and inside
    /// +-band of it each pixel is matched against the column's own sky and
    /// ground colours just outside the band. Monotone down each column (the
    /// skyline has no overhangs), so rock texture never turns into sky.
    static float[] SkyMatte(Img img, float[] limit, int band)
    {
        int w = img.W, h = img.H;
        var sky = new float[w * h];
        for (int x = 0; x < w; x++)
        {
            int l = (int)Math.Round(limit[x]);
            float[] sc = MeanColumn(img, x, l - band - 8, l - band - 1), gc = MeanColumn(img, x, l + band + 1, l + band + 8);
            float dr = sc[0] - gc[0], dg = sc[1] - gc[1], db = sc[2] - gc[2];
            float d2 = dr * dr + dg * dg + db * db;
            // Where sky and ground look alike the colour match is noise.
            float trust = Smooth(0.006f, 0.05f, d2);
            float previous = 1;
            for (int y = 0; y < h; y++)
            {
                float a;
                if (y < l - band) a = 1;
                else if (y > l + band) a = 0;
                else
                {
                    float geometric = Smooth(l + 2, l - 2, y);
                    if (trust <= 0) a = geometric;
                    else
                    {
                        float[] p = MeanRow(img, x - 1, x + 1, y);
                        float pr = p[0] - gc[0], pg = p[1] - gc[1], pb = p[2] - gc[2];
                        a = Mix(geometric, Smooth(0.3f, 0.7f, (pr * dr + pg * dg + pb * db) / d2), trust);
                    }
                }
                a = Math.Min(a, previous);
                previous = a;
                sky[y * w + x] = a;
            }
        }
        // Neighbouring columns agree, so single-column decisions cannot
        // leave streaks.
        var smooth = new float[sky.Length];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float acc = 0; int count = 0;
                for (int k = Math.Max(0, x - 3); k <= Math.Min(w - 1, x + 3); k++) { acc += sky[y * w + k]; count++; }
                smooth[y * w + x] = acc / count;
            }
        return Blur(smooth, w, h, 1);
    }

    // Mean sRGB colour over a column window (x0..x1, y0..y1 inclusive).
    static float[] MeanColumn(Img img, int x, int y0, int y1) { return Mean(img, x - 3, x + 3, y0, y1); }

    static float[] MeanRow(Img img, int x0, int x1, int y) { return Mean(img, x0, x1, y, y); }

    static float[] Mean(Img img, int x0, int x1, int y0, int y1)
    {
        float r = 0, g = 0, b = 0; int count = 0;
        for (int y = Math.Max(0, y0); y <= Math.Min(img.H - 1, y1); y++)
            for (int x = Math.Max(0, x0); x <= Math.Min(img.W - 1, x1); x++)
            {
                int i = y * img.W + x;
                r += ToSrgb(img.R[i]); g += ToSrgb(img.G[i]); b += ToSrgb(img.B[i]); count++;
            }
        return count == 0 ? new float[3] : new[] { r / count, g / count, b / count };
    }

    static void Vignette(Img img, float strength)
    {
        int w = img.W, h = img.H;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float dx = (x - w / 2f) / (w / 2f), dy = (y - h / 2f) / (h / 2f);
                float k = 1 - strength * Smooth(0.6f, 1.6f, dx * dx + dy * dy);
                int i = y * w + x;
                img.R[i] *= k; img.G[i] *= k; img.B[i] *= k;
            }
    }
}
