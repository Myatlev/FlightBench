// The address "Copy configurator address" gives for the core's MSP port, in the form the firmware's own
// configurator takes. The RotorFlight (and Betaflight) configurator takes only `tcp://host[:port]` in its port
// field (src/js/serial.js). The iNav Configurator (port list → TCP, field "IP:Port") splits the text on ':'
// (js/connection/connectionTcp.js): `tcp://127.0.0.1:31235` becomes host "tcp", port parseInt("//127.0.0.1") =
// NaN, Node's net.connect throws ERR_SOCKET_BAD_PORT and it reports "Failed to open MSP connection" before a
// byte is sent; it needs `host:port`. Which firmware, best source first: the `variant` of the core's `modes`
// ("INAV" / "RTFL" / "BTFL": what the firmware itself answered over MSP), else the firmware name the core reports —
// `hello.firmware` ("inav 9.1.0 …" once MSP answered; before that the core puts the file name there) or, when the
// firmware answers after this client connected, the core's log line "firmware says: inav 9.1.0 …" —, before any of
// these the bench's firmware file name (inav_9.1.0_SPEEDYBEEF405WING.hex). iNav = that name (the file name only,
// not its folders) contains "inav" and neither "rotorflight" nor "betaflight" (SPEEDYBEEF405WING_INAV_9.1.hex
// counts; a folder named inav does not). Anything else — RotorFlight, Betaflight, an iNav file named without "inav"
// before the firmware answered, nothing known — gets tcp://host:port, the form the RotorFlight / Betaflight
// configurators take.
using System;

namespace FlightBench;

public static class ConfiguratorAddress
{
    /// The firmware name a `modes` variant stands for ("INAV" → "inav", "RTFL" → "rotorflight", "BTFL" → "betaflight"); null: unknown.
    public static string? FromVariant(string? variant) => variant?.Trim().ToUpperInvariant() switch
    {
        "INAV" => "inav",
        "RTFL" => "rotorflight",
        "BTFL" => "betaflight",
        _ => null,
    };

    /// The firmware name that picks the address form: the modes' variant, else what the core reported (hello /
    /// "firmware says"), else the bench's firmware file.
    public static string? Pick(string? variant, string? coreFirmware, string? benchFile) =>
        FromVariant(variant) ?? (string.IsNullOrWhiteSpace(coreFirmware) ? benchFile : coreFirmware);

    /// The firmware name in the core's log line "firmware says: inav 9.1.0 SPEEDYBEEF405WING" (null: another line).
    public static string? FromLog(string text)
    {
        const string P = "firmware says: ";
        int i = text.IndexOf(P, StringComparison.Ordinal);
        return i < 0 ? null : text[(i + P.Length)..].Trim() is { Length: > 0 } fw ? fw : null;
    }

    public static bool IsInav(string? firmware)
    {
        if (firmware == null) return false;
        var name = firmware.Trim().Replace('\\', '/');
        name = name[(name.LastIndexOf('/') + 1)..];
        bool Has(string w) => name.Contains(w, StringComparison.OrdinalIgnoreCase);
        return Has("inav") && !Has("rotorflight") && !Has("betaflight");
    }

    public static string For(string? firmware, string host, int port) =>
        IsInav(firmware) ? $"{host}:{port}" : $"tcp://{host}:{port}";

    /// Which configurator the address is for (the hint and the log line say it).
    public static string Name(string? firmware) => IsInav(firmware) ? "iNav Configurator" : "RotorFlight configurator";
}
