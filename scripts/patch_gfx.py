"""
Pre-build patch for Arduino_GFX / Arduino_ST7796.

Bug: writeAddrWindow() uses "x += _yStart" instead of "y += _yStart",
so _yStart (= COL_OFFSET1 = 49 for the T-Display S3 Pro) is never added
to the RASET command in rotation=3. Symptoms: header invisible, bottom
49 rows show the factory boot image.

Registered in platformio.ini as:  extra_scripts = pre:scripts/patch_gfx.py
Runs after library installation, before compilation.
"""

Import("env")  # noqa: F821  (injected by SCons/PlatformIO)
import os

lib_path = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),  # noqa: F821
    env.subst("$PIOENV"),               # noqa: F821
    "GFX Library for Arduino",
    "src", "display", "Arduino_ST7796.cpp",
)

BUGGY = "    x += _yStart;"
FIXED = "    y += _yStart;  // patched: was 'x += _yStart' — RASET offset fix"

if not os.path.exists(lib_path):
    print("[patch_gfx] Library not yet downloaded — will patch on next build")
else:
    with open(lib_path, "r") as f:
        content = f.read()

    if FIXED in content:
        print("[patch_gfx] Arduino_ST7796.cpp already patched, OK")
    elif BUGGY in content:
        content = content.replace(BUGGY, FIXED, 1)
        with open(lib_path, "w") as f:
            f.write(content)
        print("[patch_gfx] Applied RASET y-offset fix to Arduino_ST7796.cpp")
    else:
        print("[patch_gfx] WARNING: expected line not found — library version may have changed")
