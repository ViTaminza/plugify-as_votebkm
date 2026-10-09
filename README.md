# as_votebkm (Plugify)

Player votes to ban / kick / mute / gag someone, ported from the Pisex admin_system module 'VoteBKM' ([AS] VoteBKM by Pisex). Keeps the votebkm.ini keys, the chat commands, the menus, the immunity/disable flag rules, the cooldown and the punishment path (admin core AddPlayerPunishment / SendAction + an engine kick) of the original. Requires the admin_system core.

Plugify port (fork) of **VoteBKM ([AS] VoteBKM)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_votebkm-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_votebkm`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

