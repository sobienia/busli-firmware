# Tramli Pro — Phase 1

A Swiss public transit display for the LILYGO T-Display S3 Pro.
Phase 1 is the minimal working version: WiFi, transit data, layout — no web config yet.

---

## What you'll need

- LILYGO T-Display S3 Pro (the device)
- USB-C cable (must be data-capable, not power-only)
- A computer (Windows, macOS, or Linux)
- ~60 minutes for first-time setup
- Your home WiFi name and password

---

## Step 1 — Install Visual Studio Code (5 minutes)

VS Code is the editor we'll use. PlatformIO runs as an extension inside it.

1. Go to https://code.visualstudio.com
2. Download for your operating system
3. Install with default settings
4. Open VS Code

> If you've never used VS Code before, that's fine. We won't touch most of its features.

---

## Step 2 — Install PlatformIO inside VS Code (5 minutes)

1. In VS Code, click the **Extensions** icon in the left sidebar
   (it looks like four squares with one detached)
2. Search for **PlatformIO IDE**
3. Click **Install** on the one by "PlatformIO"
4. Wait 1–3 minutes for installation. You'll see status messages at the bottom.
5. When prompted, click **Reload** to restart VS Code

After it loads, you'll see a small alien-head icon in the left sidebar — that's PlatformIO.

---

## Step 3 — Install the USB driver (Windows only, 3 minutes)

macOS and Linux can skip this step.

1. Go to https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
2. Download the **CP210x VCP** driver for Windows
3. Run the installer
4. Restart your computer if prompted

> If your device uses a CH9102 chip instead, the driver is at https://www.wch-ic.com/downloads/CH343SER_ZIP.html

---

## Step 4 — Open this project in VS Code (2 minutes)

1. Save this entire `tramli-pro` folder somewhere on your computer
   (Documents folder, Desktop — anywhere is fine)
2. In VS Code: **File → Open Folder…**
3. Select the `tramli-pro` folder (the one containing `platformio.ini`)
4. Click **Open**

VS Code will pop up a "Trust this folder?" prompt — click **Yes, I trust the authors**.

The first time you open the project, **PlatformIO downloads all the needed libraries automatically**. You'll see a progress bar at the bottom right. This takes 2–5 minutes depending on your internet. Be patient — you only do this once.

---

## Step 5 — Add your WiFi credentials (2 minutes)

The project doesn't include your WiFi password (that would be insecure). You need to provide it.

1. In VS Code's file explorer (left sidebar), find the `include` folder
2. Find `secrets.h.example` inside it
3. Right-click on it → **Copy**, then right-click in the `include` folder → **Paste**
4. Right-click the new copy and **Rename** it to `secrets.h` (remove the `.example`)
5. Open `secrets.h`
6. Replace `"Your WiFi Name Here"` with your actual WiFi name (keep the quotes)
7. Replace `"Your WiFi Password Here"` with your actual WiFi password (keep the quotes)
8. Save the file (Ctrl+S / Cmd+S)

Example:
```cpp
#define WIFI_SSID     "MyHomeWiFi"
#define WIFI_PASSWORD "supersecret123"
```

> **Important:** WiFi must be 2.4 GHz. The ESP32 cannot connect to 5 GHz only networks.

---

## Step 6 — Check your board version (1 minute)

LILYGO made small hardware changes between V1.0 and V1.1 of the Pro.

1. Look at the back of your device, near the USB-C port
2. If you see "**V1.1**" printed there → you have V1.1
3. If you don't see it → you have V1.0

The default in the code is `BOARD_VERSION 11` (for V1.1).
If your device is V1.0:
1. Open `include/config.h`
2. Find the line `#define BOARD_VERSION 11`
3. Change it to `#define BOARD_VERSION 10`
4. Save

---

## Step 7 — Plug in the device (1 minute)

1. Connect your T-Display S3 Pro to your computer with the USB-C cable
2. The device may light up showing the LILYGO factory demo — that's fine, we'll overwrite it

---

## Step 8 — Build and upload (5 minutes for first build, 30 seconds after)

