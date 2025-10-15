LCD Console (tft_console sample)

Overview
--------
This sample provides an LVGL-based TFT console that mirrors the Zephyr shell. It shows the shell output in a scrollable "console" area and renders an input prompt ("s3:~$ ") with a live-updating input line. The console window is intended to be kept visible at all times.

Key behaviors
-------------
- Input capture: keyboard input sent to the UART shell is intercepted and the typed characters are shown in the prompt area in real time.
- Backspace: pressing Backspace deletes characters in the live input and the change is immediately reflected on-screen.
- Enter: pressing Enter leaves the shell to handle command execution; the console area will display the command output, and the temporary input shown at the prompt is cleared.
- Tab completion: Tab completions performed by the shell are displayed in the prompt area when the shell writes completion text.

Implementation notes
--------------------
- To avoid calling UI code from the shell thread, input updates are delivered to the main thread via a protected "latest message" buffer.
- To prevent double-echo (characters appearing twice due to local echo and shell write-back), the code remembers recent user-typed bytes and strips echoed prefixes from subsequent writes. This prevents duplicate characters like "hh" when only "h" was typed.
- The control panel (status bar) displays a static title. Dynamic debug text previously displayed in the status bar has been removed to reduce clutter and serial log noise.

Limitations and future enhancements
-----------------------------------
- Complex ANSI/CSI sequences are filtered at a basic level; some cursor-movement heavy applications may require improved CSI handling.
- Message delivery is optimized to avoid queue flooding by storing only the latest input update. If you need to preserve every incremental state, converting to a different delivery scheme will be necessary.

Files of interest
-----------------
- `src/main.c` - LVGL UI, rendering logic and hooking into shell transport.
- `src/lcd_shell_backend.c` - backend that receives intercepted input and stores latest input state.
- `src/lcd_shell_backend.h` - public API for the backend used by `main.c`.

How to test
-----------
1. Build and flash the sample for `esp32s3_devkitc`.
2. Open the serial monitor and interact with the shell.
3. Verify: typing characters updates the prompt; Backspace deletes characters; Enter executes commands and clears the prompt area; Tab completion shows completion text.

Feedback
--------
If you encounter repeated characters, excessive serial logs, or mismatch between shell and LCD state, please collect `serial.log` and a short description of the steps to reproduce and file an issue or contact the maintainer.
