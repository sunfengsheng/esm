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

## Content search icon

The standalone content-search application uses a separate original icon so it is visually distinct from filename search:

- indigo/purple rounded-square background;
- white document page with an amber match line;
- cyan search lens;
- simplified 16, 32, 48 and 256 pixel frames.

Regenerate its source PNGs, preview and ICO with:

```powershell
python tools\generate_content_icon.py
```

`content_search.ico` is embedded by `src/apps/content_gui/resources.rc` and is also used by `packaging/content_search.nsi` for the installer and uninstaller.
