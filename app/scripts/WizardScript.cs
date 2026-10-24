// "Firmware config at first start" (wizard, summary step): what the core types / places into the firmware the first time
// this bench starts, as LAYERS with provenance (ConfigLayers.cs, LayersView.cs; core branch `config-layers`) — read from
// a session-check run of the bench as it is on the pages (a temp session file). Every line shows its layer (1 board ·
// 2 aircraft · 3 profile · 4 device · 5 bench) and its source; board / aircraft lines open their file in an inline editor (a preset's
// bundled file is copied next to the bench first), device lines can be disabled (`disabled_lines`) or overridden in the
// bench lines, and the bench lines (layer 4) are an editable box = the `bench_cli` file next to the bench. Conflicts (one
// setting in two layers) are amber: the later layer wins. A core that does not report layers: the app infers them
// (marked "inferred"). Nothing hidden: no dev knob makes anything editable that is not editable here.
using System;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

public partial class Wizard
{
    private LayersView _layers = null!;
    private Button _scriptCopy = null!;
    private Task<(LayeredConfig? Config, string Error)>? _scriptTask;
    private string _scriptFor = "";
    private bool _scriptAgain;
    /// The last layered config shown (self-tests / screenshots).
    public LayeredConfig? Script => _layers.Config;
    public string ScriptInfo => _layers.InfoText;
    public string ScriptText => _layers.Config?.Text ?? "";
    public bool ScriptBusy => _scriptTask != null;
    public LayersView Layers => _layers;

    private Control BuildScriptGroup()
    {
        var panel = new PanelContainer();
        panel.AddThemeStyleboxOverride("panel", Ui.Box(new Color(0.02f, 0.06f, 0.14f), new Color(0.30f, 0.42f, 0.62f, 0.6f), 4, 12, 10));
        var v = new VBoxContainer();
        v.AddThemeConstantOverride("separation", 4);
        panel.AddChild(v);
        _scriptCopy = Ui.Btn("Copy", () => { DisplayServer.ClipboardSet(ScriptText); GD.Print($"[app] wizard: the first-start config copied ({ScriptText.Length} chars)"); });
        _scriptCopy.TooltipText = "copy the assembled script (the enabled lines, in order) to the clipboard";
        var again = Ui.Btn("Check again", () => { _scriptFor = ""; StartScriptCheck(); });
        again.TooltipText = "run the core's session-check on the bench as it is now";
        v.AddChild(Ui.Row(new Label { Text = "Firmware config at first start — by layer (1 board · 2 aircraft · 3 profile · 4 device · 5 bench; a later layer wins)" }, Ui.Spacer(), again, _scriptCopy));
        _layers = new LayersView { BenchOf = () => _b, Root = CoreLocator.DataRoot };
        _layers.CheckRequested += () => { _scriptFor = ""; StartScriptCheck(); };
        _layers.BenchChanged += () => { if (_step == _pages.Count - 1) FillSummary(); };
        _layers.Logged += s => GD.Print("[app] wizard: " + s);
        v.AddChild(_layers);
        return panel;
    }

    /// Entering the summary: session-check on the bench as it is now (again only when it or its bench lines changed).
    private void StartScriptCheck()
    {
        var key = JsonSerializer.Serialize(_b.ToSession(CoreLocator.DataRoot), Json.Files) + "|" + BenchFiles.ReadBenchLines(_b);
        if (_scriptTask != null) { if (key != _scriptFor) _scriptAgain = true; return; }
        if (key == _scriptFor) return;
        var exe = CoreLocator.Find(out var src);
        if (exe == null && LayeredConfig.Checker == null) { SetScript(null, $"no core to ask ({src}): Settings → Paths → core"); return; }
        _scriptFor = key;
        _layers.SetBusy();
        _scriptTask = LayeredConfig.CheckAsync(_b.Clone(), exe, CoreLocator.DataRoot);
    }

    public override void _Process(double delta)
    {
        if (_scriptTask is not { IsCompleted: true } t) return;
        _scriptTask = null;
        SetScript(t.Result.Config, t.Result.Error);
        if (_scriptAgain) { _scriptAgain = false; _scriptFor = ""; StartScriptCheck(); }
    }

    private void SetScript(LayeredConfig? c, string error)
    {
        _layers.Show(c, error);
        _scriptCopy.Disabled = c == null || !c.Enabled.Any();
    }
}
