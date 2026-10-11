# lr_hits (Plugify)

[LR] ExStats Hits: per-player damage, armor damage and per-hitgroup hit counters in the `<lr_table>_hits` table (SteamID, DmgHealth, DmgArmor, Head, Chest, Belly, LeftArm, RightArm, LeftLeg, RightLeg, Neak), saved at disconnect or on every hit (lr_db_savedata_player_mode) and zeroed on !reset. Ported from the Pisex Levels Ranks module LR_ExStatsHits. Owns its plgdb connection with the levels_ranks database settings. Requires the levels_ranks core; no exported methods.

Plugify port (fork) of **LR_ExStatsHits ([LR] ExStats Hits)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, levels_ranks, hudmenu (optional)

## Installation
Download `lr_hits-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/lr_hits`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

