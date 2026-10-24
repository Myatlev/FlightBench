// Flight screen, the firmware's read-only views (docs/ui.md; product decisions 2026-10-01: modes are the firmware's,
// written only by the configurator; nothing editable in the release UI):
// - the Transmitter window is the radio (decisions 2026-10-23, TxModel.cs): one row per channel 5..16 that has a switch,
//   in channel order — a static switch of the bench's transmitter model (`switch2` / `switch3` / `switch6`, always),
//   or one derived from the firmware's `modes` on an `auto` channel (Modes.cs; once the firmware gives ranges there).
//   The positions are buttons labelled with the firmware's boxes active at them; clicking a position or the row's key
//   (switch_1..switch_5, 5..9: the N-th row) sends that position's µs on the channel (RcInput.Override: channels whose
//   source is `ui` or `none`; a channel on the radio is shown, not driven). The header says where the modes came from
//   ("from firmware (t = …)" / "from bench snapshot") and which channels are bench switches; Update modes sends
//   {"type":"modes"}. Every `modes` message is saved in the bench (`modes`, informational) and refreshes its
//   informational `switches`; it relabels a static switch, never rebuilds it.
//   The app subscribes (`subscribe {"modes":true}` after every hello, CoreSession) so the core keeps them current and
//   replays the last read to a late client; a `modes` error (`cli`, `power_off`) is logged, the switches stay.
// - the Functions window (FunctionsPanel: the aircraft's functions from the manifest, their progress incl. the core's
//   replay to a late client, EnvPanels.cs) and the Config window (ConfigPanel, ConfigWindow.cs / Flight.Config.cs: the
//   script by layer, the firmware's diff — DiffView: the cached diff by default; Refresh from firmware reboots it —,
//   Changes, the operations);
// - Settings over the bench (top bar → Settings): the settings page in a window, Back closes it, the core stays.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class Flight
{
    private ModesSnapshot? _modes;
    private bool _modesLive;                       // _modes came from the core in this session (else the bench's snapshot)
    private List<DerivedSwitch> _switches = new();
    private Label? _modesHeader;
    private Button? _updateModesBtn;
    private VBoxContainer? _switchList;
    private string? _osdText => _fc.Osd;
    private readonly List<(DerivedSwitch Sw, Button[] Pos, Label Key)> _switchRows = new();
    private readonly bool[] _switchSet = new bool[16];   // a switch position the user picked on this channel (click / key) this session
    private bool? _fcArmed => _fc.Armed;
    private FunctionsPanel? _functions;
    private DiffView? _diffView;
    private ConfigPanel? _configPanel;
    private Window? _settingsWin;
    private Settings? _settingsPage;
    private int _disconnects;                      // "disconnected" changes seen (the topbar test: Settings must not disconnect)
    private bool _diffLive;                        // a `diff` with text came from the core in this session

    /// Transmitter window: header (source of the modes + Update modes), the derived switches, the static layout.
    private Control BuildModesSwitches()
    {
        var box = new VBoxContainer();
        var head = new HBoxContainer();
        _modesHeader = new Label { AutowrapMode = TextServer.AutowrapMode.WordSmart, SizeFlagsHorizontal = Control.SizeFlags.ExpandFill, Modulate = Ui.Soft };
        _updateModesBtn = new Button { Text = "Update modes", FocusMode = Control.FocusModeEnum.None, TooltipText = "{\"type\":\"modes\"}: the core reads the firmware's mode boxes and ranges over MSP again (read-only; set them in the configurator)" };
        _updateModesBtn.Pressed += RequestModes;
        head.AddChild(_modesHeader); head.AddChild(_updateModesBtn);
        box.AddChild(head);
        _switchList = new VBoxContainer();
        box.AddChild(_switchList);
        if (BuildOrderLine() is { } order) box.AddChild(order);   // the bench's switchOrder (Flight.Plane.cs)
        return box;
    }

    /// Functions + Config windows.
    private void BuildEnvWindows()
    {
        _functions = new FunctionsPanel { Send = m => _session.Send(m) };
        _functions.Logged += s => Log("app", s);
        _functions.FirstStartChanged += (name, on) =>
        {
            (_bench.FirstStartFunctions ??= new())[name] = on;
            Log("app", $"function {name}: run at first start {(on ? "on" : "off")} (saved in the bench; informational — the core decides the first-start set)");
        };
        RefreshFunctionsCatalog();
        _windows.Make("Functions", _functions, new Vector2I(420, 120), new Vector2I(700, 280), minSize: new Vector2I(460, 180));
        _diffView = new DiffView();
        _diffView.Logged += s => Log("app", s);
        _diffView.RefreshRequested += RequestDiff;
        if (_bench.FirmwareDiff is { } d) _diffView.Show(d, fromBench: true);
        BuildConfigPanel();                                  // the Config window: layers + diff + changes + operations (Flight.Config.cs)
        _windows.Make("Config", _configPanel!, new Vector2I(360, 110), new Vector2I(1060, 640), minSize: new Vector2I(640, 360));
    }

    private void RefreshFunctionsCatalog()
    {
        _functions?.SetCatalog(Manifest.Current.FunctionsFor(_bench.Aircraft), _bench.FirstStartFunctions, $"the manifest's aircraft '{_bench.Aircraft}' ({Manifest.Current.Source})");
        _configPanel?.SetFunctions(Manifest.Current.FunctionsFor(_bench.Aircraft));
    }

    /// The core's new messages + the bench's snapshots (called from _Ready after the windows exist).
    private void HookModes()
    {
        _core.ModesReceived += OnModes;
        _core.DiffReceived += OnDiff;
        _core.FunctionReceived += m =>
        {
            if (_functions?.OnFunction(m) is { } l) Log("function", l);
            _plane.Config.OnFunction(m);                         // a first-start function: the Status config line's k/n
            UpdateStatus();
        };
        _core.ConnectionChanged += s =>
        {
            if (s != "connected" && !s.StartsWith("connecting")) _disconnects++;
            if (s == "connected") { _functions?.ClearProgress(); if (_modes != null) _modesLive = false; }
            _functions?.SetConnected(_session.Connected);
            _diffView?.SetRefreshState(_session.Connected, _fcArmed);
            _configPanel?.SetState(_session.Connected, _fcArmed);
            UpdateModesHeader();
        };
        if (_bench.Modes is { } snap) ApplyModes(snap, live: false);
        else DeriveSwitches();                                   // the bench's static switches before any modes
    }

    // ---- modes ----
    private void RequestModes()
    {
        if (_session.Send(new { type = "modes" })) Log("app", "modes: asked the core to read the firmware's modes again");
    }

    private void OnModes(JsonElement m)
    {
        // the plane / launch self-tests check the screen without modes (an all-auto bench: no switch row) against a
        // fake core that sends its own modes: they are logged and left out there
        if (PlaneTest != PlaneTestMode.None) { Log("app", "modes: ignored on the plane self-test screen (it tests the screen without modes)"); return; }
        if (Json.Str(m, "error") is { } err)
        {
            Log("core", $"modes: not read ({err}{(err == "cli" ? ": the firmware is in its CLI, exit CLI first" : err == "power_off" ? ": the battery is off" : "")}); the switches stay as they were");
            return;
        }
        var snap = ModesSnapshot.FromMessage(m, DateTime.Now);
        if (snap == null) { Log("app", "modes: a message without boxes ignored: " + m.GetRawText()); return; }
        bool variantNew = snap.Variant != null && snap.Variant != _modes?.Variant;
        ApplyModes(snap, live: true);
        if (variantNew) UpdateCfgHint();
        Log("core", $"modes: {snap.Boxes.Count} boxes, {snap.Ranges.Count} ranges → {(_switches.Count == 0 ? "no switch (no aux ranges)" : string.Join("; ", _switches.Select(s => s.Describe() + $" [at {s.Positions[s.PositionOf(_rc.ChannelsUs[s.Channel - 1])].Label}{(Drivable(s.Channel - 1) ? "" : ", on the radio")}]")))}");
    }

    /// New modes (live from the core, or the bench's snapshot at start): switches rebuilt, channels the app drives set
    /// to their position; a live one is saved into the bench with the static `switches` refreshed from it.
    public void ApplyModes(ModesSnapshot snap, bool live)
    {
        _modes = snap; _modesLive = live;
        if (live)
        {
            var old = _bench.Switches;
            _bench.Modes = snap;
            if (snap.AsBenchSwitches() is { } fresh)
            {
                foreach (var s in fresh) if (old?.Find(o => o.Channel == s.Channel)?.Note is { } note && !note.Contains("from the firmware's modes")) s.Note = note + "; " + s.Note;
                // the static layout follows the firmware (product decision: modes are the firmware's); say so when it changes
                string L(IEnumerable<BenchSwitch>? l) => string.Join("; ", (l ?? Enumerable.Empty<BenchSwitch>()).Select(x => $"ch{x.Channel} {x.Name}"));
                if (L(old) != L(fresh)) Log("app", $"bench switches: {(old == null ? "none" : L(old))} → {L(fresh)} (refreshed from the firmware's modes)");
                _bench.Switches = fresh;
            }
            if (DevEnv.SaveBench && !_deleted) _bench.Save();
        }
        DeriveSwitches();
    }

    /// The Transmitter rows (the bench's transmitter model + the current modes, TxModel.Rows); the channels the app drives
    /// set to a position: kept where the user put them, else a static switch at its `start`, a derived one where the
    /// channel is — never an ARM position nobody asked for.
    private void DeriveSwitches()
    {
        _rc.Update(0);                                           // the channels as the app would send them now (modes may come before the first frame's read)
        _switches = TxModel.Rows(TxModel.Effective(_bench.Transmitter, _bench.Board), _modes);
        int armId = _modes?.Boxes.Find(b => b.Name == "ARM")?.Id ?? 0;
        for (int ch = 4; ch < 16; ch++)
        {
            var sw = _switches.Find(s => s.Channel - 1 == ch);
            // no row (or the radio drives it): released, and the user's earlier pick forgotten — a row that comes back
            // later (Edit bench, new modes, the source back to `ui`) starts as nothing was asked for, never where the
            // channel's idle 1500 happens to fall (a switch2 throttle would come back on, a derived ARM armed)
            if (sw == null || !Drivable(ch)) { _rc.Override[ch] = 0; _rc.OverrideBy[ch] = RcOwner.None; _switchSet[ch] = false; continue; }
            // "asked for": a position the user picked on this channel (click / key) — not a value the app chose itself
            // (an earlier snapshot's start position, a `ui` channel's idle 1500)
            bool driven = _switchSet[ch];
            int cur = _rc.Override[ch] != 0 ? _rc.Override[ch] : _rc.ChannelsUs[ch];
            _rc.Override[ch] = _rc.ChannelsUs[ch] = (ushort)sw.Positions[sw.StartPosition(cur, driven, armId)].Us;
        }
        _rc.MarkDirty();
        RebuildSwitchRows();
        UpdateModesHeader();
    }

    /// The pressed position: the switch's current position stands out (the stick dot's orange). Its content margins are the
    /// buttons' own (the flight windows' theme: Godot's default button box): a button's size comes from its normal box and
    /// its text is placed inside the drawn one, so other margins here clipped the text — a lone "—" disappeared, "none"
    /// showed as "non" (the owner's "invisible — on a pressed switch", 2026-10-02).
    private static StyleBoxFlat? _positionPressed;
    private static StyleBoxFlat PositionPressed
    {
        get
        {
            if (_positionPressed != null) return _positionPressed;
            var def = ThemeDB.GetDefaultTheme().GetStylebox("normal", "Button");
            return _positionPressed = new StyleBoxFlat
            {
                BgColor = new Color(0.95f, 0.6f, 0.15f), CornerRadiusTopLeft = 3, CornerRadiusTopRight = 3, CornerRadiusBottomLeft = 3, CornerRadiusBottomRight = 3,
                ContentMarginLeft = def.ContentMarginLeft, ContentMarginRight = def.ContentMarginRight, ContentMarginTop = def.ContentMarginTop, ContentMarginBottom = def.ContentMarginBottom,
            };
        }
    }
    /// The margins every position button draws its text with (self-tests: equal in both states).
    public static (float L, float R) PressedMargins => (PositionPressed.ContentMarginLeft, PositionPressed.ContentMarginRight);

    /// Can the app drive this channel (0-based)? Its source is `ui` (the app's switches) or `none` (nothing drives it).
    private static bool Drivable(int ch) => RcInput.SourceOf(ch).Kind is "ui" or "none";

    private bool ModesDriven => _switches.Count > 0;

    private void RebuildSwitchRows()
    {
        if (_switchList == null) return;
        foreach (var c in _switchList.GetChildren()) { _switchList.RemoveChild(c); c.QueueFree(); }
        _switchRows.Clear();
        for (int i = 0; i < _switches.Count; i++)
        {
            var sw = _switches[i];
            int ch = sw.Channel - 1;
            bool drive = Drivable(ch);
            var row = new HBoxContainer();
            row.AddThemeConstantOverride("separation", 4);
            var fwRanges = sw.Ranges.Count > 0 ? string.Join(", ", sw.Ranges) : "no firmware mode on this channel";
            row.AddChild(new Label
            {
                Text = $"ch{sw.Channel}{(sw.Label != null ? " " + sw.Label : "")}", CustomMinimumSize = new Vector2(38, 0),
                TooltipText = sw.Static ? $"bench switch ({TxModel.TypeName(sw.Type)}, Edit bench → Transmitter); firmware: {fwRanges}" : $"from the firmware's modes, AUX{sw.Aux + 1}: {fwRanges}",
            });
            var group = new ButtonGroup { AllowUnpress = false };
            var buttons = new Button[sw.Positions.Count];
            for (int p = 0; p < sw.Positions.Count; p++)
            {
                var pos = sw.Positions[p];
                int idx = p, swi = i;
                var b = new Button
                {
                    Text = pos.Label, ToggleMode = true, ButtonGroup = group, FocusMode = Control.FocusModeEnum.None, Disabled = !drive,
                    TooltipText = (sw.Static ? $"sends {pos.Us} µs (position {p + 1} of {sw.Positions.Count}){(pos.BoxIds.Count > 0 ? $"; firmware boxes active there: {pos.Label}" : "; no firmware box active there")}"
                                             : $"{pos.StartUs}–{pos.EndUs} µs → sends {pos.Us}")
                                  + (drive ? "" : $"\nchannel {sw.Channel} is on the transmitter ({RcInput.SourceOf(ch)}): shown, not driven"),
                };
                b.AddThemeStyleboxOverride("pressed", PositionPressed);
                b.AddThemeStyleboxOverride("hover_pressed", PositionPressed);
                b.AddThemeColorOverride("font_pressed_color", Colors.Black);
                b.AddThemeColorOverride("font_hover_pressed_color", Colors.Black);
                b.Pressed += () => SetSwitchPosition(swi, idx, "click");
                buttons[p] = b;
                row.AddChild(b);
            }
            var key = i < Keybinds.SwitchKeys ? Keybinds.Bind(Keybinds.SwitchId(i + 1)) : "";
            var keyLabel = new Label { Text = key != "" ? $"[{key}]" : "", Modulate = Ui.Soft, TooltipText = drive ? $"{Keybinds.SwitchId(i + 1)}: next position (Settings → Keys)" : "on the transmitter" };
            row.AddChild(keyLabel);
            _switchList.AddChild(row);
            _switchRows.Add((sw, buttons, keyLabel));
        }
        UpdateDerivedSwitches();
    }

    /// Per frame: the pressed button follows the channel as sent (the radio's value on a channel it drives).
    private void UpdateDerivedSwitches()
    {
        foreach (var (sw, pos, _) in _switchRows)
        {
            int p = sw.PositionOf(_rc.ChannelsUs[sw.Channel - 1]);
            for (int i = 0; i < pos.Length; i++) if (pos[i].ButtonPressed != (i == p)) pos[i].SetPressedNoSignal(i == p);
        }
    }

    /// switch_N: the N-th Transmitter row (channel order) to its next position (wrapping).
    private void SwitchKey(int n)
    {
        if (n > _switches.Count)
        {
            Log("app", $"{Keybinds.SwitchId(n)}: no row {n} — the Transmitter has {_switches.Count} switch(es){(_switches.Count > 0 ? " (" + string.Join(", ", _switches.Select(s => $"ch{s.Channel}")) + ")" : "")}{(_modes == null ? "; the firmware's modes are not read yet" : "")} (bench switches: Edit bench → Transmitter)");
            return;
        }
        var sw = _switches[n - 1];
        int cur = sw.PositionOf(_rc.ChannelsUs[sw.Channel - 1]);
        SetSwitchPosition(n - 1, (cur + 1) % sw.Positions.Count, "key");
    }

    /// Puts switch `i` at position `p` (the channel's Override); a channel on the radio is left alone and logged.
    public bool SetSwitchPosition(int i, int p, string how)
    {
        if (i < 0 || i >= _switches.Count) return false;
        var sw = _switches[i];
        int ch = sw.Channel - 1;
        if (!Drivable(ch)) { Log("app", $"switch ch{sw.Channel}: on the transmitter ({RcInput.SourceOf(ch)}), not driven from the app"); UpdateDerivedSwitches(); return false; }
        var pos = sw.Positions[Math.Clamp(p, 0, sw.Positions.Count - 1)];
        _rc.Override[ch] = (ushort)pos.Us;
        _rc.OverrideBy[ch] = how == "key" ? RcOwner.Kb : RcOwner.Ui;   // the bar's mark: its key or a click
        _rc.ChannelsUs[ch] = (ushort)pos.Us;                // shown at once; Update re-applies it and marks it for sending
        _switchSet[ch] = true;
        _rc.MarkDirty();
        Log("app", $"switch ch{sw.Channel} ({how}): {pos.Label} ({pos.Us} µs)");
        UpdateDerivedSwitches();
        return true;
    }

    private void UpdateModesHeader()
    {
        if (_modesHeader == null) return;
        string text;
        var stat = TxModel.StaticText(TxModel.Effective(_bench.Transmitter, _bench.Board));
        if (_modes == null) text = "modes: not read yet (the core reads them once the firmware answers)";
        else
        {
            var src = _modesLive ? $"from firmware (t = {_modes.TimeText()} s)" : $"from bench snapshot ({_modes.Received:yyyy-MM-dd HH:mm})";
            text = $"modes: {src}" + (_modes.Switches().Count == 0 ? " — no aux ranges: set the modes in the configurator" : "");
        }
        if (stat != "") text += " · " + stat;
        _modesHeader.Text = text;
        if (_updateModesBtn != null) _updateModesBtn.Disabled = !_session.Connected;
    }

    /// Every 10th state: the diff refresh state follows armed.
    private void OnStateModes(JsonElement s)
    {
        _diffView?.SetRefreshState(_session.Connected, _fcArmed);
        _configPanel?.SetState(_session.Connected, _fcArmed);
    }

    // ---- diff ----
    /// Config window: the core's cached diff ({"type":"diff","refresh":false}: nothing happens to the firmware), or
    /// a fresh `diff all` (`fromFirmware`: CLI + `exit` = the firmware reboots; refused here while armed).
    private void RequestDiff(bool fromFirmware)
    {
        if (!fromFirmware)
        {
            if (_session.Send(new { type = "diff", refresh = false })) Log("app", "firmware config: asked the core for its last diff (cached: the firmware is not touched)");
            return;
        }
        if (_fcArmed != false) { Log("app", $"firmware config: {(_fcArmed == true ? "armed" : "the firmware's state is not known yet")} — Refresh from firmware reboots it, so only while disarmed"); return; }
        if (_session.Send(new { type = "diff" })) Log("app", "firmware config: refresh from the firmware — the core enters the CLI, runs diff all and leaves with exit: the firmware reboots (disarmed; re-arm yourself)");
    }

    private void OnDiff(JsonElement m)
    {
        if (ConfigChanges.IsChanges(m)) { OnChanges(m); return; }       // {"type":"diff","against":"script"}'s answer (Flight.Config.cs)
        ChangesNotUnderstood(m);
        var d = FirmwareDiff.FromMessage(m, DateTime.Now);
        var err = Json.Str(m, "error");
        if (err != null) _diffView?.ShowRefusal(err);
        if (d == null)
        {
            // no dump (yet): the core says why (`error`: armed, calibrating, cli, busy, not_ready, none…); a kept one stays
            Log("core", err is { } e ? $"diff: {(e == "none" ? "the core has no diff yet" : "not refreshed: " + e)}" : "diff: a message without text ignored");
            return;
        }
        _bench.FirmwareDiff = d;
        _diffLive = true;
        if (DevEnv.SaveBench) _bench.Save();
        _diffView?.Show(d, fromBench: false);
        Log("core", $"diff: {d.Text.Split('\n').Length} lines{(d.Source != null ? $" ({FirmwareDiff.SourceText(d.Source)})" : "")}{(d.Stale ? $" (stale{(err != null ? ": " + err : "")})" : "")}{(d.Replay ? " (replay)" : "")} — Windows → Config");
    }

    // ---- Settings over the bench ----
    private void OpenSettings()
    {
        if (_settingsWin != null) { _settingsWin.GrabFocus(); return; }
        var vp = GetViewport().GetVisibleRect().Size;
        var page = _settingsPage = new Settings { Overlay = true, BenchSwitches = _switches };   // Keys names switch_N after the bench's rows
        var size = new Vector2I((int)Math.Clamp(vp.X - 80, 480, 1160), (int)Math.Clamp(vp.Y - 110, 360, 900));
        var pos = new Vector2I(Math.Max(0, ((int)vp.X - size.X) / 2), Math.Max(34, ((int)vp.Y - size.Y) / 2));   // below the top bar; never off-screen (headless: a tiny viewport)
        var win = _settingsWin = new Window { Title = "Settings", Transient = true, Exclusive = true, Unresizable = false, Theme = Ui.BaseTheme, Visible = false };
        win.AddChild(page);
        page.Closed += CloseSettings;
        win.CloseRequested += CloseSettings;
        _ui.AddChild(win);
        win.Popup(new Rect2I(pos, size));
        Log("app", "settings opened over the bench (the core keeps running; Back returns here)");
    }

    private void CloseSettings()
    {
        var w = _settingsWin;
        if (w == null) return;
        _settingsWin = null;
        w.Hide();
        w.QueueFree();                                          // Settings._ExitTree saves the settings
        RefreshAfterSettings();
        Log("app", "settings closed: key binds and transmitter sources apply now");
    }

    /// Key binds and channel sources may have changed: labels and driven channels follow.
    private void RefreshAfterSettings()
    {
        DeriveSwitches();                                       // which channels the app drives, key labels
        UpdateKeysHint();
    }

    /// Is the Settings overlay open (keys go to it, not to the bench)?
    public bool SettingsOpen => _settingsWin != null;
}
