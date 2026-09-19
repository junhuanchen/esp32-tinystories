# ESP-IDF TinyStories firmware

This is the minimal ESP-IDF build of the existing USB-serial TinyStories
firmware. It does not use Arduino, OLED, touch, audio, Wi-Fi, or the Xiaozhi
project. `idf.py flash` writes both the ESP-IDF application and the verified
`artifacts/tinystories/model.bin` into the `model` partition.

## Prerequisites

- ESP-IDF with its environment activated in the current shell
- Python `uv` on `PATH`
- model artifacts downloaded with `scripts/fetch_model.ps1 -ModelKind tinystories`

## Build and flash

```powershell
.\scripts\build_idf.ps1
.\scripts\build_idf.ps1 -Flash -Port COM5 -Monitor
```

The first command only builds. The second command builds, flashes, and opens
the serial monitor. Replace `COM5` with the board's USB Serial/JTAG port.
Flashing replaces the current partition table and firmware on the board. If
ESP-IDF is not in its default location, pass `-IdfPath C:\path\to\esp-idf`.
At `prompt>`, enter a short printable-ASCII English prompt and press Enter.

### Linux

```bash
scripts/build_idf.sh
scripts/build_idf.sh --flash --port /dev/ttyACM0 --monitor
```

The first command only builds. Set `IDF_PATH` before running it, or pass
`--idf-path /path/to/esp-idf`.
