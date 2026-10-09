# WeaponSkins: native C++ port

## Status

Implementation and regression checks are in progress. This report distinguishes automated
verification from the server and human checks that remain. No production server or database
has been changed.

## Included

- Pisex `sc_*` storage through asynchronous plgdb, configured server ID, player auto-creation,
  lazy loading, and the existing additive `sc_skins_ext` schema.
- GiveNamedItem post-hook through polyhook, with native attribute writes from an in-memory
  cache: paint, seed, wear, quality, name tag, StatTrak, six stickers and a keychain.
- Knife, gloves, agent, music kit and scoreboard pin application; guarded deferred actions.
- StatTrak delta persistence, sticker refresh, `!wp` with a 20-second cooldown and nine
  website redirects. Production config and all five translation files are packaged.
- Catalog and language parsing from game VPKs, repeated KV sections, prefab inheritance,
  modern item definitions, and optional permission gates.
- Production `MusicKitMvpAnthem=false`: no MVP event replay; the separate round-end sound
  plugin keeps its existing behavior.

## Excluded

FreeSql/inherit/external backends, migration infrastructure, the in-game menu stack and the
public WeaponSkin API are excluded because the supplied production configuration uses the
Pisex website and hook backend. `SyncFromDatabaseWhenPlayerJoin` full-table preloading is
replaced by per-player reconciliation/loading; production already disables this option.
SQLite support in the storage adapter exists for isolated tests, not as an additional
production backend. No .NET dependency is included in the native plugin manifest.

## Deliberate corrections and differences

- Protected econ fields use raw writes at runtime schema offsets and network notifications.
  Typed s2sdk setters reject these fields with `FollowCS2ServerGuidelines=true`; the native
  attribute setter is resolved from the plugin's own gamedata.
- Current CS2 has **inline `int32[2]` reserve ammo**, despite `m_pReserveAmmo` spelling. Both
  elements are preserved during refresh. The older handoff's pointer interpretation is wrong.
- Current `CUtlVectorEmbeddedNetworkVar` header has count/data/capacity at relative offsets
  **0/8/16** inside resolved `m_Attributes`. Attribute clearing changes only the 4-byte count.
  The real binary evidence and schema widths are in `tests/fixtures/game_excerpt_source.md`.
- Engine teams 2/3 and Pisex database teams 0/1 are decoded separately. Definitions come from
  current game data: MP5-SD is 23, MP7 is 33; obsolete index 20 is absent.
- Bot guards, map guards, player/pawn/team identity checks and explicit teardown correct bugs
  in the older reference. One-shot timers leave the tracking list before they are freed.
- StatTrak writes update only the counter. They do not rewrite website paint/seed/name/sticker
  choices. Failed writes retain their pending delta; in-flight bookkeeping prevents ordinary
  duplicate scheduling and retains disconnected caches until acknowledgement.
- Website StatTrak flags and the first four sticker IDs win over stale extension rows.
  Extension geometry applies only to the same sticker ID. Agent-only/pin-only snapshots count
  as configured items, and music is applied when a lazy load arrives after spawn.
- Missing translation keys fall back to the configured message; key names are not printed in
  chat. Russian command messages come from the production TOML; no Russian translation file
  was present in the supplied resources.

## Verification completed so far

- Config: 337 checks passed against production TOML, translations, parsing edge cases and
  credential redaction.
- Storage: 354 checks passed against disposable MariaDB 11, including concurrent first loads,
  server fallback, extension retry, site changes, delta-only updates and asynchronous lifetime.
- Econ: 243 checks passed; 23 additional checks passed against unchanged extracted records and
  the full installed 8,390,439-byte `items_game.txt` (CS2 1.41.8.9). Full catalog parsing took
  approximately 0.12 seconds in the sniper build container.
- All pre-existing workspace test binaries passed in the baseline `./test.sh` run.
- Test fixture rows for SteamID64 `76561198042508552` were created in **mirrors only**. The
  fixture helper saves prior tester rows privately before replacement and fixes its database
  target to `mirrors`. No previous skin tables/rows existed in that database.

Full native build/ABI, the final combined test run, server survival tests and human visual
checks will be recorded here after they actually complete.

## Remaining limits

- Visual replication, knife subclass/model changes, glove rebuilding, all sticker offsets,
  keychain rendering, pickup timing and ammo preservation require the human in-game checklist.
- A lost SQL connection after an UPDATE has committed but before its acknowledgement has an
  ambiguous result. The shared plgdb driver and retrying counter deltas cannot guarantee exactly
  once across that failure without a database operation journal. No extra schema/table is
  introduced by this port. Normal failed queries and delayed acknowledgements are tested;
  a simulated network acknowledgement loss is not covered.
- Shutdown drains work for a bounded interval. An unavailable database or abrupt process crash
  can leave unsaved deltas; saves happen during play and disconnect, rather than relying on
  plugin shutdown. The underlying s2sdk quit-crash is outside this plugin.

## Build, package and production connection

Use `./build.sh plugify-WeaponSkins weaponskins_tests`, then `./build.sh` and `./test.sh` from
the repository root. `plugins/WeaponSkins/package.sh` stages `build/package/WeaponSkins`, with
the native `.so`, Plugify manifest, gamedata, config and translations.

Packaged `database.jsonc` contains blank credentials. Test deployment must write connection
`skins` in `<plugify configs>/WeaponSkins/database.jsonc` with database `mirrors`; never reuse
a production database name on port 27035. The prepared shared tooling patch is in the handoff
analysis directory and requires approval under the supplied handoff instructions.

For a future authorized production installation, supply the website's existing MySQL
connection privately in that JSON file (`connections.skins`), retain its `sc_` prefix and
correct `ServerId`, and install the native package with the required s2sdk/polyhook versions.
The plugin creates only the additive extension table; base tables belong to the website.
Changing production credentials, deployment or removing the installed .NET module is a
separate action and has not been performed.
