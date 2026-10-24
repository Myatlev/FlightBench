// The flight screen's link to the core: the address of this run (the bench's, or FB_CORE for this
// run only — never written into the bench), connect / reconnect (a new address closes the old
// connection first), the command policy (nothing is queued while disconnected: Send reports false and
// the caller leaves its own state alone) and what every new connection gets again: the current
// `subscribe` list, a fresh `rc` frame, and — after its `hello` — `subscribe {"modes":true}`: without it the core
// reads the firmware's modes only on request (docs/protocol.md `modes`: scripts leave it off so the board's timing
// is untouched; the app turns it on to get the modes after every boot and their replay).
using System;
using System.Collections.Generic;

namespace FlightBench;

public sealed class CoreSession : IDisposable
{
    public readonly CoreClient Client = new();
    public string Host { get; private set; }
    public int Port { get; private set; }
    public bool Connected => Client.Connected;
    public CoreClient.ConnState State => Client.State;
    /// The ids checked in the Signals window that the core publishes (sent as `subscribe` on every connection).
    public IReadOnlyList<string> Subscribed => _subscribed;
    /// Raised after a connection came up and was re-subscribed (e.g. to force an `rc` frame).
    public event Action? Up;

    private List<string> _subscribed = new();

    /// Ask the core to keep this client's `modes` current (sent after every `hello`).
    public bool SubscribeModes { get; set; } = true;

    public CoreSession(string host, int port)
    {
        Host = host; Port = port;
        Client.HelloReceived += _ => { if (SubscribeModes) Client.Send(new { type = "subscribe", modes = true }); };
        Client.ConnectionChanged += s =>
        {
            if (s != "connected") return;
            if (_subscribed.Count > 0) Client.Send(new { type = "subscribe", signals = _subscribed });
            Up?.Invoke();
        };
    }

    /// "host:port" → (host, port); null when it is not one.
    public static (string Host, int Port)? ParseAddress(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        var i = text.LastIndexOf(':');
        if (i <= 0 || !int.TryParse(text[(i + 1)..], out var port) || port <= 0 || port > 65535) return null;
        return (text[..i].Trim(), port);
    }

    public void Connect() => Client.Connect(Host, Port);

    /// Connects to another address (the old connection is closed first).
    public void Connect(string host, int port) { Host = host; Port = port; Client.Connect(host, port); }

    public void Disconnect() => Client.Disconnect();

    /// Queues a command for the current connection; false (dropped, logged once) when not connected.
    public bool Send(object message) => Client.Send(message);

    /// Replaces the subscription; sent now when connected, otherwise on the next connection.
    public void Subscribe(List<string> ids)
    {
        _subscribed = ids;
        if (Client.Connected) Client.Send(new { type = "subscribe", signals = ids });
    }

    public void Poll() => Client.Poll();

    public void Dispose() => Client.Dispose();
}
