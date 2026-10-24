// TCP client for the FlightBench core (docs/protocol.md): newline-delimited JSON both ways.
// Each connection runs on its own background task with its own socket, token and bounded outbox;
// its lines reach the main thread through one queue, tagged with the connection's id, and are
// delivered as C# events from Poll() (lines of an older connection are dropped there). The connection
// state is owned by the main thread and changes only from Poll() (`_conn` lines) and Connect/Disconnect.
// Send() queues only while connected: a command given while disconnected (or still connecting) is
// dropped, never delivered to the next core.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

public sealed class CoreClient : IDisposable
{
    public event Action<JsonElement>? StateReceived;
    public event Action<JsonElement>? HelloReceived;
    public event Action<string, string>? LogReceived;      // src, text
    public event Action<JsonElement>? SignalsReceived;     // the core's signal list
    public event Action<JsonElement>? ManifestReceived;    // what the core can emulate (boards, devices, faults, aircraft)
    public event Action<JsonElement>? SamplesReceived;     // subscribed signals at state_hz
    public event Action<JsonElement>? OsdReceived;         // OSD canvas cells (MSP DisplayPort)
    public event Action<JsonElement>? ModesReceived;       // the firmware's mode boxes + ranges (read over MSP)
    public event Action<JsonElement>? DiffReceived;        // the firmware's `diff all` text (and `diff against script`)
    public event Action<JsonElement>? ConfigReceived;      // `config` answers: snapshot written, export, refusals
    public event Action<JsonElement>? FunctionReceived;    // an environment function's progress
    public event Action<string, JsonElement, double>? EventReceived;   // `event`: name, data (Undefined when absent), t (NaN when absent)
    public event Action<string>? ConnectionChanged;        // "connecting to h:p" / "connected" / "disconnected: reason"
    /// Every line accepted by Send, before it is queued (dev self-tests print it).
    public event Action<string>? Sending;
    // JsonElements handed to the events live only during the call (the document is disposed after it): Clone() to keep one.

    public enum ConnState { Disconnected, Connecting, Connected }
    public ConnState State { get; private set; } = ConnState.Disconnected;
    public bool Connected => State == ConnState.Connected;
    public string Host => _conn?.Host ?? "";
    public int Port => _conn?.Port ?? 0;
    /// Lines dropped by Send (not connected, or the outbox full) since this client was made.
    public int Dropped { get; private set; }

    /// Lines waiting for the socket; above this Send drops (a core that stopped reading).
    public const int OutboxLimit = 1024;

    private sealed class Conn
    {
        public readonly int Id;
        public readonly string Host;
        public readonly int Port;
        public readonly CancellationTokenSource Cts = new();
        public readonly Channel<string> Outbox = Channel.CreateBounded<string>(new BoundedChannelOptions(OutboxLimit) { SingleReader = true, FullMode = BoundedChannelFullMode.Wait });
        public volatile bool Closed;                        // Disconnect() asked: no "disconnected" line from the task
        public volatile bool Closing;                       // outbox completed (Disconnect, or the task is ending)
        public Conn(int id, string host, int port) { Id = id; Host = host; Port = port; }

        /// Stops the connection's task and completes its outbox (the Cts may already be disposed by the task).
        public void Stop()
        {
            Closing = true;
            try { Cts.Cancel(); } catch (ObjectDisposedException) { }
            Outbox.Writer.TryComplete();
        }
    }

    private Conn? _conn;
    private int _nextId;
    private readonly ConcurrentQueue<(int Id, string Line)> _inbox = new();
    private bool _dropLogged, _fullLogged, _closingLogged;
    private readonly HashSet<string> _failedTypes = new();

    public void Connect(string host, int port)
    {
        Disconnect();
        var c = new Conn(++_nextId, host, port);
        _conn = c;
        _dropLogged = _fullLogged = _closingLogged = false;
        SetState(ConnState.Connecting, $"connecting to {host}:{port}");
        _ = Task.Run(() => RunAsync(c));
    }

    public void Disconnect()
    {
        var c = _conn;
        _conn = null;
        if (c == null) return;
        c.Closed = true;
        c.Stop();                                           // the task disposes the socket
        SetState(ConnState.Disconnected, "disconnected");
    }

    private void SetState(ConnState s, string what)
    {
        State = s;
        ConnectionChanged?.Invoke(what);
    }

    private async Task RunAsync(Conn c)
    {
        var ct = c.Cts.Token;
        string? reason = null;
        TcpClient? client = null;
        Task? reader = null, writer = null;
        try
        {
            client = new TcpClient { NoDelay = true };
            await client.ConnectAsync(c.Host, c.Port, ct);
            var stream = client.GetStream();
            _inbox.Enqueue((c.Id, "{\"type\":\"_conn\",\"what\":\"connected\"}"));
            reader = ReadAsync(stream, c, ct);
            writer = WriteAsync(stream, c, ct);
            var first = await Task.WhenAny(reader, writer);
            await first;                                    // rethrows the failure of whichever ended first
            reason = first == reader ? "closed by core" : "writer stopped";
        }
        catch (Exception e) { reason = e is OperationCanceledException ? "cancelled" : e.Message; }
        finally
        {
            c.Stop();                                       // stops the other half
            // let both halves leave the stream before it is disposed (bounded: a read that ignores the
            // token ends when the socket closes anyway)
            if (reader != null && writer != null)
                try { await Task.WhenAny(Task.WhenAll(reader, writer), Task.Delay(2000)); } catch (Exception) { }
            try { client?.Dispose(); } catch (Exception) { }
            c.Cts.Dispose();
        }
        if (!c.Closed) _inbox.Enqueue((c.Id, "{\"type\":\"_conn\",\"what\":\"disconnected: " + JsonEncodedText.Encode(reason ?? "?") + "\"}"));
    }

