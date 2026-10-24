// Shared look of the non-flight screens: blueprint background, centered card, title, footer.
// Also: UI scale (root content_scale_factor), the check box / check button icons used everywhere,
// buttons that never take keyboard focus (flight screen), text fields that let go of it when they stop
// editing, the one file dialog, text-field focus test.
using System;
using System.Globalization;
using Godot;

namespace FlightBench;

public static class Ui
{
    public static readonly Color Blue = new(0.07f, 0.16f, 0.32f);
    public static readonly Color Dim = new(1, 1, 1, 0.5f);
    public static readonly Color Soft = new(1, 1, 1, 0.7f);

    public static string Version
    {
        get { var v = ProjectSettings.GetSetting("application/config/version").AsString(); return string.IsNullOrEmpty(v) ? "dev" : v; }
    }

    // ---- UI scale ----
    public static readonly float[] ScaleSteps = { 1f, 1.25f, 1.5f, 1.75f, 2f };

    /// FB_UI_SCALE=<number|auto> overrides the saved setting (testing).
    public static string? ScaleOverride => DevEnv.UiScale;

    /// Screen DPI / 96, clamped to 1..2, rounded to 0.25.
    public static float AutoScale()
    {
        var dpi = DisplayServer.ScreenGetDpi();
        var f = dpi > 0 ? dpi / 96f : 1f;
        return Mathf.Round(Mathf.Clamp(f, 1f, 2f) * 4f) / 4f;
    }

    /// "auto" (or anything unparsable) → AutoScale(); a number → that factor (0.5..3).
    public static float ResolveScale(string? setting)
    {
        if (setting != null && float.TryParse(setting, NumberStyles.Float, CultureInfo.InvariantCulture, out var f) && f > 0)
            return Mathf.Clamp(f, 0.5f, 3f);
        return AutoScale();
    }

    public static float CurrentScale => ResolveScale(ScaleOverride ?? AppSettings.Current.UiScale);

    /// Applies the current scale to the root window: with stretch mode "disabled" the root's 2D
    /// coordinate space becomes window size / factor, so every Control (and the embedded Windows)
    /// is drawn factor× larger while the 3D view still renders at the full window resolution.
    public static void ApplyScale(SceneTree tree)
    {
        tree.Root.ContentScaleFactor = CurrentScale;
        tree.Root.Theme = BaseTheme;
    }

    public static string FormatScale(float f) => f.ToString("0.0#", CultureInfo.InvariantCulture);

