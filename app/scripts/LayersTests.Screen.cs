// The screen parts of the config-layers self-tests (LayersTests.cs): the modes screen (an RS7 bench in memory, the plain fake
// core — it arms at t > 8 s), after the modes / windows tests and before the bench tests (BenchTests.cs).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class Flight
{
    private int _ltStep;
    private ulong _ltDeadline;
    private bool _ltDone;
    private string? _ltKeepDir, _ltDir, _ltExport;
    private readonly List<string> _ltSent = new();

    private void LayersSelfTest()
    {
        void Check(string t, bool ok, string what) { if (!ok) LayersTests.Fails[t].Add(what); }
        bool Waited(string t, bool cond, string what)
        {
            if (cond) return true;
            if (Time.GetTicksMsec() < _ltDeadline) return false;
            LayersTests.Fails[t].Add("timed out: " + what);
            return true;
        }
        void Next(int ms = 8000) { _ltStep++; _ltDeadline = Time.GetTicksMsec() + (ulong)ms; }
        switch (_ltStep)
        {
            case 0:
                _core.Sending += line => { if (line.Contains("\"type\":\"config\"") || line.Contains("\"against\"")) _ltSent.Add(line); };
                // the bench's files go to a temp benches folder (never user://benches)
                _ltKeepDir = BenchData.Dir;
                BenchData.Dir = _ltDir = $"user://selftest-layers-screen-{Time.GetTicksUsec()}";
                Check("config_changes", _windows.TryGet("Config", out _) && _configPanel != null, "no Config window");
                RequestChanges();
                Next();
                break;
            case 1:
            {
                if (!Waited("config_changes", _configPanel!.Changes != null, "the fake core's diff against the script")) break;
                var c = _configPanel.Changes;
                // the plain fake core: RS7_SCRIPT vs RS7_DIFF (tail_center_trim 60 → 137, …) — or refused once it armed (t > 8 s)
                Check("config_changes", c != null && (c.Error == "armed" && _configPanel.ChangesHead == "Changes: not compared (armed)"
                                                      || c.Error == null && c.ChangedSettings.Any(x => x.Setting == "set tail_center_trim" && x.Script == "set tail_center_trim = 60" && x.Firmware == "set tail_center_trim = 137") && c.OnlyScript.Contains("set rescue_mode = ON")),
                      $"the fake core's answer: {(c?.Error != null ? "error " + c.Error : $"{c?.ChangedSettings.Count} changed, {c?.OnlyFirmware.Count} only firmware, {c?.OnlyScript.Count} only script")}; head '{_configPanel.ChangesHead}'");
                Check("config_changes", _ltSent.Contains("{\"type\":\"diff\",\"against\":\"script\"}"), "sent: " + string.Join(" | ", _ltSent));
                LayersTests.Info["config_changes"].Add($"screen: the fake core answered the request ({(c?.Error != null ? "refused: " + c.Error : "compared")})");
                _core.InjectForTest(LayersTests.ChangesMessage);
                Check("config_changes", _configPanel.Changes?.ChangedSettings.Count == 2 && _configPanel.ChangeRows().Count == 5 && LogHas("config changes against the script: 2 changed, 2 only in the firmware, 1 only in the script"),
                      "an injected answer on the screen: " + _configPanel.ChangesHead);
                // snapshot: the bench's bench_cli path (disarmed as far as the app knows); the fake core answers (written, or refused when armed)
                _core.InjectForTest("{\"type\":\"state\",\"fc\":{\"armed\":false,\"msp\":true}}");
                bool sent = SnapshotToBench();
                var path = BenchFiles.CliPath(_bench);
                Check("snapshot_export", sent && _bench.BenchCli == BenchFiles.CliRel(_bench.Name) && _ltSent.Any(l => l.Contains("\"snapshot\":\"bench\"") && l.Contains(JsonSerializer.Serialize(path).Trim('"'))),
                      $"snapshot sent with the bench's bench_cli {path}: " + string.Join(" | ", _ltSent));
                Next();
                break;
            }
            case 2:
            {
                if (!Waited("snapshot_export", _configPanel!.StatusText.Contains("snapshot written") || _configPanel.StatusText.Contains("snapshot refused"), "the fake core's snapshot answer")) break;
                LayersTests.Info["snapshot_export"].Add($"screen: the fake core's answer '{_configPanel.StatusText}'");
                // a written snapshot reloads layer 5 — in the Config window and in the Edit-bench wizard over the screen
                EditBench();
                Next();
                break;
            }
            case 3:
            {
                if (!Waited("snapshot_export", _wizard is { } w0 && w0.IsNodeReady(), "the wizard over the screen")) break;
                var path = BenchFiles.CliPath(_bench);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, "# the firmware's diff all as bench lines\nset from_snapshot = 1\n");
                int before = _snapshotsWritten;
                _core.InjectForTest("{\"type\":\"config\",\"snapshot\":\"written\",\"path\":" + JsonSerializer.Serialize(path) + ",\"lines\":1}");
                Check("snapshot_export", _snapshotsWritten == before + 1 && _configPanel!.Layers.BenchLinesText.Contains("set from_snapshot = 1") && _wizard!.Layers.BenchLinesText.Contains("set from_snapshot = 1")
                                         && _configPanel.StatusText.StartsWith("snapshot written"),
                      $"after the written answer: Config '{_configPanel?.Layers.BenchLinesText}', wizard '{_wizard?.Layers.BenchLinesText}', status '{_configPanel?.StatusText}'");
                CloseEditWizard();
                // export as preset: the core's export (the fake core answers), then the folder
                _ltExport = ProjectSettings.GlobalizePath($"user://selftest-export-screen-{Time.GetTicksUsec()}");
                LastExport = null;
                ExportPreset(_ltExport);
                Next();
                break;
            }
            case 4:
            {
                if (!Waited("snapshot_export", LastExport != null, "the export")) break;
                var names = LastExport!.Select(Path.GetFileName).ToList();
                var id = BenchData.SafeName(_bench.Name);
                var readmePath = Path.Combine(_ltExport!, id + ".README.md");
                var readme = File.Exists(readmePath) ? File.ReadAllText(readmePath) : "";
                var cliPath = Path.Combine(_ltExport!, id + ".cli");
                Check("snapshot_export", names.Contains(id + ".json") && names.Contains(id + ".cli") && names.Contains(id + ".README.md") && File.Exists(cliPath) && File.ReadAllText(cliPath).Contains("set from_snapshot = 1"),
                      "export folder: " + string.Join(", ", names));
                Check("snapshot_export", _ltSent.Contains("{\"type\":\"config\",\"export\":true}") && _coreExport != null && readme.Contains("### 2 · aircraft") && readme.Contains("models/rs7/qemu-profile.txt"),
                      $"the README from the core's export: {(_coreExport == null ? "no export answer" : _coreExport.Lines.Count + " lines")}; README {readme.Length} chars");
                LayersTests.Info["snapshot_export"].Add($"screen: Snapshot sends the bench's bench_cli path; a written answer reloads layer 5 in the Config window and the Edit-bench wizard; Export as preset → {string.Join(", ", names)} (README from the core's config export)");
                // boot hint: the fake core's reboot after exit CLI (MSP silent ~2 s)
                _cli!.Sent!("status");
                Next();
                break;
            }
            case 5:
                if (!Waited("boot_hint", _fc.Cli == true, "the fake core's CLI open")) break;
                ExitCli();
                Next();
                break;
            case 6:
            {
                if (!Waited("boot_hint", BootHintText != "", "the boot hint after the reboot")) break;
                Check("boot_hint", BootHintText == "firmware booting — the configurator can connect once it answers" && BootHintTooltip.Contains("wait for the green dot") && BootHintTooltip.Contains("Save and Reboot"),
                      $"first reboot (nothing measured yet): '{BootHintText}', tooltip '{BootHintTooltip}'");
                var names2 = _topBar?.GetChildren().Select(c => c.Name.ToString()).ToList() ?? new();
                Check("boot_hint", names2.IndexOf("boot_hint") == names2.IndexOf("cfg_address") + 1, "top bar: " + string.Join(",", names2));
                Next();
                break;
            }
            case 7:
                if (!Waited("boot_hint", BootHintText == "" && !double.IsNaN(_lastBootS), "the firmware answering again")) break;
                Check("boot_hint", _lastBootS is > 0.5 and < 10, $"measured boot {_lastBootS:0.00} s");
                _cli!.Sent!("status");
                Next();
                break;
            case 8:
                if (!Waited("boot_hint", _fc.Cli == true, "the CLI again")) break;
                ExitCli();
                Next();
                break;
            case 9:
                if (!Waited("boot_hint", BootHintText != "", "the second boot hint")) break;
                Check("boot_hint", System.Text.RegularExpressions.Regex.IsMatch(BootHintText, @"^firmware booting — the configurator can connect in ~\d+ s$"), $"second reboot: '{BootHintText}' (last boot {_lastBootS:0.00} s)");
                LayersTests.Info["boot_hint"].Add($"screen: exit CLI → reboot → 'once it answers' the first time, measured {_lastBootS:0.0} s, then '{BootHintText}'; the green-dot tooltip; next to the configurator address");
                Next();
                break;
            case 10:
                if (!Waited("boot_hint", BootHintText == "" && _fc.Cli == false, "the second boot over")) break;
                // leave the screen as the bench tests expect it: their CLI counters from zero, the benches folder back
                _cliOpened = _cliClosed = 0;
                if (_ltKeepDir != null) BenchData.Dir = _ltKeepDir;
                if (_ltDir != null) try { Directory.Delete(ProjectSettings.GlobalizePath(_ltDir), true); } catch (Exception) { }
                if (_ltExport != null) try { Directory.Delete(_ltExport, true); } catch (Exception) { }
                _bench.BenchCli = null;
                foreach (var n in LayersTests.ScreenNames.Where(DevEnv.SelfTest)) LayersTests.Report(n);
                _ltDone = true;
                if (!BenchTests.ScreenAny) ModesTests.Done = true;
                break;
        }
    }
}