    private async Task ReadAsync(NetworkStream stream, Conn c, CancellationToken ct)
    {
        using var reader = new StreamReader(stream, Encoding.UTF8, false, 65536, leaveOpen: true);
        while (!ct.IsCancellationRequested)
        {
            var line = await reader.ReadLineAsync(ct);
            if (line == null) return;                       // EOF
            if (line.Length > 0) _inbox.Enqueue((c.Id, line));
        }
    }

    private static async Task WriteAsync(NetworkStream stream, Conn c, CancellationToken ct)
    {
        await foreach (var line in c.Outbox.Reader.ReadAllAsync(ct))
        {
            var bytes = Encoding.UTF8.GetBytes(line + "\n");
            await stream.WriteAsync(bytes, ct);
        }
        await Task.Delay(Timeout.Infinite, ct);             // completed outbox (Disconnect): wait for the cancel
    }

    /// Queues a message for the current connection. Not connected, the connection closing, or the outbox
    /// full: dropped, logged once per connection, false.
    public bool Send(object message)
    {
        var c = _conn;
        if (c == null || State != ConnState.Connected)
        {
            Dropped++;
            if (!_dropLogged) { _dropLogged = true; LogReceived?.Invoke("app", "not connected: commands are dropped until connected"); }
            return false;
        }
        var line = JsonSerializer.Serialize(message);
        if (!c.Outbox.Writer.TryWrite(line))
        {
            Dropped++;
            if (c.Closing)
            {
                if (!_closingLogged) { _closingLogged = true; LogReceived?.Invoke("app", "connection closing, command dropped"); }
            }
            else if (!_fullLogged) { _fullLogged = true; LogReceived?.Invoke("app", $"core is not reading: outbox full ({OutboxLimit}), commands dropped"); }
            return false;
        }
        Sending?.Invoke(line);
        return true;
    }

    /// Self-tests: handle one JSON line on the main thread as if the core had sent it (events, states), whatever the connection.
    public void InjectForTest(string line)
    {
        using var doc = JsonDocument.Parse(line);
        var root = doc.RootElement;
        Dispatch(Json.Str(root, "type") ?? "", root);
    }

    /// Deliver queued messages on the caller's (main) thread.
    public void Poll()
    {
        while (_inbox.TryDequeue(out var item))
        {
            if (_conn == null || item.Id != _conn.Id) continue;         // a line of an older connection
            JsonDocument doc;
            try { doc = JsonDocument.Parse(item.Line); }
            catch (JsonException) { LogReceived?.Invoke("app", "bad json from core: " + item.Line); continue; }
            using (doc)
            {
                var type = "?";
                try
                {
                    var root = doc.RootElement;
                    type = root.ValueKind == JsonValueKind.Object ? Json.Str(root, "type") ?? "" : "";
                    Dispatch(type, root);
                }
                catch (Exception e)
                {
                    if (_failedTypes.Add(type)) { GD.PushError($"core message '{type}': {e}"); LogReceived?.Invoke("app", $"error handling a '{type}' message (logged once): {e.Message}"); }
                }
            }
        }
    }

    private void Dispatch(string type, JsonElement root)
    {
        switch (type)
        {
            case "state": StateReceived?.Invoke(root); break;
            case "hello": HelloReceived?.Invoke(root); break;
            case "osd": OsdReceived?.Invoke(root); break;
            case "signals": SignalsReceived?.Invoke(root); break;
            case "manifest": ManifestReceived?.Invoke(root); break;
            case "samples": SamplesReceived?.Invoke(root); break;
            case "modes": ModesReceived?.Invoke(root); break;
            case "diff": DiffReceived?.Invoke(root); break;
            case "config": ConfigReceived?.Invoke(root); break;
            case "function": FunctionReceived?.Invoke(root); break;
            case "log": LogReceived?.Invoke(Json.Str(root, "src") ?? "core", Json.Str(root, "text") ?? ""); break;
            case "_conn":
            {
                var what = Json.Str(root, "what") ?? "";
                if (what == "connected") { _dropLogged = _fullLogged = _closingLogged = false; SetState(ConnState.Connected, what); }
                else { _conn = null; SetState(ConnState.Disconnected, what); }
                break;
            }
            case "event":
            {
                var name = Json.Str(root, "name") ?? "?";
                root.TryGetProperty("data", out var d);                  // default (Undefined) when absent
                // a listener (the flight screen) formats and logs them; without one: fault events carry what changed
                // ({device, kind, on}), the others log their name
                if (EventReceived != null) EventReceived(name, d, Json.Num(root, "t", double.NaN));
                else LogReceived?.Invoke("event", name + (name == "fault" && d.ValueKind != JsonValueKind.Undefined ? " " + d.GetRawText() : ""));
                break;
            }
            case "error": LogReceived?.Invoke("error", Json.Str(root, "what") ?? root.GetRawText()); break;
            case "pong": break;
            default: LogReceived?.Invoke("app", "unhandled message type: " + type); break;
        }
    }

    /// Closes the connection without raising events (the owner is going away).
    public void Dispose()
    {
        var c = _conn;
        _conn = null;
        State = ConnState.Disconnected;
        if (c == null) return;
        c.Closed = true;
        c.Stop();
    }
}
