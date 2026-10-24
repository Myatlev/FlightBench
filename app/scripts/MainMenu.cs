// Main screen: bundled presets (Presets.cs: <data root>/presets/*.json; double-click / Open preset = copy into the
// benches folder and fly), recent benches (double-click = load and start), then Load bench · Edit bench ·
// New bench · Settings · Exit. First run (no benches): the presets come first, with "Start with a preset".
// FB_BENCH=<name> starts a bench at once, FB_PRESET=<id|name> opens a preset at once (scripted runs).
using System;
using System.Collections.Generic;
using System.Linq;
using Godot;

namespace FlightBench;

public partial class MainMenu : Control
{
    public override void _Ready()
    {
        var col = Ui.Screen(this, "FlightBench", 760, 680);
        col.AddChild(new Label { Text = "a bench for flight-controller firmware developers", Modulate = Ui.Soft });
        col.AddChild(new Control { CustomMinimumSize = new Vector2(0, 6) });

        var benches = BenchData.ListAll();
        var presetErrors = new List<string>();
        var presets = Presets.List(presetErrors);
        bool firstRun = benches.Count == 0;

        // ---- presets ----
        var presetBox = new VBoxContainer();
        presetBox.AddThemeConstantOverride("separation", 6);
        presetBox.AddChild(Ui.Heading("Presets"));
        if (firstRun && presets.Count > 0)
        {
            var hint = new Label { Text = "Start with a preset: it opens a ready bench (bundled firmware, board config, RS7 wiring) and starts the core." };
            hint.AutowrapMode = TextServer.AutowrapMode.WordSmart;
            hint.AddThemeFontSizeOverride("font_size", 17);
            hint.Modulate = new Color(0.62f, 0.86f, 1f);
            presetBox.AddChild(hint);
        }
        var plist = new Tree { Columns = 2, HideRoot = true, ColumnTitlesVisible = false, SelectMode = Tree.SelectModeEnum.Row, CustomMinimumSize = new Vector2(0, firstRun ? 110 : 78) };
        plist.SetColumnExpandRatio(0, 2); plist.SetColumnExpandRatio(1, 5);
        var proot = plist.CreateItem();
        foreach (var p in presets)
        {
            var it = plist.CreateItem(proot);
            it.SetText(0, p.Name); it.SetText(1, p.Description);
            it.SetTooltipText(0, p.File); it.SetTooltipText(1, p.Description);
            it.SetCustomColor(1, Ui.Soft);
        }
        if (presets.Count == 0)
        {
            var it = plist.CreateItem(proot);
            it.SetText(0, "(no presets)"); it.SetText(1, $"none in {Presets.Dir}{(presetErrors.Count > 0 ? " — " + string.Join("; ", presetErrors) : "")}");
            it.SetCustomColor(0, Ui.Dim); it.SetCustomColor(1, Ui.Dim);
            for (int c = 0; c < 2; c++) it.SetSelectable(c, false);
        }
        Preset? SelectedPreset() { var it = plist.GetSelected(); int i = it?.GetIndex() ?? -1; return i >= 0 && i < presets.Count ? presets[i] : null; }
        plist.ItemActivated += () => { if (SelectedPreset() is { } p) OpenPreset(p); };
        if (presets.Count > 0) proot.GetChild(0).Select(0);
        presetBox.AddChild(plist);
        var openPreset = Ui.Btn("Open preset", () => { if (SelectedPreset() is { } p) OpenPreset(p); });
        openPreset.TooltipText = "copies the preset into your benches (never changes the bundled one) and starts it; a bench of that name that exists is opened as it is";
        openPreset.Disabled = presets.Count == 0;
        presetBox.AddChild(Ui.Row(openPreset, new Label { Text = "double-click a preset to fly it", Modulate = Ui.Dim }));

        // ---- recent benches ----
        var recentBox = new VBoxContainer { SizeFlagsVertical = SizeFlags.ExpandFill };
        recentBox.AddThemeConstantOverride("separation", 6);
        recentBox.AddChild(Ui.Heading("Recent benches"));
        // columns: name · board / aircraft · last used
        var recent = new Tree { Columns = 3, HideRoot = true, ColumnTitlesVisible = true, SelectMode = Tree.SelectModeEnum.Row, SizeFlagsVertical = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(0, 110) };
        recent.SetColumnTitle(0, "name"); recent.SetColumnTitle(1, "board / aircraft"); recent.SetColumnTitle(2, "last used");
        recent.SetColumnExpandRatio(0, 3); recent.SetColumnExpandRatio(1, 3); recent.SetColumnExpandRatio(2, 2);
        for (int c = 0; c < 3; c++) recent.SetColumnTitleAlignment(c, HorizontalAlignment.Left);
        var rootItem = recent.CreateItem();
        foreach (var b in benches)
        {
            var it = recent.CreateItem(rootItem);
            it.SetText(0, b.Name); it.SetText(1, $"{b.Board} / {b.Aircraft}");
            it.SetText(2, b.LastUsed == DateTime.MinValue ? "never" : b.LastUsed.ToString("yyyy-MM-dd HH:mm"));
            it.SetCustomColor(1, Ui.Soft); it.SetCustomColor(2, Ui.Soft);
        }
        if (benches.Count == 0)
        {
            var it = recent.CreateItem(rootItem);
            it.SetText(0, presets.Count > 0 ? "(no benches yet)" : "(no benches yet — create one)"); it.SetCustomColor(0, Ui.Dim);
            for (int c = 0; c < 3; c++) it.SetSelectable(c, false);
        }
        BenchData? Selected() { var it = recent.GetSelected(); int i = it?.GetIndex() ?? -1; return i >= 0 && i < benches.Count ? benches[i] : null; }
        recent.ItemActivated += () => { if (Selected() is { } b) Start(b); };
        if (benches.Count > 0) rootItem.GetChild(0).Select(0);
        recentBox.AddChild(recent);

        // first run: presets first (prominent); otherwise the recent benches first
        if (firstRun) { col.AddChild(presetBox); col.AddChild(recentBox); recent.CustomMinimumSize = new Vector2(0, 60); }
        else { col.AddChild(recentBox); col.AddChild(presetBox); }

        col.AddChild(new Control { CustomMinimumSize = new Vector2(0, 4) });
        var edit = Ui.Btn("Edit bench", () => { if (Selected() is { } b) Wizard.Open(GetTree(), b, edit: true); });
        edit.TooltipText = "the wizard with the selected bench: the same name saves over it, another name makes a new bench";
        edit.Disabled = benches.Count == 0;
        var load = Ui.Btn("Load bench", () => { if (Selected() is { } b) Start(b); });
        load.Disabled = benches.Count == 0;
        var del = Ui.Btn("Delete…", () => { if (Selected() is { } b) AskDelete(b); });
        del.Name = "delete";
        del.TooltipText = "delete the selected bench: its bench file, its session file and its saved firmware config (bench state); bundled presets stay as they are";
        del.Disabled = benches.Count == 0;
        col.AddChild(Ui.Row(
            load,
            edit,
            del,
            Ui.Btn("New bench", () => Wizard.Open(GetTree(), BenchData.New(), edit: false)),
            Ui.Btn("Settings", () => GetTree().ChangeSceneToFile("res://scenes/Settings.tscn")),
            Ui.Spacer(),
            Ui.Btn("Exit", () => GetTree().Quit())));

        // FB_DIALOG=delete: the Delete… confirmation for the selected bench (screenshots; never confirmed)
        if (DevEnv.Dialog == "delete" && Selected() is { } sel && DevEnv.Bench == null && DevEnv.Preset == null) CallDeferred(nameof(AskDeleteDeferred), sel.Name);

        // FB_BENCH=<name> starts that bench at once, FB_PRESET=<id|name> opens that preset (scripted runs / smoke tests)
        var auto = DevEnv.Bench;
        if (!string.IsNullOrEmpty(auto))
        {
            var b = benches.Find(x => x.Name == auto) ?? BenchData.Load(BenchData.PathFor(auto));
            if (b != null) Start(b);
            else GD.Print($"FB_BENCH={auto}: bench not found");
        }
        else if (DevEnv.Preset is { } pre)
        {
            var p = presets.Find(x => string.Equals(x.Id, pre, StringComparison.OrdinalIgnoreCase) || x.Name == pre);
            if (p != null) OpenPreset(p);
            else GD.Print($"FB_PRESET={pre}: no such preset in {Presets.Dir} ({string.Join(", ", presets.ConvertAll(x => x.Id))})");
        }
    }