    /// Builds background + card + footer under the screen root; returns the card's column.
    /// Card is at most maxW × maxH, centered, shrinking with the window.
    public static VBoxContainer Screen(Control root, string title, float maxW, float maxH)
    {
        root.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.FullRect);
        root.Theme = MakeTheme();
        var bg = new BlueprintBackground(); bg.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.FullRect); root.AddChild(bg);
        var margin = new MarginContainer(); margin.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.FullRect);
        foreach (var s in new[] { "margin_left", "margin_right", "margin_top" }) margin.AddThemeConstantOverride(s, 24);
        margin.AddThemeConstantOverride("margin_bottom", 10);
        root.AddChild(margin);
        var outer = new VBoxContainer(); outer.AddThemeConstantOverride("separation", 8); margin.AddChild(outer);
        var host = new CardHost { MaxW = maxW, MaxH = maxH, SizeFlagsVertical = Control.SizeFlags.ExpandFill };
        outer.AddChild(host);
        var card = new PanelContainer();
        card.AddThemeStyleboxOverride("panel", Box(new Color(0.04f, 0.10f, 0.21f, 0.94f), new Color(0.40f, 0.56f, 0.82f, 0.55f), 8, 28, 22));
        host.AddChild(card);
        var col = new VBoxContainer(); col.AddThemeConstantOverride("separation", 10); card.AddChild(col);
        var t = new Label { Text = title }; t.AddThemeFontSizeOverride("font_size", 36); col.AddChild(t);
        outer.AddChild(new Label { Text = $"repo: {AppSettings.Current.RepoRoot}    ·    FlightBench {Version}", Modulate = Dim, HorizontalAlignment = HorizontalAlignment.Center, TextOverrunBehavior = TextServer.OverrunBehavior.TrimEllipsis });
        return col;
    }

    /// Row of natural-width controls; put Spacer() between groups.
    public static HBoxContainer Row(params Control[] items)
    {
        var h = new HBoxContainer(); h.AddThemeConstantOverride("separation", 10);
        foreach (var c in items) h.AddChild(c);
        return h;
    }

    public static Control Spacer() => new() { SizeFlagsHorizontal = Control.SizeFlags.ExpandFill };

    public static Button Btn(string text, Action onPressed)
    {
        var b = new Button { Text = text, CustomMinimumSize = new Vector2(120, 34) };
        b.Pressed += onPressed;
        return b;
    }

    public static Label Heading(string text) { var l = new Label { Text = text }; l.AddThemeFontSizeOverride("font_size", 20); return l; }

    /// Vertical scroll wrapper (no horizontal scrolling) for long forms.
    public static ScrollContainer Scroll(Control content)
    {
        var s = new ScrollContainer { HorizontalScrollMode = ScrollContainer.ScrollMode.Disabled, SizeFlagsVertical = Control.SizeFlags.ExpandFill, SizeFlagsHorizontal = Control.SizeFlags.ExpandFill };
        content.SizeFlagsHorizontal = Control.SizeFlags.ExpandFill;
        s.AddChild(content);
        return s;
    }

    public static StyleBoxFlat Box(Color bg, Color border, int radius, int padX, int padY)
    {
        var b = new StyleBoxFlat { BgColor = bg, BorderColor = border };
        b.SetBorderWidthAll(1); b.SetCornerRadiusAll(radius);
        b.ContentMarginLeft = b.ContentMarginRight = padX; b.ContentMarginTop = b.ContentMarginBottom = padY;
        return b;
    }

    // ---- theme ----
    private static Theme? _base;
    /// Check box / check button icons with a visible box on dark backgrounds. Set on the root window
    /// (inherited by the menu screens); the flight screen sets it on its bars and windows because
    /// a CanvasLayer breaks theme inheritance.
    public static Theme BaseTheme
    {
        get
        {
            if (_base != null) return _base;
            _base = new Theme();
            AddCheckIcons(_base);
            return _base;
        }
    }

    private const string BoxOff = "<rect x='1.5' y='1.5' width='13' height='13' rx='2.5' fill='#0a1830' stroke='#a9c1e8' stroke-width='1.5'/>";
    private const string BoxOn = "<rect x='1.5' y='1.5' width='13' height='13' rx='2.5' fill='#3d7fd9' stroke='#cfe0ff' stroke-width='1.5'/>" +
                                 "<path d='M4.3 8.2 L7 10.9 L11.9 5.3' fill='none' stroke='#ffffff' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/>";
    private const string RadioOff = "<circle cx='8' cy='8' r='6.5' fill='#0a1830' stroke='#a9c1e8' stroke-width='1.5'/>";
    private const string RadioOn = "<circle cx='8' cy='8' r='6.5' fill='#3d7fd9' stroke='#cfe0ff' stroke-width='1.5'/><circle cx='8' cy='8' r='2.8' fill='#ffffff'/>";
    private const string SwOff = "<rect x='1' y='2' width='30' height='14' rx='7' fill='#0a1830' stroke='#a9c1e8' stroke-width='1.5'/><circle cx='9' cy='9' r='4.5' fill='#a9c1e8'/>";
    private const string SwOn = "<rect x='1' y='2' width='30' height='14' rx='7' fill='#3d7fd9' stroke='#cfe0ff' stroke-width='1.5'/><circle cx='23' cy='9' r='4.5' fill='#ffffff'/>";

    private static DpiTexture Svg(int w, int h, string body, float opacity = 1f) =>
        DpiTexture.CreateFromString($"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{h}' viewBox='0 0 {w} {h}'><g opacity='{opacity.ToString(CultureInfo.InvariantCulture)}'>{body}</g></svg>");

    private static void AddCheckIcons(Theme t)
    {
        t.SetIcon("unchecked", "CheckBox", Svg(16, 16, BoxOff));
        t.SetIcon("checked", "CheckBox", Svg(16, 16, BoxOn));
        t.SetIcon("unchecked_disabled", "CheckBox", Svg(16, 16, BoxOff, 0.45f));
        t.SetIcon("checked_disabled", "CheckBox", Svg(16, 16, BoxOn, 0.45f));
        // a CheckBox in a ButtonGroup draws the radio icons
        t.SetIcon("radio_unchecked", "CheckBox", Svg(16, 16, RadioOff));
        t.SetIcon("radio_checked", "CheckBox", Svg(16, 16, RadioOn));
        t.SetIcon("radio_unchecked_disabled", "CheckBox", Svg(16, 16, RadioOff, 0.45f));
        t.SetIcon("radio_checked_disabled", "CheckBox", Svg(16, 16, RadioOn, 0.45f));
        t.SetIcon("unchecked", "Tree", Svg(16, 16, BoxOff));
        t.SetIcon("checked", "Tree", Svg(16, 16, BoxOn));
        t.SetIcon("unchecked_disabled", "Tree", Svg(16, 16, BoxOff, 0.45f));
        t.SetIcon("checked_disabled", "Tree", Svg(16, 16, BoxOn, 0.45f));
        foreach (var suffix in new[] { "", "_mirrored" })
        {
            t.SetIcon("unchecked" + suffix, "CheckButton", Svg(32, 18, SwOff));
            t.SetIcon("checked" + suffix, "CheckButton", Svg(32, 18, SwOn));
            t.SetIcon("unchecked_disabled" + suffix, "CheckButton", Svg(32, 18, SwOff, 0.45f));
            t.SetIcon("checked_disabled" + suffix, "CheckButton", Svg(32, 18, SwOn, 0.45f));
        }
    }

    private static Theme? _theme;
    private static Theme MakeTheme()
    {
        if (_theme != null) return _theme;
        _theme = new Theme();
        _theme.MergeWith(BaseTheme);                     // the same check icons
        // buttons: keep the default look, add side padding so wide labels are not flush with the edge
        var def = ThemeDB.GetDefaultTheme();
        foreach (var st in new[] { "normal", "hover", "pressed", "disabled", "focus", "hover_pressed" })
        {
            if (!def.HasStylebox(st, "Button")) continue;
            var sb = (StyleBox)def.GetStylebox(st, "Button").Duplicate();
            sb.ContentMarginLeft = sb.ContentMarginRight = 14;
            _theme.SetStylebox(st, "Button", sb);
        }
        var field = Box(new Color(0.02f, 0.06f, 0.14f), new Color(0.30f, 0.42f, 0.62f, 0.6f), 4, 6, 4);
        _theme.SetStylebox("panel", "Tree", field);
        _theme.SetStylebox("panel", "ItemList", field);
        _theme.SetStylebox("normal", "TextEdit", field);
        _theme.SetStylebox("normal", "LineEdit", field);
        _theme.SetStylebox("panel", "TabContainer", Box(new Color(0.03f, 0.08f, 0.17f), new Color(0.30f, 0.42f, 0.62f, 0.5f), 4, 12, 10));
        _theme.SetConstant("v_separation", "Tree", 6);
        _theme.SetConstant("line_separation", "ItemList", 6);
        return _theme;
    }

    // ---- focus / dialogs ----
    /// Buttons of every kind under `root` (now and added later) never take keyboard focus, so Space /
    /// Enter cannot re-press the last clicked one; text fields, spin boxes, sliders and trees keep it.
    /// Dialogs (file dialog) keep their own keyboard navigation.
    public static void NoFocusButtons(Node root)
    {
        void Apply(Node n)
        {
            if (n is BaseButton b && !InDialog(b)) b.FocusMode = Control.FocusModeEnum.None;
        }
        void Walk(Node n) { Apply(n); foreach (var c in n.GetChildren(true)) Walk(c); }
        Walk(root);
        var tree = root.GetTree();
        void OnAdded(Node n) { if (root.IsAncestorOf(n)) Apply(n); }
        tree.NodeAdded += OnAdded;
        root.TreeExiting += () => tree.NodeAdded -= OnAdded;
    }

    /// Text fields under `root` (now and added later, incl. a SpinBox's) give the keyboard back when they
    /// leave edit mode: LineEdit takes Escape (ui_cancel) / Enter itself to stop editing but keeps the focus,
    /// so the flight screen's Escape handler never sees it.
    public static void ReleaseTextOnUnedit(Node root)
    {
        void Apply(Node n)
        {
            if (n is LineEdit le && !InDialog(le)) le.EditingToggled += on => { if (!on && le.HasFocus()) le.ReleaseFocus(); };
        }
        void Walk(Node n) { Apply(n); foreach (var c in n.GetChildren(true)) Walk(c); }
        Walk(root);
        var tree = root.GetTree();
        void OnAdded(Node n) { if (root.IsAncestorOf(n)) Apply(n); }
        tree.NodeAdded += OnAdded;
        root.TreeExiting += () => tree.NodeAdded -= OnAdded;
    }

    private static bool InDialog(Node n)
    {
        for (var p = n.GetParent(); p != null; p = p.GetParent()) if (p is AcceptDialog) return true;
        return false;
    }

    /// Opens a file dialog (filesystem, one file) under `parent`; `picked` gets the path. The dialog frees
    /// itself when it closes (picked or cancelled). `dir`: the folder to start in when it exists.
    public static void PickFile(Node parent, string[] filters, string? dir, Action<string> picked)
    {
        var dlg = new FileDialog { FileMode = FileDialog.FileModeEnum.OpenFile, Access = FileDialog.AccessEnum.Filesystem, Filters = filters };
        if (!string.IsNullOrWhiteSpace(dir) && System.IO.Directory.Exists(dir)) dlg.CurrentDir = dir;
        dlg.FileSelected += p => picked(p);
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        parent.AddChild(dlg);
        dlg.PopupCentered(new Vector2I(900, 600));
    }

    /// A folder dialog (filesystem), same lifetime as PickFile.
    public static void PickDir(Node parent, string? dir, Action<string> picked)
    {
        var dlg = new FileDialog { FileMode = FileDialog.FileModeEnum.OpenDir, Access = FileDialog.AccessEnum.Filesystem, Title = "Pick a folder" };
        if (!string.IsNullOrWhiteSpace(dir) && System.IO.Directory.Exists(dir)) dlg.CurrentDir = dir;
        dlg.DirSelected += p => picked(p);
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        parent.AddChild(dlg);
        dlg.PopupCentered(new Vector2I(900, 600));
    }

    /// A save-file dialog, same lifetime as PickFile.
    public static void SaveFile(Node parent, string[] filters, string? dir, string name, Action<string> picked)
    {
        var dlg = new FileDialog { FileMode = FileDialog.FileModeEnum.SaveFile, Access = FileDialog.AccessEnum.Filesystem, Filters = filters, CurrentFile = name };
        if (!string.IsNullOrWhiteSpace(dir) && System.IO.Directory.Exists(dir)) dlg.CurrentDir = dir;
        dlg.FileSelected += p => picked(p);
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        parent.AddChild(dlg);
        dlg.PopupCentered(new Vector2I(900, 600));
    }

    /// The firmware folder for file dialogs: the setting, else the folder of `current`.
    public static string? FirmwareDir(string? current)
    {
        var d = AppSettings.Current.FirmwareDir;
        if (!string.IsNullOrWhiteSpace(d)) return d;
        try { return string.IsNullOrWhiteSpace(current) ? null : System.IO.Path.GetDirectoryName(current); } catch (Exception) { return null; }
    }

    /// Does a text field (a LineEdit in edit mode, incl. a SpinBox's, or a TextEdit) own the keyboard — in
    /// the focused embedded window, or in the root viewport when none is focused? A LineEdit that has the
    /// focus but left edit mode (Escape) does not take typed keys.
    public static bool TextFocus()
    {
        if (Engine.GetMainLoop() is not SceneTree tree) return false;
        var root = tree.Root;
        foreach (var w in root.GetEmbeddedSubwindows())
            if (w.HasFocus()) return IsText(w.GuiGetFocusOwner());
        return IsText(root.GuiGetFocusOwner());
        static bool IsText(Control? c) => c is LineEdit le ? le.IsEditing() : c is TextEdit;
    }
}

