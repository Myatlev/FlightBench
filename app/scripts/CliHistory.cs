// The Log / CLI window's command history, shell-like and global (not per bench): user://cli_history.txt, one
// command per line, UTF-8 without BOM, appended (and closed, so flushed) on every sent command; loaded on first
// use. Consecutive duplicates are not stored; at most Cap entries (the oldest dropped; a file longer than Cap is
// compacted on load). FB_CLI_HISTORY=<path> moves the file; a self-test run without it keeps history in memory.
// Browsing (Up / Down, CliLine.cs): the text in the field when Up is first pressed is the prefix — only commands
// starting with it are recalled (fish / zsh style), newest first, each distinct command once; Down past the
// newest (or Cancel) gives the typed text back.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Godot;

namespace FlightBench;

public sealed class CliHistory
{
    public const int Cap = 1000;
    public const string UserPath = "user://cli_history.txt";
    private static readonly UTF8Encoding Utf8 = new(false);

    private readonly List<string> _entries = new();
    // the file's last line has no line break (hand-edited, or a write cut short): the next append starts with one
    private bool _openLine;
    /// Oldest first.
    public IReadOnlyList<string> Entries => _entries;
    /// The file (absolute), or null: in memory only.
    public string? Path { get; }

    // browsing: matches newest first, _pos = index into them (-1 = the typed text)
    private List<string>? _matches;
    private int _pos = -1;
    private string _typed = "";
    public bool Browsing => _matches != null;

    private static CliHistory? _shared;
    /// The app's history (created and loaded on first use).
    public static CliHistory Shared => _shared ??= new CliHistory(DefaultPath());

    /// FB_CLI_HISTORY, else user://cli_history.txt; null (in memory) in a self-test run without the knob.
    public static string? DefaultPath() =>
        DevEnv.CliHistory ?? (DevEnv.SelfTestRun ? null : ProjectSettings.GlobalizePath(UserPath));

    public CliHistory(string? path)
    {
        Path = path;
        Load();
    }

    /// Reads the file (a missing / unreadable one = empty); longer than Cap: the newest Cap kept and the file rewritten.
    private void Load()
    {
        if (Path == null || !System.IO.File.Exists(Path)) return;
        string[] lines;
        try { lines = System.IO.File.ReadAllLines(Path, Utf8); }
        catch (Exception ex) { GD.PrintErr($"cli history: cannot read {Path}: {ex.Message}"); return; }
        foreach (var l in lines)
        {
            var s = l.Trim();
            if (s != "" && (_entries.Count == 0 || _entries[^1] != s)) _entries.Add(s);
        }
        if (_entries.Count > Cap) _entries.RemoveRange(0, _entries.Count - Cap);
        if (lines.Length > Cap) Compact();
        else
        {
            try
            {
                using var fs = System.IO.File.OpenRead(Path);
                if (fs.Length > 0) { fs.Seek(-1, System.IO.SeekOrigin.End); _openLine = fs.ReadByte() != '\n'; }
            }
            catch (Exception) { }
        }
    }

    private void Compact()
    {
        try
        {
            var tmp = Path + ".tmp";
            using (var fs = new System.IO.FileStream(tmp, System.IO.FileMode.Create, System.IO.FileAccess.Write))
            {
                var bytes = Utf8.GetBytes(string.Concat(_entries.Select(e => e + "\n")));
                fs.Write(bytes, 0, bytes.Length);
                fs.Flush(true);                                             // on disk before it replaces the old file
            }
            System.IO.File.Move(tmp, Path!, true);                          // the old file stays whole until this replace
        }
        catch (Exception ex) { GD.PrintErr($"cli history: cannot compact {Path}: {ex.Message}"); }
    }

    /// One line as stored: trimmed, line breaks as spaces.
    public static string Clean(string text) => text.Replace("\r\n", " ").Replace('\n', ' ').Replace('\r', ' ').Trim();

    /// A sent command: stored unless empty or equal to the newest; appended to the file at once. Ends browsing.
    public void Add(string text)
    {
        EndBrowse();
        var s = Clean(text);
        if (s == "" || (_entries.Count > 0 && _entries[^1] == s)) return;
        _entries.Add(s);
        if (_entries.Count > Cap) _entries.RemoveAt(0);
        if (Path == null) return;
        try
        {
            if (System.IO.Path.GetDirectoryName(Path) is { Length: > 0 } dir) System.IO.Directory.CreateDirectory(dir);
            System.IO.File.AppendAllText(Path, (_openLine ? "\n" : "") + s + "\n", Utf8);   // opened and closed: on disk even if the app crashes next
            _openLine = false;
        }
        catch (Exception ex) { GD.PrintErr($"cli history: cannot append to {Path}: {ex.Message}"); }
    }

    /// Up: the next older match, or null (none; the field stays as it is). The first Up takes `current` as the prefix.
    public string? Up(string current)
    {
        if (_matches == null)
        {
            _typed = current;
            var seen = new HashSet<string>();
            _matches = new List<string>();
            for (int i = _entries.Count - 1; i >= 0; i--)
            {
                var e = _entries[i];
                if (e.StartsWith(current, StringComparison.Ordinal) && e != current && seen.Add(e)) _matches.Add(e);
            }
            _pos = -1;
        }
        if (_pos + 1 >= _matches.Count) { if (_matches.Count == 0) EndBrowse(); return null; }
        return _matches[++_pos];
    }

    /// Down: the next newer match; past the newest the typed text (and browsing ends). Null when not browsing.
    public string? Down()
    {
        if (_matches == null) return null;
        if (--_pos >= 0) return _matches[_pos];
        return Cancel();
    }

    /// Leaves browsing: the typed text back, or null when not browsing.
    public string? Cancel()
    {
        if (_matches == null) return null;
        var t = _typed;
        EndBrowse();
        return t;
    }

    /// Forget the browse (the user edited the recalled text: it is the new typed text).
    public void EndBrowse() { _matches = null; _pos = -1; }
}
