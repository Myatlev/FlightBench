// OSD symbol font: a MAX7456 .mcm file (the analog OSD font format RotorFlight/Betaflight/iNav configurators
// ship and upload) decoded into a glyph atlas. Format, as the configurator's own parser reads it
// (src/js/tabs/osd.js FONT.parseMCMFontFile): first line "MAX7456", then one byte per line as 8 ASCII
// '0'/'1'; 64 lines = one character, of which the first 54 bytes are the 12×18 glyph, 4 pixels per byte,
// 2 bits each MSB first, row-major; the last 10 bytes are padding. Pixel values: 0 black, 2 white,
// 1 / 3 transparent. The index is the OSD character code.
// Two fonts, picked by the `osd` message's `font` (docs/protocol.md; provenance in assets/osd/FONTS.md):
// - "rotorflight": res://assets/osd/default.mcm, the RotorFlight Configurator's, 256 glyphs; glyph = byte.
// - "inav": res://assets/osd/inav-default.mcm, the INAV Configurator 9.1.1 analogue font, 512 glyphs;
//   glyph = (attr & 3) << 8 | byte (page 1 = the HUD: horizon, sidebars, crosshairs, logo).
// Capacity = the font's glyph slots (256 or 512; a file is read up to its capacity, like the configurator).
// Atlas: 16 glyph cells per row (16 rows for 256, 32 for 512), each 12×18 with a 1 px transparent border
// (so linear filtering does not bleed in the neighbour). No inverted copy: the DisplayPort attr has no
// invert bit. Built once per font path at first use (a few ms) and cached.
using System;
using System.Collections.Generic;
using Godot;

namespace FlightBench;

public sealed class OsdFont
{
    public const int GlyphW = 12, GlyphH = 18, Glyphs = 256, InavGlyphs = 512, Cols = 16;
    private const int LinesPerChar = 64, BytesPerGlyph = GlyphW * GlyphH / 4;   // 54
    private const int StrideW = GlyphW + 2, StrideH = GlyphH + 2;               // 1 px border
    public const string DefaultPath = "res://assets/osd/default.mcm", InavPath = "res://assets/osd/inav-default.mcm";
    /// `osd` message `font` values (docs/protocol.md).
    public const string Rotorflight = "rotorflight", Inav = "inav";

    public string Name { get; }
    /// Characters actually present in the file (the rest stay transparent).
    public int Count { get; }
    /// Glyph slots of this font: 256 (RotorFlight) or 512 (iNav); codes at or above it are not in the font.
    public int Capacity { get; }
    /// RGBA8, 16 cells wide, Capacity / 16 cells high.
    public Image Atlas { get; }
    private readonly byte[] _px;            // Capacity × 216 raw 2-bit values
    private ImageTexture? _tex;

    private static readonly Dictionary<string, OsdFont?> Cache = new();

    private OsdFont(string name, int count, int capacity, byte[] px)
    {
        Name = name; Count = count; Capacity = capacity; _px = px;
        int w = Cols * StrideW, h = capacity / Cols * StrideH;
        var data = new byte[w * h * 4];
        for (int code = 0; code < capacity; code++)
        {
            int ox = code % Cols * StrideW + 1, oy = code / Cols * StrideH + 1;
            for (int y = 0; y < GlyphH; y++)
                for (int x = 0; x < GlyphW; x++)
                {
                    int v = px[code * BytesPerGlyph * 4 + y * GlyphW + x];
                    if (v != 0 && v != 2) continue;                       // transparent
                    byte c = v == 2 ? (byte)255 : (byte)0;
                    int k = ((oy + y) * w + ox + x) * 4;
                    data[k] = data[k + 1] = data[k + 2] = c; data[k + 3] = 255;
                }
        }
        Atlas = Image.CreateFromData(w, h, false, Image.Format.Rgba8, data);
    }

    /// Decodes .mcm text into a font of `capacity` glyph slots (256 or 512); throws FormatException on a
    /// bad header or a non-binary line.
    public static OsdFont Parse(string text, string name, int capacity = Glyphs)
    {
        if (capacity != Glyphs && capacity != InavGlyphs) throw new ArgumentOutOfRangeException(nameof(capacity));
        var lines = text.Trim().Split('\n');
        if (lines.Length == 0 || lines[0].Trim() != "MAX7456") throw new FormatException("no MAX7456 header");
        int n = Math.Min((lines.Length - 1) / LinesPerChar, capacity);         // only full characters, like the configurator
        var px = new byte[capacity * BytesPerGlyph * 4];
        for (int ch = 0; ch < n; ch++)
            for (int b = 0; b < BytesPerGlyph; b++)
            {
                var line = lines[1 + ch * LinesPerChar + b].Trim();
                if (line.Length != 8) throw new FormatException($"line {2 + ch * LinesPerChar + b}: '{line}'");
                for (int i = 0; i < 4; i++)
                {
                    char hi = line[2 * i], lo = line[2 * i + 1];
                    if ((hi != '0' && hi != '1') || (lo != '0' && lo != '1')) throw new FormatException($"line {2 + ch * LinesPerChar + b}: '{line}'");
                    px[ch * BytesPerGlyph * 4 + b * 4 + i] = (byte)((hi - '0') * 2 + (lo - '0'));
                }
            }
        return new OsdFont(name, n, capacity, px);
    }

