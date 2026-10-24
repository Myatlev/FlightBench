// OSD: the firmware's MSP DisplayPort canvas (`osd` messages, docs/protocol.md) kept in an OsdBuffer and
// drawn by OsdPanel controls (the "OSD" window and the FPV overlay share one buffer).
// Buffer: rows×cols chars + one attr byte per cell (default 18×50; a message with other rows/cols
// resizes and clears it). `clear` wipes, then each cell's string is written from (r,c) onward, clipped
// at the row end (no wrap). `s` carries one char per cell whose code point (U+0000..U+00FF, JSON `\u00NN`
// for non-ASCII) is the firmware's byte = the glyph index; System.Text.Json already yields that char.
// Attr = the MSP_DP_WRITE_STRING byte, unchanged. RotorFlight 4.6: bits 0-5 (0x3F) font page, 0x40 blink,
// 0x80 version flag (ignored); page 0 = the loaded MCM, other pages are drawn with the same font (logged
// once per page: "font page N not loaded"). iNav 9.1: bits 0-1 = bits 8..9 of its 9-bit character code
// (glyph = (attr & 3) << 8 | byte, page 1 = the HUD), never blink. No invert bit in either.
// Font: the message's `font` ("rotorflight" | "inav") picks the atlas (OsdFont.For); absent = keep the
// current one (RotorFlight until a message names another; Reset on a new connection goes back to it). A
// change bumps Version, so every panel redraws the kept canvas in the new font at once.
// Frames: `frame` (the device's DRAW count) is not used — the core skips DRAWs that changed nothing, so it
// jumps; each message is applied as it comes (a delta onto the canvas, a full frame after a wipe), and
// nothing waits for a DRAW or a contiguous count: a missing message only means nothing changed.
// Blink (0x40): 2 Hz, 250 ms shown / 250 ms hidden, phased from the moment the cell was last written.
// With displayport_msp_use_device_blink OFF (default) the firmware blinks the element itself (omits it
// every other 250 ms phase, re-sends it with 0x40): each re-send restarts our phase, so our shown window
// coincides with the firmware's and the cell does not double-blink (a free-running wall-clock toggle
// could sit in anti-phase and hide the cell for good). With device blink ON the cell is sent once and
// stays; we then toggle it at 2 Hz from that write.
// Link: `"link": false` (with clear:true, cells:[]) = the firmware stopped drawing (power off, CLI mode,
// link_lost fault, contact lost): the last canvas is kept (not cleared) and drawn at 35% alpha with
// "NO SIGNAL" centred in the OSD font; the next message with link true/absent (a full frame) ends it.
// Panel: fixed grid of cells, cell aspect 2:3 (like a 12×18 analog glyph), the whole grid scaled to fit
// and centred. Glyphs: the cell's glyph index (OsdFont.GlyphIndex) in the MCM symbol font's atlas
// (OsdFont.cs: RotorFlight's 256 or iNav's 512): black + white pixels, transparent elsewhere; nearest
// filtering when a cell is at least 12 px wide, linear below. Fallback (no font file, or a code outside
// the font): a monospace system font, each glyph centred in its cell, white with a dark outline,
// non-ASCII codes of the font range as a placeholder box.
using System;
using System.Text;
using System.Text.Json;
using Godot;

namespace FlightBench;

public sealed class OsdBuffer
{
    public const int AttrPage = 0x3F, AttrBlink = 0x40, AttrVersion = 0x80, BlinkPhaseMs = 250;
    public const char Placeholder = '▯';   // white vertical rectangle
    public int Rows { get; private set; }
    public int Cols { get; private set; }
    private char[] _ch = Array.Empty<char>();
    private byte[] _attr = Array.Empty<byte>();
    private ulong[] _written = Array.Empty<ulong>();   // ms of the cell's last write: its blink phase
    private static readonly System.Collections.Generic.HashSet<int> PagesLogged = new();
    /// Clock for write stamps (tests replace it).
    public Func<ulong> Now = () => Time.GetTicksMsec();
    /// Bumped on every change (panels redraw when it moves).
    public int Version { get; private set; }
    public int BlinkCells { get; private set; }
    /// False after an `osd` message with `"link": false` until the next one with link true/absent.
    public bool Link { get; private set; } = true;
    public const string NoSignal = "NO SIGNAL";
    /// The font the codes index (`osd` message `font`): OsdFont.Rotorflight until a message names another.
    public string Font { get; private set; } = OsdFont.Rotorflight;
    /// The last `font` a message named on this connection (null: none yet — the core does not know the firmware).
    public string? FontSaid { get; private set; }
    private static readonly System.Collections.Generic.HashSet<string> FontsLogged = new();

