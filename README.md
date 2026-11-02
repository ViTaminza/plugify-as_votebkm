# as_votebkm (Plugify)

Player votes to ban / kick / mute / gag someone, ported from the Pisex admin_system module 'VoteBKM' ([AS] VoteBKM by Pisex) to a standalone native s2sdk plugin. Keeps the votebkm.ini keys, the chat commands, the menus, the immunity/disable flag rules, the cooldown and the punishment path (admin core AddPlayerPunishment / SendAction + an engine kick) of the original. A delayed result always revalidates the captured SteamID64 and never punishes a new occupant of a reused slot. Requires the admin_system core; the hudmenu dependency is optional and only preserves the player's own !menu rendering choice.

Plugify port (fork) of **VoteBKM ([AS] VoteBKM)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), admin_system (>=1.1.0), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `as_votebkm-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_votebkm`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

