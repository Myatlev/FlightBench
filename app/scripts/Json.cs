// JSON helpers shared by the app: serializer options for our own files (bench, settings) and for the
// core's documents, safe getters on dotted paths ("fc.armed", "timing.realtime_factor") that never
// throw on a missing field or a wrong type, and write-then-rename saving (a crash mid-write leaves the
// old file whole).
using System;
using System.IO;
using System.Text.Json;
using Godot;

namespace FlightBench;

public static class Json
{
    /// Our files: camelCase, indented (bench, settings).
    public static readonly JsonSerializerOptions Files = new() { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    /// The core's documents (manifest): property names in any case.
    public static readonly JsonSerializerOptions Lenient = new() { PropertyNameCaseInsensitive = true };

    /// Walks a dotted path through objects ("fc.link.lq"); false if any step is missing or not an object.
    public static bool TryNode(JsonElement el, string path, out JsonElement v)
    {
        v = el;
        var rest = path.AsSpan();
        while (rest.Length > 0)
        {
            if (v.ValueKind != JsonValueKind.Object) return false;
            int dot = rest.IndexOf('.');
            var name = dot < 0 ? rest : rest[..dot];
            if (!v.TryGetProperty(name, out v)) return false;
            rest = dot < 0 ? ReadOnlySpan<char>.Empty : rest[(dot + 1)..];
        }
        return true;
    }

    public static bool TryNum(JsonElement el, string path, out double v)
    {
        v = 0;
        if (!TryNode(el, path, out var n) || n.ValueKind != JsonValueKind.Number) return false;
        return n.TryGetDouble(out v);
    }

    public static double Num(JsonElement el, string path, double def = double.NaN) => TryNum(el, path, out var v) ? v : def;

    public static bool TryBool(JsonElement el, string path, out bool v)
    {
        v = false;
        if (!TryNode(el, path, out var n)) return false;
        if (n.ValueKind == JsonValueKind.True) { v = true; return true; }
        return n.ValueKind == JsonValueKind.False;
    }

    public static string? Str(JsonElement el, string path) => TryNode(el, path, out var n) && n.ValueKind == JsonValueKind.String ? n.GetString() : null;

    /// A numeric array of exactly 3 at that path.
    public static bool TryArr3(JsonElement el, string path, out double a, out double b, out double c)
    {
        a = b = c = 0;
        if (!TryNode(el, path, out var n) || n.ValueKind != JsonValueKind.Array || n.GetArrayLength() != 3) return false;
        return Num(n[0], out a) && Num(n[1], out b) && Num(n[2], out c);
    }

    /// A numeric array of exactly 4 at that path (quaternions).
    public static bool TryArr4(JsonElement el, string path, out double a, out double b, out double c, out double d)
    {
        a = b = c = d = 0;
        if (!TryNode(el, path, out var n) || n.ValueKind != JsonValueKind.Array || n.GetArrayLength() != 4) return false;
        return Num(n[0], out a) && Num(n[1], out b) && Num(n[2], out c) && Num(n[3], out d);
    }

    private static bool Num(JsonElement e, out double v) { v = 0; return e.ValueKind == JsonValueKind.Number && e.TryGetDouble(out v); }

    private static readonly System.Text.UTF8Encoding Utf8NoBom = new(false);

    /// Writes `text` to `path` (user:// or absolute) through a temp file and a rename. False + error on failure.
    public static bool WriteAtomic(string path, string text, out string error)
    {
        error = "";
        try
        {
            var abs = ProjectSettings.GlobalizePath(path);
            var dir = Path.GetDirectoryName(abs);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            var tmp = abs + ".tmp";
            File.WriteAllText(tmp, text, Utf8NoBom);                   // UTF-8 without BOM (the core reads session files)
            File.Move(tmp, abs, overwrite: true);
            return true;
        }
        catch (Exception e) { error = e.Message; return false; }
    }

    /// The file's text, or null when it does not exist / cannot be read.
    public static string? ReadText(string path) => ReadText(path, out var text, out _) == Read.Ok ? text : null;

    public enum Read { Ok, Missing, Unreadable }

    /// Reads a file, telling a missing one from one that exists but cannot be read (`error` then says why).
    public static Read ReadText(string path, out string? text, out string error)
    {
        text = null; error = "";
        string abs;
        try { abs = ProjectSettings.GlobalizePath(path); }
        catch (Exception e) { error = e.Message; return Read.Unreadable; }
        if (!File.Exists(abs))
        {
            if (!Directory.Exists(abs)) return Read.Missing;
            error = "a folder"; return Read.Unreadable;
        }
        try { text = File.ReadAllText(abs); return Read.Ok; }
        catch (FileNotFoundException) { return Read.Missing; }                  // removed in between
        catch (DirectoryNotFoundException) { return Read.Missing; }
        catch (Exception e) { error = e.Message; return Read.Unreadable; }
    }

    /// Copies a file that failed to parse to "<path>.bad-<time>" (never over an earlier copy). The copy's path, or null.
    public static string? BackUpBad(string path)
    {
        try
        {
            var abs = ProjectSettings.GlobalizePath(path);
            if (!File.Exists(abs)) return null;
            var stem = $"{abs}.bad-{DateTime.Now:yyyyMMdd-HHmmss}";
            var dst = stem;
            for (int n = 2; File.Exists(dst); n++) dst = $"{stem}-{n}";
            File.Copy(abs, dst);
            return dst;
        }
        catch (Exception) { return null; }
    }
}