    public OsdBuffer(int rows = 18, int cols = 50) => Resize(rows, cols);

    private void Resize(int rows, int cols)
    {
        Rows = Math.Clamp(rows, 1, 64); Cols = Math.Clamp(cols, 1, 128);
        _ch = new char[Rows * Cols]; _attr = new byte[Rows * Cols]; _written = new ulong[Rows * Cols];
        Clear();
    }

    public void Clear() { Array.Fill(_ch, ' '); Array.Clear(_attr); BlinkCells = 0; Version++; }

    /// A new connection: wiped, and the font back to the default (the new core names its own).
    public void Reset() { Clear(); SetFont(OsdFont.Rotorflight); FontSaid = null; }

    /// Switches the font the codes index; the canvas is kept and redrawn in it (Version bumped).
    public void SetFont(string font)
    {
        if (font == Font) return;
        if (font != OsdFont.Rotorflight && font != OsdFont.Inav && FontsLogged.Add(font))
            GD.Print($"[osd] font '{font}' unknown (drawn with RotorFlight's)");
        Font = font; Version++;
    }

    public char CharAt(int r, int c) => _ch[r * Cols + c];
    public int AttrAt(int r, int c) => _attr[r * Cols + c];
    /// The cell's glyph index in the current font (iNav: the page bits of attr on top of the byte).
    public int GlyphAt(int r, int c) => OsdFont.GlyphIndex(Font, _ch[r * Cols + c], _attr[r * Cols + c]);
    public ulong WrittenAt(int r, int c) => _written[r * Cols + c];
    public static int Page(int attr) => attr & AttrPage;
    /// A blink cell written at `written` is shown in the first 250 ms of every 500 ms from then on.
    public static bool BlinkShown(ulong written, ulong now) => now < written || (now - written) / BlinkPhaseMs % 2 == 0;
    /// Whether font page `page` (!= 0) has been seen and logged in this run.
    public static bool PageLogged(int page) => PagesLogged.Contains(page);

    /// `s` from (r,c) onward on row r, every char with `attr`; clipped at the row end.
    public void Write(int r, int c, string s, int attr)
    {
        if (r < 0 || r >= Rows) return;
        int page = Font == OsdFont.Inav ? Page(attr) & ~3 : Page(attr);   // iNav: pages 0..3 are part of the code
        if (page != 0 && PagesLogged.Add(page)) GD.Print($"[osd] font page {page} not loaded (drawn with page 0)");
        ulong now = Now();
        for (int i = 0; i < s.Length; i++)
        {
            int col = c + i;
            if (col < 0) continue;
            if (col >= Cols) break;
            int k = r * Cols + col;
            if ((_attr[k] & AttrBlink) != 0) BlinkCells--;
            _ch[k] = s[i]; _attr[k] = (byte)attr; _written[k] = now;
            if ((attr & AttrBlink) != 0) BlinkCells++;
        }
        Version++;
    }

