// Transmitter widgets shared by Settings → Transmitter and the flight screen's Transmitter window:
// channel names and a live channel bar (1000..2000 µs scale, centre tick, filled bar, µs text).
using Godot;

namespace FlightBench;

public static class RcNames
{
    /// Long label: "1 A roll", "2 E pitch", "3 C collective", "4 R yaw", "5 AUX1", "6 AUX2", …
    public static string Long(int ch)
    {
        string s = ch switch { 0 => "A roll", 1 => "E pitch", 2 => "C collective", 3 => "R yaw", _ => $"AUX{ch - 3}" };
        return $"{ch + 1} {s}";
    }
    /// Short label for compact bars: "1 A", "2 E", "3 C", "4 R", "5 AUX1", …
    public static string Short(int ch) => $"{ch + 1} " + (ch switch { 0 => "A", 1 => "E", 2 => "C", 3 => "R", _ => $"AUX{ch - 3}" });
}

/// Horizontal channel bar: 1000..2000 µs, filled from the left, tick at 1500, text "label  1234".
public partial class ChannelBar : Control
{
    private int _us = 1500;
    private string _label = "";
    public bool Compact;
    public bool Dimmed;   // e.g. source `none`

    public ChannelBar(string label = "", bool compact = false)
    {
        _label = label; Compact = compact;
        CustomMinimumSize = new Vector2(compact ? 110 : 160, compact ? 16 : 24);
        SizeFlagsHorizontal = SizeFlags.ExpandFill;
        MouseFilter = MouseFilterEnum.Ignore;
    }

    public void Set(int us) { if (us != _us) { _us = us; QueueRedraw(); } }
    public void SetLabel(string label) { if (label != _label) { _label = label; QueueRedraw(); } }
    public void SetDimmed(bool d) { if (d != Dimmed) { Dimmed = d; QueueRedraw(); } }
    /// Who last drove the channel ("joy" / "kb" / "ui", "" = nobody), drawn after the label.
    public string Mark { get; private set; } = "";
    public void SetMark(string mark) { if (mark != Mark) { Mark = mark; QueueRedraw(); } }

    public override void _Draw()
    {
        var r = new Rect2(Vector2.Zero, Size);
        DrawRect(r, new Color(0.08f, 0.13f, 0.22f));
        float f = RcMath.BarFraction(_us);
        var fill = Dimmed ? new Color(0.35f, 0.4f, 0.5f) : (_us < RcMath.MinUs || _us > RcMath.MaxUs ? new Color(0.95f, 0.35f, 0.25f) : new Color(0.95f, 0.6f, 0.15f));
        DrawRect(new Rect2(1, 1, (Size.X - 2) * f, Size.Y - 2), fill);
        DrawLine(new Vector2(Size.X / 2, 0), new Vector2(Size.X / 2, Size.Y), new Color(1, 1, 1, 0.8f), 1);
        DrawRect(r, new Color(0.45f, 0.55f, 0.75f), false, 1);
        var font = GetThemeDefaultFont();
        int size = Mathf.Max(8, GetThemeDefaultFontSize() - (Compact ? 4 : 1));
        float y = (Size.Y + font.GetAscent(size) - font.GetDescent(size)) / 2;
        var text = new Color(1, 1, 1, Dimmed ? 0.6f : 1f);
        var shadow = new Color(0, 0, 0, 0.75f);
        foreach (var (off, col) in new[] { (new Vector2(1, 1), shadow), (Vector2.Zero, text) })
        {
            if (_label.Length > 0) DrawString(font, new Vector2(4, y) + off, _label, HorizontalAlignment.Left, -1, size, col);
            if (Mark.Length > 0)
            {
                float x = 4 + (_label.Length > 0 ? font.GetStringSize(_label, HorizontalAlignment.Left, -1, size).X + 6 : 0);
                DrawString(font, new Vector2(x, y) + off, Mark, HorizontalAlignment.Left, -1, size, off == Vector2.Zero ? new Color(0.75f, 0.9f, 1f, Dimmed ? 0.6f : 1f) : col);
            }
            DrawString(font, new Vector2(0, y) + off, _us.ToString(), HorizontalAlignment.Right, Size.X - 4, size, col);
        }
    }
}
