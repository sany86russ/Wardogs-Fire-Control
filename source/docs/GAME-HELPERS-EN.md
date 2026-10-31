# Build helper and Team Piker

Open **Helpers** from the main window or the application's tray menu. Both helpers have their own tab and settings in one window. They do not change artillery calculations.

**WARDOGS rules prohibit input-automation macros and exploiting bugs for advantage; account bans are possible. Publisher approval for these helpers is unconfirmed. Randomization does not provide anti-cheat protection.** [Official policy, checked October 31, 2026](https://www.wardogs.com/enforcement).

## First use

1. Enable the helper's hotkey on its tab. Both helpers are disabled in a fresh profile.
2. Assign a key or combination and check the intervals. Defaults: **F6** for Build helper and **F7** for Team Piker.
3. Click **Save**. Change any occupied or duplicate shortcut; helper shortcuts are never replaced automatically. F12 and combinations with Win are unavailable.
4. Return to WARDOGS, aim the cursor/sight at the desired object and press the assigned shortcut. Release it and its modifiers to start the loop. You do not need to hold the left mouse button yourself.
5. Press the same shortcut again to stop. **Stop helpers** is also available in the application and tray menu.

Only one helper runs at a time. Switching the active window, pausing or closing the application stops the loop and releases the left button. Returning to the game **does not restart** it. Opening settings also stops the helper.

## Build helper

Base sequence: **hold the left button for 660 ms → release for 100 ms → repeat**. Hold and pause durations are configurable. A separate switch adds a random number of milliseconds from the configured range to the released pause; it is disabled by default with a 0–30 ms addition range. Hold duration is not randomized.

[Steam](https://steamcommunity.com/app/1867240/discussions/0/562541966849565845/) and [Reddit](https://www.reddit.com/r/WarDogs/comments/1wf8elq/is_hammer_animation_cancel_an_exploit/) discussions describe releasing the left button after the hammer contacts the object to start the next swing sooner. They support the community observation, but do not establish a universal **660/100 ms** preset. The project owner supplied this preset; our testing has not established its speed benefit in the current game version.

## Team Piker

A series of short left-button clicks without moving the cursor. The default interval is **800 ms + a random 100–200 ms addition**, giving **900–1000 ms between click starts**. This interval already includes the 30 ms button hold. Randomization can be disabled or its range changed.

The first click follows the start command and activation-key release; subsequent clicks follow the configured interval. All timings are nominal: Windows scheduling can delay an event.

## Persistence and limits

Assigned shortcuts, hotkey enable choices, intervals and randomization switches are saved in the Windows profile: `%LOCALAPPDATA%\WardogsFireControl\settings.ini`. Restarting restores these preferences while **both loops remain stopped** until an explicit shortcut press.

Starting requires a foreground window with matching WARDOGS caption/class metadata; this does not authenticate the game process. The loop is tied to one HWND and checks foreground every 10 ms and before a new press. This reduces the chance of clicking in another application but cannot eliminate a short window-switch race. If Windows refuses input, the loop stops with an error; restrictions are not bypassed. See [system interaction and game rules](ANTICHEAT-EN.md).