    /// One `osd` message.
    public void Apply(JsonElement m)
    {
        int rows = m.TryGetProperty("rows", out var rv) && rv.ValueKind == JsonValueKind.Number ? rv.GetInt32() : Rows;
        int cols = m.TryGetProperty("cols", out var cv) && cv.ValueKind == JsonValueKind.Number ? cv.GetInt32() : Cols;
        if (rows != Rows || cols != Cols) Resize(rows, cols);
        if (m.TryGetProperty("font", out var fv) && fv.ValueKind == JsonValueKind.String && fv.GetString() is { Length: > 0 } font)
        { FontSaid = font; SetFont(font); }                  // absent: keep the current font
        bool link = !(m.TryGetProperty("link", out var lv) && lv.ValueKind == JsonValueKind.False);
        if (link != Link) { Link = link; Version++; }
        if (!link) return;                                   // keep the last canvas (dimmed) while the firmware is silent
        if (m.TryGetProperty("clear", out var cl) && cl.ValueKind == JsonValueKind.True) Clear();
        if (!m.TryGetProperty("cells", out var cells) || cells.ValueKind != JsonValueKind.Array) return;
        foreach (var cell in cells.EnumerateArray())
        {
            if (cell.ValueKind != JsonValueKind.Object) continue;
            int r = cell.TryGetProperty("r", out var x) && x.ValueKind == JsonValueKind.Number ? x.GetInt32() : -1;
            int c = cell.TryGetProperty("c", out var y) && y.ValueKind == JsonValueKind.Number ? y.GetInt32() : -1;
            var s = cell.TryGetProperty("s", out var sv) && sv.ValueKind == JsonValueKind.String ? sv.GetString() ?? "" : "";
            int a = cell.TryGetProperty("attr", out var av) && av.ValueKind == JsonValueKind.Number ? av.GetInt32() : 0;
            Write(r, c, s, a);
        }
    }

    /// The char as drawn by the text fallback: printable ASCII as is, other codes of the Betaflight font range as the placeholder.
    public static char Display(char ch) => ch >= 0x20 && ch < 0x7F ? ch : ch <= 0xFF ? Placeholder : ch;

    /// The text fallback for a glyph index of `font`: RotorFlight as Display; iNav: upper case, digits and
    /// plain punctuation as is, its symbols (0x60..0x7E, " $ ' ?, page 1..) as the placeholder.
    public static char Display(string font, int glyph)
    {
        if (font != OsdFont.Inav) return glyph <= 0xFFFF ? Display((char)glyph) : Placeholder;
        return glyph >= 0x20 && glyph < 0x60 && glyph is not (0x22 or 0x24 or 0x27 or 0x3F) ? (char)glyph : Placeholder;
    }

    public string RowText(int r)
    {
        var sb = new StringBuilder(Cols);
        for (int c = 0; c < Cols; c++) sb.Append(CharAt(r, c));
        return sb.ToString();
    }
}

public partial class OsdPanel : Control
{
    public OsdBuffer Buffer = new();
    /// A faint frame around the canvas (the window: shows the extent of an empty OSD).
    public bool ShowFrame;
    private int _drawn = -1;
    /// The buffer version / font of the last draw and how many cells it drew from the atlas / the text fallback (tests).
    public int DrawnVersion => _drawn;
    public string? DrawnFont { get; private set; }
    public int DrawnAtlas { get; private set; }
    public int DrawnFallback { get; private set; }
    private ulong _blinkTick;
    private static Font? _mono;
    /// Another .mcm to draw with instead of the one the buffer's `font` picks (null: by the firmware).
    /// Read as RotorFlight's layout (256 glyphs, glyph = byte) unless the buffer's font is iNav's.
    public string? FontPath;

    private static Font Mono()
    {
        if (_mono != null) return _mono;
        var sf = new SystemFont { FontNames = new[] { "Consolas", "DejaVu Sans Mono", "Liberation Mono", "Courier New", "monospace" } };
        sf.Fallbacks = new Godot.Collections.Array<Font> { ThemeDB.FallbackFont };
        _mono = sf;
        return _mono;
    }

    public override void _Ready()
    {
        MouseFilter = MouseFilterEnum.Ignore;
        SizeFlagsHorizontal = SizeFlags.ExpandFill; SizeFlagsVertical = SizeFlags.ExpandFill;
        CustomMinimumSize = new Vector2(200, 72);
    }

    public override void _Process(double delta)
    {
        if (!IsVisibleInTree()) return;
        ulong tick = Time.GetTicksMsec() / 50;                  // blink cells have their own phases: re-check every 50 ms
        var g = GridRect(Size, Buffer.Rows, Buffer.Cols);
        var filter = g.Size.X / Buffer.Cols >= OsdFont.GlyphW ? TextureFilterEnum.Nearest : TextureFilterEnum.Linear;
        if (TextureFilter != filter) TextureFilter = filter;
        if (Buffer.Version != _drawn || (Buffer.BlinkCells > 0 && tick != _blinkTick)) { _blinkTick = tick; QueueRedraw(); }
    }

