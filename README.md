# WeaponSkins (Plugify)

Native C++ port: Pisex website skins, knives, gloves, agents, music kits, pins, stickers, keychains, StatTrak and wp sync.

Plugify port (fork) of **WeaponSkins (SwiftlyS2)** by samyyc & ELDment (SwiftlyS2 (C#)).
Original: https://github.com/samyycX/WeaponSkins
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, polyhook, permissions (optional), hudmenu (optional)

## Installation
Download `WeaponSkins-1.0.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/WeaponSkins`.

## License
AGPL-3.0 (as the original), see LICENSE.