    /// The font at `path` (res:// or absolute) with `capacity` glyph slots, decoded once and cached; null
    /// (also cached) if missing or bad.
    public static OsdFont? Load(string path, int capacity = Glyphs)
    {
        var key = capacity == Glyphs ? path : $"{path}#{capacity}";
        if (Cache.TryGetValue(key, out var f)) return f;
        f = null;
        if (FileAccess.FileExists(path))
        {
            try { f = Parse(FileAccess.GetFileAsString(path), System.IO.Path.GetFileNameWithoutExtension(path), capacity); }
            catch (FormatException e) { GD.PushWarning($"OSD font {path}: {e.Message}; text fallback"); }
        }
        Cache[key] = f;
        return f;
    }

    public static OsdFont? Default => Load(DefaultPath);

    /// The font for an `osd` message's `font` value: "inav" → iNav's 512-glyph font; anything else (and
    /// null: not said yet) → RotorFlight's.
    public static OsdFont? For(string? font) => font == Inav ? Load(InavPath, InavGlyphs) : Load(DefaultPath);

    /// A cell's glyph index: iNav: (attr & 3) << 8 | byte (the attr's font page = bits 8..9 of iNav's code);
    /// RotorFlight: the byte (its page bits select a configured font page, not part of the code).
    public static int GlyphIndex(string? font, char ch, int attr) => font == Inav ? (attr & 3) << 8 | ch : ch;

    /// The atlas as a texture (created on first draw, not in headless tests).
    public ImageTexture Texture => _tex ??= ImageTexture.CreateFromImage(Atlas);

    /// Whether `code` is a glyph slot of this font (0 ≤ code < Capacity).
    public bool Has(int code) => code >= 0 && code < Capacity;

    /// The glyph's rect in the atlas (code 0 .. Capacity − 1; wrapped into that range otherwise).
    public Rect2 Src(int code)
    {
        code &= Capacity - 1;
        return new(code % Cols * StrideW + 1, code / Cols * StrideH + 1, GlyphW, GlyphH);
    }

    /// Raw 2-bit value (0 black, 2 white, 1/3 transparent) of one glyph pixel.
    public int Pixel(int code, int x, int y) => _px[(code & (Capacity - 1)) * BytesPerGlyph * 4 + y * GlyphW + x];

    /// (white, black) pixel counts of a glyph.
    public (int White, int Black) Ink(int code)
    {
        int w = 0, b = 0;
        for (int y = 0; y < GlyphH; y++) for (int x = 0; x < GlyphW; x++) { int v = Pixel(code, x, y); if (v == 2) w++; else if (v == 0) b++; }
        return (w, b);
    }

    /// One text row as an image: glyphs blended over `bg`, scaled ×`scale` (nearest) — for dumps / tests.
    public Image RenderRow(string s, Color bg, int scale = 3)
    {
        var img = Image.CreateEmpty(Math.Max(1, s.Length) * GlyphW, GlyphH, false, Image.Format.Rgba8);
        img.Fill(bg);
        for (int i = 0; i < s.Length; i++)
        {
            var r = Src(s[i]);
            img.BlendRect(Atlas, new Rect2I((int)r.Position.X, (int)r.Position.Y, GlyphW, GlyphH), new Vector2I(i * GlyphW, 0));
        }
        if (scale > 1) img.Resize(img.GetWidth() * scale, img.GetHeight() * scale, Image.Interpolation.Nearest);
        return img;
    }

    /// A whole OSD canvas as an image: every non-blank cell's glyph (font by `buffer.Font`) over `bg`,
    /// blink cells shown, scaled ×`scale` (nearest) — for evidence PNGs and tests. Codes outside the font
    /// are left out.
    public static Image RenderBuffer(OsdBuffer buffer, Color bg, int scale = 2)
    {
        var font = For(buffer.Font);
        var img = Image.CreateEmpty(buffer.Cols * GlyphW, buffer.Rows * GlyphH, false, Image.Format.Rgba8);
        img.Fill(bg);
        if (font != null)
            for (int r = 0; r < buffer.Rows; r++)
                for (int c = 0; c < buffer.Cols; c++)
                {
                    int g = buffer.GlyphAt(r, c);
                    if (g == ' ' || !font.Has(g)) continue;
                    var src = font.Src(g);
                    img.BlendRect(font.Atlas, new Rect2I((int)src.Position.X, (int)src.Position.Y, GlyphW, GlyphH), new Vector2I(c * GlyphW, r * GlyphH));
                }
        if (scale > 1) img.Resize(img.GetWidth() * scale, img.GetHeight() * scale, Image.Interpolation.Nearest);
        return img;
    }
}