    /// The grid rect inside `size`: cells 2:3, scaled to fit, centred.
    public static Rect2 GridRect(Vector2 size, int rows, int cols)
    {
        float cw = Mathf.Min(size.X / cols, size.Y / rows / 1.5f);
        var g = new Vector2(cw * cols, cw * 1.5f * rows);
        return new Rect2((size - g) / 2, g);
    }

    public override void _Draw()
    {
        _drawn = Buffer.Version;
        var b = Buffer;
        DrawnFont = b.Font; DrawnAtlas = DrawnFallback = 0;
        var grid = GridRect(Size, b.Rows, b.Cols);
        float cw = grid.Size.X / b.Cols, ch = grid.Size.Y / b.Rows;
        if (cw < 2) return;
        if (ShowFrame) DrawRect(grid, new Color(1, 1, 1, 0.15f), false, 1);
        var sym = FontPath == null ? OsdFont.For(b.Font) : OsdFont.Load(FontPath, b.Font == OsdFont.Inav ? OsdFont.InavGlyphs : OsdFont.Glyphs);
        Font? font = null; int fs = 0, outline = 0; float ascent = 0, height = 0;
        ulong now = Time.GetTicksMsec();
        float alpha = b.Link ? 1f : 0.35f;                   // no link: the last canvas, dimmed
        for (int r = 0; r < b.Rows; r++)
            for (int c = 0; c < b.Cols; c++)
            {
                int glyph = b.GlyphAt(r, c), attr = b.AttrAt(r, c);
                if (glyph == ' ') continue;                 // blank (iNav: a page-1 0x20 is 0x120, a glyph)
                if ((attr & OsdBuffer.AttrBlink) != 0 && !OsdBuffer.BlinkShown(b.WrittenAt(r, c), now)) continue;
                // RotorFlight: font page (attr & 0x3F): only page 0 exists, other pages use the same glyphs (OsdBuffer
                // logs them); iNav: the page is part of the glyph index
                DrawGlyph(r, c, glyph, alpha);
            }
        if (!b.Link)
        {
            int r0 = b.Rows / 2, c0 = Math.Max(0, (b.Cols - OsdBuffer.NoSignal.Length) / 2);
            for (int i = 0; i < OsdBuffer.NoSignal.Length && c0 + i < b.Cols; i++)
                if (OsdBuffer.NoSignal[i] != ' ') DrawGlyph(r0, c0 + i, OsdBuffer.NoSignal[i], 1f);
        }

        void DrawGlyph(int r, int c, int glyph, float a)
        {
            var cell = new Rect2(grid.Position + new Vector2(c * cw, r * ch), new Vector2(cw, ch));
            if (sym != null && sym.Has(glyph))
            {
                DrawTextureRectRegion(sym.Texture, cell, sym.Src(glyph), new Color(1, 1, 1, a));
                DrawnAtlas++;
                return;
            }
            // text fallback
            DrawnFallback++;
            if (font == null)
            {
                font = Mono();
                // font size: the cell height, shrunk if the measured glyph advance is wider than the cell
                fs = Mathf.Max(6, (int)ch);
                float adv = font.GetCharSize('M', fs).X;
                if (adv > cw * 0.95f) fs = Mathf.Max(6, (int)(fs * cw * 0.95f / adv));
                ascent = font.GetAscent(fs); height = font.GetHeight(fs);
                outline = Mathf.Max(2, fs / 6);
            }
            var s = OsdBuffer.Display(b.Font, glyph).ToString();
            float w = font.GetStringSize(s, HorizontalAlignment.Left, -1, fs).X;
            var pos = new Vector2(cell.Position.X + (cw - w) / 2, cell.Position.Y + (ch - height) / 2 + ascent);
            DrawStringOutline(font, pos, s, HorizontalAlignment.Left, -1, fs, outline, new Color(0, 0, 0, 0.85f * a));
            DrawString(font, pos, s, HorizontalAlignment.Left, -1, fs, new Color(1, 1, 1, a));
        }
    }
}
