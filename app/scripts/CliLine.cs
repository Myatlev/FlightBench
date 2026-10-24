// The Log / CLI window's input line: Enter sends (Sent decides whether it went out; then it is stored in the
// history and the field cleared), Up / Down browse the history (CliHistory: prefix recall), Escape while browsing
// restores the typed text (a second Escape leaves editing as usual), Ctrl+L asks the window to clear the log.
// The keys are taken in _GuiInput, which runs before LineEdit's own handling: Up / Down do not move the caret
// (or the focus); Left / Right / Home / End are LineEdit's as always.
using System;
using Godot;

namespace FlightBench;

public partial class CliLine : LineEdit
{
    public const string Hint = "Up/Down: history · Ctrl+L: clear";
    public CliHistory History = CliHistory.Shared;
    /// A command to send; true = it went out (stored in the history, the field cleared); false = the text stays.
    public Func<string, bool>? Sent;
    public Action? ClearLog;

    public CliLine()
    {
        KeepEditingOnTextSubmit = true;                                 // Enter sends and stays for the next command
        TextSubmitted += Submit;
        TextChanged += _ => History.EndBrowse();                        // typing over a recalled command: that is the new typed text
    }

    private void Submit(string text)
    {
        if (CliHistory.Clean(text) == "") { Text = ""; return; }      // empty / whitespace only: nothing sent
        if (Sent?.Invoke(text) != true) return;
        History.Add(text);
        Text = "";
    }

    private void Show(string? s)
    {
        if (s == null) return;
        Text = s;                                                       // set_text: no TextChanged
        CaretColumn = s.Length;
    }

    public override void _GuiInput(InputEvent e)
    {
        if (e is not InputEventKey { Pressed: true } k) return;
        bool mods = k.CtrlPressed || k.AltPressed || k.ShiftPressed || k.MetaPressed;
        switch (k.Keycode)
        {
            case Key.Up when !mods: Show(History.Up(Text)); AcceptEvent(); break;
            case Key.Down when !mods: Show(History.Down()); AcceptEvent(); break;
            case Key.Escape when !mods && History.Browsing: Show(History.Cancel()); AcceptEvent(); break;
            case Key.L when k.IsCommandOrControlPressed() && !k.AltPressed && !k.ShiftPressed: ClearLog?.Invoke(); AcceptEvent(); break;
        }
    }
}
