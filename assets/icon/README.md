# everything_sm application icon

This is an original clean-room project asset. It intentionally avoids the orange palette and proprietary icon resources associated with Everything.

Design language:

- navy rounded-square tool tile;
- high-contrast search lens;
- teal index rows and speed lines;
- simplified geometry that remains recognizable at 16 x 16 pixels.

Regenerate all PNG and ICO outputs from the checked-in generator:

```powershell
python tools\generate_icon.py
```

The Windows resource embeds `everything_sm.ico`, containing 16, 20, 24, 32, 40, 48, 64, 128, and 256 pixel frames.
