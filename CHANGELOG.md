# Changelog

## 0.1.0 (2026-09-27)

First release.

- A "Mic" panel in the SteamVR dashboard that switches the Steam Frame's microphone filters, with two tabs ("Quick" and "Fine-tune"; it reopens on the last one): presets for how you listen ("Earphones / headphones" = echo cancellation off and noise suppression off, "Frame speakers" = echo cancellation on and noise suppression off; each card shows what it sets, and both cards turn grey when the settings match neither), plus separate "Fine-tune" switches for echo cancellation and noise suppression. Changes apply at once, even while the mic is in use, and are kept after a restart.
- Noise filter strength sliders (Strictness = VAD threshold, Hold = VAD grace period) that apply live without restarting PipeWire. The values are saved and applied again whenever the app starts.
- Shows whether the mic is in use and which filters the sound actually goes through, read from the live PipeWire links.
- Voice check: record up to 10 seconds of what apps hear, keep the last 5 recordings in memory and play them back to compare settings. Nothing is written to disk.
- Japanese and English UI. It starts in the Steam Frame's language (from Steam's language setting) and remembers your choice. Colors meet WCAG 2.x AA contrast (`--contrast-report` prints every pair).
- Self-repair: if SteamVR loses the app's dashboard panel (for example when the dashboard restarts), the app notices within 3 seconds and recreates it.
- Starts from the dashboard's "+" (launch a program) list. Launching it again while it runs opens its panel. Optional autostart with SteamVR, switched from the panel.
- Includes the WirePlumber script (`contrib/wireplumber/`) that makes the filters switchable without restarting PipeWire.
- `install.sh` builds and installs everything into the home directory, with no sudo and no PipeWire / WirePlumber restart.