/// Blueprint blue with a faint grid, like the flight screen's ground.
public partial class BlueprintBackground : Control
{
    public override void _Ready() { MouseFilter = MouseFilterEnum.Ignore; Resized += QueueRedraw; }
    public override void _Draw()
    {
        DrawRect(new Rect2(Vector2.Zero, Size), Ui.Blue);
        const int step = 32;
        var minor = new Color(0.55f, 0.70f, 1f, 0.05f); var major = new Color(0.55f, 0.70f, 1f, 0.11f);
        for (int i = 0, x = 0; x <= Size.X; i++, x += step) DrawLine(new Vector2(x, 0), new Vector2(x, Size.Y), i % 5 == 0 ? major : minor);
        for (int i = 0, y = 0; y <= Size.Y; i++, y += step) DrawLine(new Vector2(0, y), new Vector2(Size.X, y), i % 5 == 0 ? major : minor);
    }
}

/// Centers its child at min(available, max) size (never below the child's minimum).
public partial class CardHost : Container
{
    public float MaxW = 700, MaxH = 600;
    public override void _Notification(int what)
    {
        if (what != NotificationSortChildren) return;
        foreach (var n in GetChildren())
        {
            if (n is not Control c || !c.Visible) continue;
            var min = c.GetCombinedMinimumSize();
            var sz = new Vector2(Mathf.Max(min.X, Mathf.Min(Size.X, MaxW)), Mathf.Max(min.Y, Mathf.Min(Size.Y, MaxH)));
            FitChildInRect(c, new Rect2(((Size - sz) / 2).Floor(), sz));
        }
    }
    public override Vector2 _GetMinimumSize() => Vector2.Zero;
}