1. Look at the bottom of VS Code — you'll see a row of small icons (PlatformIO toolbar)
2. Click the **→** (right arrow) icon — this is "Upload"
3. PlatformIO will compile the code (3–5 minutes the first time, much faster afterwards)
4. Then it will upload to your device

Watch the **Terminal** panel at the bottom for progress. You'll see lots of output — that's normal. Look for these signs of success:

- `Linking .pio/build/...firmware.elf` ← compiled successfully
- `Writing at 0x00010000` ← uploading firmware
- `Hash of data verified` ← upload finished
- `=== [SUCCESS] ===` ← all done

**If the upload fails** with "could not enter download mode" or similar:
1. Hold the **BOOT** button on the device
2. While holding BOOT, press **RST** once
3. Release **RST**
4. Wait 1 second
5. Release **BOOT**
6. Click the upload arrow again

This puts the chip into "manual upload mode."

---

## Step 9 — Watch it boot (30 seconds)

After upload, the device automatically restarts. You should see:

1. **Matrix-style boot animation** in amber for 2-3 seconds
2. **"Connecting..."** message
3. **"Syncing time..."** message
4. **"Loading departures..."** message
5. **The departure board** showing ETH Hönggerberg

If something doesn't work, click the **plug icon** (Serial Monitor) in the PlatformIO toolbar. This shows you what the device is printing as it runs — very helpful for debugging.

---

## Daily use

Now that it's running:

| Action | Result |
|---|---|
| Press BOOT button briefly | Switch to next stop |
| Hold BOOT button 2 seconds | Force immediate refresh |
| Press RST button | Restart the device |
| Unplug from computer | Plug into a phone charger to use as a permanent display |

Don't use a powerbank — most have auto-shutoff that triggers when the device draws too little current.

---

## What works in Phase 1

- ✅ Display rendering (4-row layout, amber theme, big readable font)
- ✅ WiFi connection
- ✅ Live transit data from the SBB API
- ✅ Two stops (ETH Hönggerberg southbound + Schlieren Gasometerbrücke trams 2/20)
- ✅ Switching between stops with the BOOT button
- ✅ Auto-refresh every 30 seconds
- ✅ Boot animation
- ✅ Clock display
- ✅ Delay indicators
- ✅ "Cached" indicator if WiFi drops
- ✅ Night mode brightness (23:00 – 06:00)

## What's coming in later phases

- Phase 2: Web configuration page, persistent settings, multiple WiFi networks
- Phase 3: Weather + UV display, event countdowns, public holidays, touchscreen, themes you can change live, self-update over WiFi

---

## Troubleshooting

### Display shows nothing / black screen

- Check the V1.0/V1.1 setting in `include/config.h`
- Make sure the upload actually succeeded (check the terminal for `[SUCCESS]`)
- Try pressing the RST button on the device

### "WiFi failed" message

- Verify SSID and password in `include/secrets.h` — they're case-sensitive
- Check the network is 2.4 GHz (the ESP32 can't do 5 GHz)
- Open Serial Monitor — you'll see error messages

### "No departures" message

- The API may be down — try again in a minute
- Check Serial Monitor for `[API]` errors
- Make sure the device's clock is set correctly (you'll see it printed at boot)

### Upload fails repeatedly

- Try a different USB cable (most "charging cables" are missing the data wires)
- Use the manual BOOT+RST method described in Step 8
- On Windows, make sure the CP210x driver is installed
- Try a different USB port (avoid USB hubs)

### Hot questions for the Serial Monitor

When in doubt, open the Serial Monitor (the plug icon in PlatformIO's toolbar). The device prints what it's doing at every step. Look for lines starting with `[WiFi]`, `[API]`, `[Display]`, etc. They tell you exactly where things are going wrong.

---

## How to make changes

| What you want to change | File to edit |
|---|---|
| WiFi credentials | `include/secrets.h` |
| Brightness, refresh interval, night hours | `include/config.h` |
| Theme colors | `include/config.h` (top section) |
| Which stops to show | `src/main.cpp` (the `STOPS` array near the top) |
| Display layout details | `src/display.cpp` |

After any change: click the **→** Upload button in the PlatformIO toolbar. Subsequent builds take only ~30 seconds because the libraries are already downloaded.
