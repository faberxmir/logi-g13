# G13 screen diagnosis

Status on 2026-11-05: the screen doesn't display frames. Keys, stick, backlight and USB all work.
This is a working theory for after a repair attempt. Nothing has been confirmed by opening the G13.

## Working theory

The first frame after a cold start wakes the display, and the "G13" logo already in the screen's memory becomes visible. Pixel data from later frames never lands in that memory, so frames only cause a flicker.

| Part | Assessment | Confidence |
|---|---|---|
| USB and main chip | Working: frames arrive and the G13 responds | High |
| LCD panel and its drivers | Mostly working: they can show a crisp, solid image | Fairly high |
| Writing pixels to the screen's memory | This is where it fails | Fairly high |
| Why it fails | Damaged line or connection, failing controller, or firmware | **Unknown** |
| Capacitor as the main cause | Unlikely: it wouldn't explain ignored pixels | Medium |

If it's physical, a knock, age or corrosion from a spill could all explain it. The cold exposure (around 10°C) is an unlikely cause.

## What we observed

- **Cold start:** blank after being unplugged for a while. Sometimes random, sparse pixels of mixed brightness instead.
- **Quick reconnect:** the letters can reappear with nothing sent.
- **First frame after blank:** the logo appears. Half-dark and `0x55` frames did the same, so content doesn't seem to matter.
- **The logo:** it's stored in the G13, not sent by us. It's flipped vertically, and sometimes shifted sideways so the 3 wraps around the edge.
- **Later frames:** flicker only, with stripes and bright horizontal lines during the flicker.
- **Logitech Gaming Software 9.04.49:** detects the G13 and can change the backlight color, but doesn't fix the screen.

What photos of these states showed (not kept):

- **Clean logo, nothing writing to it:** crisp, solid and centered, but flipped vertically, with a few dark dashes in one row near the bottom.
- **After a few frames:** the same logo shifted sideways with the 3 wrapping around, the dashes gone, and tiny unread text at the bottom-left.
- **During writes:** letters striped with every other row lit, plus a bright horizontal line.
- **Random pixels:** sparse light dots of mixed brightness across the whole screen.

## Ruled out or not helpful

- **Frame format:** it matches open-source drivers and the G13's reported 992-byte output report. One Linux project confirmed this format on firmware 2.03. This unit is **1.03**, so a firmware difference hasn't been ruled out.
- **G HUB:** frames failed with it closed too. It isn't needed for the logo to appear.
- **Feature report 4** (`02 00 00 00`, then `02 80 00 FF`): the G13 accepted it, but it didn't fix anything. A replug cleared it.
- **Common LCD command bytes as frame data** (`AE AF A6 A7 C0 C8 A0 A1 40 60`): no repeatable effect. The one time the letters vanished, it was the first frame after a restart, and it didn't repeat.
- **Firmware update:** Logitech Gaming Software doesn't bundle a G13 updater.

Not ruled out: gentle pressure on the frame changed nothing, but that doesn't exclude a bad connection. The screen was also never tested on another computer or with a Linux driver.

## When opening it

Unplug the G13 first. Before moving anything, take photos of the board and the screen's connector.

Look for:
- A loose, creased or torn ribbon cable, or a lifted joint where it meets the board or screen
- Corrosion or residue around the connector, which could point to a spill
- Cracks in the screen's glass, particularly along the edge where the cable attaches
- Swollen or leaking capacitors, or cracked solder joints near the connector

## Checks after a repair

Run these in order with G HUB and Logitech Gaming Software closed, and quit the mapper from the tray. All frames use [tools/lcd_test.py](../tools/lcd_test.py), whose arguments are the delay before the first frame, the gap between frames (both in seconds), then the fill bytes in hex.

1. **Confirm the rest still works.** Check the keys, stick and backlight before anything else.
2. **Cold start.** Unplug it for at least a minute, then plug it in. Note whether it's blank, shows letters or shows random pixels.
3. **Single full frame.** This is the key test. A working screen fills up completely instead of showing the logo:
   ```
   python -I tools\lcd_test.py 5 0 FF
   ```
4. **Empty frame.** A working screen clears completely, and the logo doesn't come back:
   ```
   python -I tools\lcd_test.py 5 0 00
   ```
5. **Striped frame.** A working screen shows even horizontal stripes, which also checks the rows:
   ```
   python -I tools\lcd_test.py 5 0 55
   ```
6. **The toad.** Set `show=1` under `[screen]` in `g13.ini` and run `install.bat`. Replug the G13, and the toad and `superpadde` should appear.

Which fill looks light and which looks dark isn't confirmed on this unit, so `FF` and `00` may look swapped. That's fine, as long as each fills or clears the whole screen.

### Reading the results

| Result | Likely meaning | Next step |
|---|---|---|
| Steps 3 to 6 work | Fixed | Keep `show=1` |
| Image is flipped or shifted, but follows the frames | Pixels now land, and the settings are wrong | Software may be able to correct it |
| Parts of the image are missing or garbled, with a pattern | Some signal lines are still bad | Recheck the connection |
| Same as before: logo and flicker only | Repair didn't reach the fault | Suspect the controller or firmware |
| Keys or stick stopped working | Repair disturbed something else | Recheck connectors first |
