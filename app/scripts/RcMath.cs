// Channel arithmetic shared by RcInput, Settings (calibration capture, bars) and ChannelBar.
// Normalised value n −1..1 ↔ µs = 1500 + 500·n. Calibration: the µs a source really sends at its
// low / centre / high end are mapped onto 1000 / 1500 / 2000 in two linear segments; a calibration
// that is not strictly min < centre < max is treated as identity (1000 / 1500 / 2000).
using System;

namespace FlightBench;

public static class RcMath
{
    public const int MinUs = 1000, CenterUs = 1500, MaxUs = 2000, ClampLo = 800, ClampHi = 2200;

    public static float ToUsF(float n) => CenterUs + 500f * n;
    public static int ToUs(float n) => (int)MathF.Round(ToUsF(n), MidpointRounding.AwayFromZero);
    public static float ToNorm(float us) => Math.Clamp((us - CenterUs) / 500f, -1f, 1f);
    /// 0..1 position of a value on a 1000..2000 µs bar (clamped).
    public static float BarFraction(int us) => Math.Clamp((us - MinUs) / 1000f, 0f, 1f);

    public static bool ValidCalibration(int min, int center, int max) => min < center && center < max;

    /// Raw source value (−1..1) → channel µs: calibration (two segments), deadzone around the centre,
    /// reverse, clamped to 800..2200.
    public static ushort Map(float raw, int min, int center, int max, bool reverse, float deadzone)
    {
        if (!ValidCalibration(min, center, max)) { min = MinUs; center = CenterUs; max = MaxUs; }
        float us = ToUsF(raw);
        float n = us >= center ? (us - center) / (max - center) : (us - center) / (center - min);
        if (deadzone > 0 && MathF.Abs(n) < deadzone) n = 0;
        if (reverse) n = -n;
        return (ushort)Math.Clamp(ToUs(n), ClampLo, ClampHi);
    }

    /// Map with a channel's calibration from the transmitter settings.
    public static ushort Map(TransmitterSettings t, int ch, float raw, bool deadzone) =>
        Map(raw, t.MinUs[ch], t.CenterUs![ch], t.MaxUs[ch], t.Reverse[ch], deadzone ? t.Deadzone : 0f);
}