    private void AskDeleteDeferred(string name) { if (BenchData.ListAll().Find(b => b.Name == name) is { } b) AskDelete(b); }

    /// "Delete…": a confirmation naming the bench file (and what else goes with it), then BenchData.Delete and a fresh menu.
    public ConfirmationDialog AskDelete(BenchData b)
    {
        var dlg = DeleteDialog(b);
        dlg.Confirmed += () =>
        {
            Flight.ReleaseKeptCore(b.Name);                  // a core kept running for it (Settings → keep running) is stopped first
            bool ok = BenchData.Delete(b.Name, out var gone, out var err);
            GD.Print($"[app] delete bench '{b.Name}': {(gone.Count > 0 ? "deleted " + string.Join(", ", gone) : "nothing on disk")}{(ok ? "" : "; " + err)}");
            GetTree().CallDeferred(SceneTree.MethodName.ChangeSceneToFile, "res://scenes/MainMenu.tscn");
        };
        AddChild(dlg);
        dlg.PopupCentered();
        return dlg;
    }

    /// The confirmation shared by the main menu and the flight screen's Bench → Delete bench…: names the bench file and the
    /// session / bench-state files that go with it.
    public static ConfirmationDialog DeleteDialog(BenchData b)
    {
        var file = ProjectSettings.GlobalizePath(b.FilePath).Replace('\\', '/');
        var others = BenchData.FilesOf(b.Name).Where(f => f != file).ToList();
        var dlg = new ConfirmationDialog
        {
            Title = "Delete bench", Theme = Ui.BaseTheme, OkButtonText = "Delete",
            DialogText = $"Delete the bench '{b.Name}'?\n\n{file}"
                         + (others.Count > 0 ? "\nand with it:\n" + string.Join("\n", others) : "")
                         + "\n\nIts saved firmware config (calibrations, modes set in the configurator) goes too. Bundled presets are not touched: opening the preset again makes a fresh copy.",
        };
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        return dlg;
    }

    /// Copies the preset into the benches (or takes the existing bench of that name) and starts it.
    private void OpenPreset(Preset p)
    {
        var b = Presets.Open(p, out _, out var note);
        if (b == null) { GD.Print("[app] " + note); return; }
        AppState.Notes.Add(note);                // the flight screen logs it (window + stdout)
        Start(b);
    }

    private void Start(BenchData b)
    {
        b.LastUsed = DateTime.Now;
        if (DevEnv.SaveBench) b.Save();   // headless / layout-knob / self-test runs leave the bench file alone
        AppState.Bench = b;
        // may be called from _Ready (FB_BENCH): scene changes must be deferred there
        GetTree().CallDeferred(SceneTree.MethodName.ChangeSceneToFile, "res://scenes/Flight.tscn");
    }
}
