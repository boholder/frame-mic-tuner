# Third-party licenses

Frame Mic Tuner's own code is MIT licensed (see [LICENSE](LICENSE)). That license does not cover the third-party parts listed below.

## Bundled in this repository

### OpenVR header (`third_party/openvr/openvr.h`)

- From the [OpenVR SDK](https://github.com/ValveSoftware/openvr) 2.15.6 (tag `v2.15.6`, `headers/openvr.h`), unchanged
- License: BSD-3-Clause, Copyright (c) 2015, Valve Corporation
- Full text: [third_party/openvr/LICENSE](third_party/openvr/LICENSE) (the SDK's `LICENSE` at that tag)

The BSD-3-Clause license does not allow using Valve's name to endorse or promote this project. Frame Mic Tuner is not affiliated with or endorsed by Valve Corporation.

## Not bundled: libraries used from the headset

The build links dynamically against these libraries, which are already installed on SteamOS (or come with SteamVR). None of their code is included in this repository or in the release tar.gz: the prebuilt binary in a release (`frame-mic-tuner-<version>.tar.gz`) links to the copies already on the headset and ships with this file, [LICENSE](LICENSE) and [third_party/openvr/LICENSE](third_party/openvr/LICENSE). The licenses below were checked against the packages installed on a Steam Frame (SteamOS, `pacman -Qi`, 2026-09-27).

| Library | Provided by | License |
|---|---|---|
| `libopenvr_api.so` | SteamVR (`/opt/steamvr/bin/linuxarm64/`), installed on the headset as part of SteamVR | Part of SteamVR, used under Valve's terms. The same library in the OpenVR SDK is BSD-3-Clause |
| cairo | SteamOS package `cairo` 1.18.0 | LGPL-2.1-only OR MPL-1.1 |
| FreeType | SteamOS package `freetype2` 2.13.2 | FreeType License (FTL) OR GPL-2.0-or-later (the package metadata says "GPL"; FreeType is dual-licensed, see its `LICENSE.TXT`) |
| libpipewire-0.3 | SteamOS package `libpipewire` 1.6.8 | MIT |
| Vulkan loader (`libvulkan.so.1`) | SteamOS package `vulkan-icd-loader` 1.4.309 | Apache-2.0 |

cairo is only linked dynamically, which keeps its LGPL option satisfied.

## Not bundled: font

The panel is drawn with the headset's own Noto Sans CJK (`/usr/share/fonts/noto-cjk/`, SteamOS package `noto-fonts-cjk`), licensed under the SIL Open Font License 1.1. No font files are included in this repository.

## Programs called at run time

The app runs `wpctl`, `pw-link`, `pw-dump`, `pw-cli`, `pw-metadata` (part of PipeWire / WirePlumber) and `systemctl` (systemd) as separate programs with fixed arguments, and the bundled update script (`vendor/frame-updater/`, by the same author and covered by this repository's MIT license) runs `curl`, `tar`, `sha256sum` and `systemd-run`. None of them are linked or bundled.
