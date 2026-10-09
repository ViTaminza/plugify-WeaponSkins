# Installed CS2 source fixture

Extracted read-only on 2026-10-08 from the installed game VPKs. No downloads or production snapshot files were used.

This is a selective KV1 excerpt, not a complete item schema. Entire retained definition records are copied verbatim; only wrapper blocks and whitespace around them are constructed. Original block ordering and repeated section names are retained. Pass the whole fixture to `EconData::Parse`. The final `lang` sibling contains matching original English token records: extract the substring beginning at the final `"lang"` block and pass that substring to `EconData::ParseLanguage("en", ...)`, because the language parser expects a single root block.

## Sources

- Directory: `/srv/cs2-go/game/csgo/pak01_dir.vpk`; SHA-256 `8a27b2eb5582eba499da2c718ede61e70d84ee883d3d71e74b53b303a7ad69b7`; VPK v2.
- Items: `scripts/items/items_game.txt` in `/srv/cs2-go/game/csgo/pak01_502.vpk`, archive byte offset `19065040`, payload bytes `8390439`, CRC32 `1fb27047`, SHA-256 `1edea31ef2af0a46d1b38b4e4a6337de534bb61629f4b527137617ef03879fed`.
- English: `resource/csgo_english.txt` in `/srv/cs2-go/game/csgo/pak01_471.vpk`, archive byte offset `75345360`, payload bytes `4877193`, CRC32 `70db13a4`, SHA-256 `34f001eb623b8d05a16d90d859fb862a5c2593e762da8d86e3376d64fa40d3c4`.
- Full items payload retained locally at `/root/plugify-port/handoff/weaponskins/analysis/implementation-20261008/items_game.full.txt`; do not package it into the plugin.
- Excerpt SHA-256: `e9b3fc076d415f40be921cfe89cd3edc72d83e4d8f4ed8a0df7cda3ecbb7bd6d`.

Installed steam.inf (metadata only, not a claim about live-server state):

```text
ClientVersion=2000927
ServerVersion=2000927
PatchVersion=1.41.8.9
ProductName=cs2
appID=730
ServerAppID=2347773
SourceRevision=11087167
VersionDate=Oct 05 2026
VersionTime=15:17:28
```

## Expected records

| Section | Key | Name |
|---|---|---|
| `items` | `7` | `weapon_ak47` |
| `items` | `9` | `weapon_awp` |
| `items` | `16` | `weapon_m4a1` |
| `items` | `23` | `weapon_mp5sd` |
| `items` | `33` | `weapon_mp7` |
| `items` | `34` | `weapon_mp9` |
| `items` | `42` | `weapon_knife` |
| `items` | `59` | `weapon_knife_t` |
| `items` | `60` | `weapon_m4a1_silencer` |
| `items` | `500` | `weapon_bayonet` |
| `items` | `507` | `weapon_knife_karambit` |
| `items` | `5027` | `studded_bloodhound_gloves` |
| `items` | `5030` | `sporty_gloves` |
| `sticker_kits` | `1` | `dh_gologo1` |
| `sticker_kits` | `2` | `dh_gologo1_holo` |
| `paint_kits` | `0` | `default` |
| `paint_kits` | `5` | `hy_ddpat` |
| `paint_kits` | `180` | `cu_fireserpent_ak47_bravo` |
| `paint_kits` | `344` | `cu_medieval_dragon_awp` |
| `paint_kits` | `10018` | `sporty_light_blue` |
| `client_loot_lists` | `set_nuke_2_rare` | `set_nuke_2_rare` |
| `client_loot_lists` | `crate_operation_ii_ancient` | `crate_operation_ii_ancient` |
| `client_loot_lists` | `set_cobblestone_ancient` | `set_cobblestone_ancient` |
| `music_definitions` | `1` | `valve_cs2_01` |
| `music_definitions` | `3` | `danielsadowski_01` |
| `items` | `5105` | `customplayer_tm_leet_variantg` |
| `items` | `5305` | `customplayer_ctm_fbi_variantf` |
| `paint_kits` | `941` | `cu_ak-47_phantom_disruptor` |
| `paint_kits` | `106` | `m4a1s_vaporwave` |
| `keychain_definitions` | `1` | `kc_missinglink_ava` |
| `music_definitions` | `104` | `starjunk95_01` |
| `keychain_definitions` | `84` | `kc_cologne2026` |
| `sticker_kits` | `10369` | `cologne2026_team_vita` |

Important assertions:

- Defindex `23` is `weapon_mp5sd`; `33` is `weapon_mp7`. No item with defindex `20` exists in the complete extracted source.
- The real `weapon_mp5sd_prefab` has `item_class = weapon_mp7` but the item record name is `weapon_mp5sd`. Use the item record name or actual entity classname for the definition-to-classname map, not prefab `item_class`.
- Agent `5105` has model `agents/models/tm_leet/tm_leet_variantg.vmdl` and terrorist ownership; `5305` has model `agents/models/ctm_fbi/ctm_fbi_variantf.vmdl` and counter-terrorist ownership.
- Paint `180` (`cu_fireserpent_ak47_bravo`) and `344` (`cu_medieval_dragon_awp`) have `use_legacy_model = 1`; paint `106` (`m4a1s_vaporwave`) does not. Resolve item names/models/teams through the complete prefab ancestry retained in the excerpt.
- `set_cobblestone_ancient` contains `[cu_medieval_dragon_awp]weapon_awp`; `crate_operation_ii_ancient` contains `[cu_fireserpent_ak47_bravo]weapon_ak47`.
- The complete source has repeated root blocks: `{'items': 136, 'prefabs': 28, 'paint_kits': 39, 'sticker_kits': 55, 'keychain_definitions': 5, 'music_definitions': 32, 'client_loot_lists': 144}`. Iterate/merge all matching sections, not only the first lookup.
- Complete source unique numeric record counts before any econ-category filters: `{'items': 2059, 'paint_kits': 1481, 'sticker_kits': 11789, 'keychain_definitions': 143, 'music_definitions': 102}`.
- The excerpt contains `56` raw definition records and `72` original English token records. English is UTF-8 with BOM in the VPK; the combined excerpt is UTF-8 without BOM.

## Raw record provenance

Spans below are zero-based Unicode character spans in the decoded UTF-8 payload; end is exclusive. Each record hash covers the UTF-8 bytes of that exact unchanged span.

| Section/root block | Key | Character span | SHA-256 |
|---|---|---|---|
| `prefabs` / 7 | `weapon_supports_keychains` | `9411:9530` | `2d3e4a3689dd8cde381170fecae492da0410f7dad0eb12438e144fbecccbbcc9` |
| `prefabs` / 7 | `weapon_supports_stickers` | `9534:9650` | `ac9ba372b97c2c12a33103f4f93f007bdac416b1588efabc104fdb188c7ac763` |
| `prefabs` / 7 | `weapon_case_base` | `11475:12386` | `76114c9a52af93d2921d6197983e6db96e1bd4e9b3e016e1568f6bebf4fa5fde` |
| `prefabs` / 7 | `weapon_base` | `37318:38396` | `9e6419c60f9a7c0b924d935a3bece24af303732ccd9091887e85be5cb9f2c212` |
| `prefabs` / 7 | `melee` | `40856:42596` | `bc19baaac0bd46f05e1e4deca6b429a5eb6b49bada366c65745c459f877f468a` |
| `prefabs` / 7 | `melee_unusual` | `42600:42758` | `69ba45f3af848fb460a40515f6439b07e4b8233d0df1087ed4ce22e80975b3a9` |
| `prefabs` / 7 | `primary` | `43455:43772` | `62c0052f2f0bb9f1d1967417e2dba50f8c57a2c1cd049fe93c5dd800d1ffccdb` |
| `prefabs` / 7 | `smg` | `43776:44187` | `66fdca1fdd5625dbaae1a6b621fefe6916e75630155edf195a7789f734e263c8` |
| `prefabs` / 7 | `rifle` | `44191:44610` | `97c437c99c1721aabb9bc636a41906e2a007d13dbb471290d954e5e03d51fe7e` |
| `prefabs` / 7 | `sniper_rifle` | `44614:45055` | `5c243afbf6bf75e8fc86a38b5a2a0715b83b6aba3f2acc877ecac99bfd87d05f` |
| `prefabs` / 7 | `weapon_mp7_prefab` | `63521:65159` | `35c12f20291fb3b6fb1ccf31a5e7e5ce233c77da585ca921827072f84e179abf` |
| `prefabs` / 7 | `weapon_mp5sd_prefab` | `65163:67106` | `b74f7ff474f05814af89977f5e4188418f5fe8dd1bbca69ed82aac510ed31713` |
| `prefabs` / 7 | `weapon_mp9_prefab` | `67110:68865` | `54355c40ba1241f54c4bea5cd685843ee7415f037e4766d4102856bbe88d90d5` |
| `prefabs` / 7 | `weapon_ak47_prefab` | `72495:74059` | `e86212d7d74048192cf479d6bf512f30418642c196c65489bbd26b3cebdaf4ed` |
| `prefabs` / 7 | `weapon_m4a1_prefab` | `79238:80931` | `f7912320cd14cddf4ffe5be6f9e56fe2c1414d62c711de702eae4d56aafe50c6` |
| `prefabs` / 7 | `weapon_awp_prefab` | `82703:84823` | `3334bbf95ee783d3f290a38005c5321e322d30a466e2cc3c8006bc9def670689` |
| `prefabs` / 7 | `weapon_m4a1_silencer_prefab` | `107665:109676` | `6740047acc3d10b8d5b9701fdf4d23c205a3351a26b6afac0a3ef65bd0da8742` |
| `prefabs` / 7 | `wearable` | `115570:116604` | `94a3e20d06d2caaa80fe1386af77cb354aff4c994c95b81d7036a9f9c8d8d521` |
| `prefabs` / 7 | `hands` | `116608:117000` | `44f23fa5491b11fddad787b2227073fb62be478ea93ba340e52852f08a92b65a` |
| `prefabs` / 7 | `hands_paintable` | `117004:117223` | `b48e27bc24cd2c400ade9669b854f9bf0c24a7a9f603e133f6f5259ff8213230` |
| `prefabs` / 7 | `customplayer` | `117227:118567` | `14731cbe6b92f05b0de0f7b49c735f8ed090acc789a13a60d377d0939669d984` |
| `prefabs` / 7 | `customplayertradable` | `118571:118714` | `906d95460bfe1bb9a9f2ddfacd92458b0d3593e36d65b29ea0a619033b62b68f` |
| `items` / 8 | `7` | `124515:124712` | `50366850dd45876b5c9d8248552304daef149c6e7fd6799cc66a07531a43c14c` |
| `items` / 8 | `9` | `124915:125110` | `03553fc8801371183ea6d820ff68e1b1389d6e4a77eb1ccedcf9d421fa6d0e67` |
| `items` / 8 | `16` | `125930:126162` | `b7c1d11ee6d56dd75cd07f7b88b2198455445d143c035d7bce6486f375c73ddd` |
| `items` / 8 | `23` | `126566:126798` | `2e2f042fcdf215db44b8f2fe196a1b755faad34c253135782630112b2f790c07` |
| `items` / 8 | `33` | `128757:128985` | `99e5f6e375f8f61b4376104851df2ada37a9b0a8a5b7ec844bb3c1e475ae45ab` |
| `items` / 8 | `34` | `128989:129183` | `42e4d63975b1ffa7121266965a2485314bffadc705126c103357a272b00bfb18` |
| `items` / 8 | `42` | `131242:132120` | `f25619a4780cd1f415e8e470b94419fd1016a9670f28103ee4ecb02abd27118e` |
| `items` / 8 | `59` | `137949:138895` | `6259493074eccce2f0c97de54aa3c37983d95bca9e174b8bf916fdb50044e6fa` |
| `items` / 8 | `60` | `138899:139149` | `ba19bf45359baf00b7c566b00a4cd92a4b338fd71f8e589e92b3d2b8344cd5e0` |
| `items` / 8 | `500` | `140515:141457` | `ac3b19621d99a476862be55db1b13efb721f72f7091fd5371329d5f478ceff0f` |
| `items` / 8 | `507` | `144999:146103` | `ba7cc4ecb49dbb96f2f918efd1a13fc567f238b211aea36bf943e4dde6336350` |
| `items` / 8 | `5027` | `527096:527469` | `31f182b4417f8fa4501aea72e44eb7dbd9bc1a5e89708c5314a1bcd76f6377df` |
| `items` / 8 | `5030` | `528361:528704` | `ada61aad5d7c208b2ed2f4ac8c446a4d6a43242bbd6a02a069fe5fe589197e63` |
| `sticker_kits` / 10 | `1` | `686583:686797` | `2dcee2f8ae5bf1ce421cd02695c5285c81fb591bacb8fe7d39f835146ee9f306` |
| `sticker_kits` / 10 | `2` | `686801:687035` | `159927bce3a2bbfc8637e58a1f08f72b48524391690ba257641b6d12b00c1d0b` |
| `paint_kits` / 11 | `0` | `1021543:1021831` | `21bcc4b97f8f93d1f6cb1af1de51172d14956d97fa63a870aeea54c5bd01f9cc` |
| `paint_kits` / 11 | `5` | `1022395:1022571` | `bdcabe8a2ec24c81f34df91abca7327a6ab319ade93311b1c8a75ad941993309` |
| `paint_kits` / 11 | `180` | `1045436:1045698` | `5e68a826a5542dc9f0ed5c01d83ce6af2fb72a0b6e90658cca0508ac8c3d5124` |
| `paint_kits` / 11 | `344` | `1086491:1086777` | `80defb8b02c9391299082d7c836be4abeeb3d521efe55d46d4e458569148dd9f` |
| `paint_kits` / 11 | `10018` | `1142606:1142861` | `7db2e6c8eaddc4606c30b6fdcb420c36c9a466f46f267c191f1fe85f7c8bc876` |
| `client_loot_lists` / 15 | `set_nuke_2_rare` | `1215142:1215343` | `477aff9ac19be7ef185972d398fe7673a4ff7713a8f56821d9f42483288fa8da` |
| `client_loot_lists` / 28 | `crate_operation_ii_ancient` | `1237403:1237533` | `22f1cfe413cae8e701d78f8fb0c507e73ae1c5cd711c173bcac790e5f4a06a3a` |
| `client_loot_lists` / 28 | `set_cobblestone_ancient` | `1261973:1262054` | `9a2041bcdf74540034427b50116ab88c6ea9d1c71c24612b87043489a475af25` |
| `music_definitions` / 33 | `1` | `1350797:1351079` | `b1226bfc3018df3cb735a86582bfdd299b6a533b752005a11c4e521c43b4cdb5` |
| `music_definitions` / 33 | `3` | `1351399:1351706` | `8df8ddf29d6572c64b988cabc4f42a725c5212a4f4f5a36593e1bbf40db4c592` |
| `items` / 261 | `5105` | `4320266:4320934` | `d23fd3e732c7197cf57853d28bde4328ec723af8b026509a044a665b8c5e9b78` |
| `items` / 263 | `5305` | `4331166:4331843` | `0b5ed67d66a8dbd59a64ad28288b882eba326f3d20b6376704902d662c5efe7d` |
| `paint_kits` / 352 | `941` | `4504518:4504816` | `8b846c725f076f83c8554846c6c475ad0824285fdde26eda74bf7c6895a1d204` |
| `paint_kits` / 515 | `106` | `6358220:6358551` | `f6d0046f2d39708fff989173930ab3cc54097cead4498f222fc6a1d467ce0d66` |
| `keychain_definitions` / 517 | `1` | `6548295:6548669` | `84051cd76c2b6c7854d33f532ee77c84cb06f7c83804d133349a010bdb754bf9` |
| `music_definitions` / 557 | `104` | `6595380:6595669` | `f6e744cc6d1b29b544a202f8b9c95092aae2df6c458671feffa06fc76b57e265` |
| `prefabs` / 689 | `cologne2026_sticker_capsule_prefab` | `7635351:7635577` | `fe0fbe63e120d660346004274fb63beee6dd0f8559d5d0d492efa0518c540024` |
| `keychain_definitions` / 690 | `84` | `7637454:7638090` | `108d0e0fdc5e3cdfbe3627aad19c909c8bfefd3ccf5441d25d8a0eee53722d2b` |
| `sticker_kits` / 696 | `10369` | `7835615:7835921` | `6586151646feb653490bcadd51045c165d2316ae3aefa3a4a9bf835ec1fa1ae8` |

## Historical schema widths for engine implementation

Source: `/root/plugify-port/backup-27035-20261007-224244/addons/swiftlys2/gamedata/cs2/sdk.json`; SHA-256 `9603e8d2178f72f15132f78dd4113e67b0f46f059f593baa9bcae3bd3682181f`. This is the 2026-10-07 SwiftlyS2 backup schema dump, not a live runtime query. No absolute field offsets from it are used here; resolve current offsets with s2sdk after the map is ready.

| Class::field | Kind/type | Bytes | Array shape |
|---|---|---:|---|
| `CBasePlayerWeapon::m_pReserveAmmo` | `fixed_array` / `int32` | 8 | 2 × 4 |
| `CBasePlayerPawn::m_pWeaponServices` | `ptr` / `CPlayer_WeaponServices` | 8 | — |
| `CBasePlayerPawn::m_pItemServices` | `ptr` / `CPlayer_ItemServices` | 8 | — |
| `CPlayer_WeaponServices::m_hMyWeapons` | `atomic` / `CNetworkUtlVectorBase< CHandle< CBasePlayerWeapon > >` | 24 | — |
| `CCSPlayerController::m_pInventoryServices` | `ptr` / `CCSPlayerController_InventoryServices` | 8 | — |
| `CCSPlayerController::m_iMusicKitID` | `ref` / `int32` | 4 | — |
| `CBaseEntity::m_iTeamNum` | `ref` / `uint8` | 1 | — |
| `CBaseEntity::m_hOwnerEntity` | `atomic` / `CHandle< CBaseEntity >` | 4 | — |
| `CCSPlayerController_InventoryServices::m_unMusicID` | `ref` / `uint16` | 2 | — |
| `CCSPlayerController_InventoryServices::m_rank` | `fixed_array` / `MedalRank_t` | 24 | 6 × 4 |
| `CAttributeContainer::m_Item` | `ref` / `CEconItemView` | 680 | — |
| `CCSPlayerPawn::m_EconGloves` | `ref` / `CEconItemView` | 680 | — |
| `CEconItemView::m_iItemDefinitionIndex` | `ref` / `uint16` | 2 | — |
| `CEconItemView::m_iEntityQuality` | `ref` / `int32` | 4 | — |
| `CEconItemView::m_iItemID` | `ref` / `uint64` | 8 | — |
| `CEconItemView::m_iItemIDHigh` | `ref` / `uint32` | 4 | — |
| `CEconItemView::m_iItemIDLow` | `ref` / `uint32` | 4 | — |
| `CEconItemView::m_iAccountID` | `ref` / `uint32` | 4 | — |
| `CEconItemView::m_bInitialized` | `ref` / `bool` | 1 | — |
| `CEconItemView::m_AttributeList` | `ref` / `CAttributeList` | 120 | — |
| `CEconItemView::m_NetworkedDynamicAttributes` | `ref` / `CAttributeList` | 120 | — |
| `CEconItemView::m_szCustomName` | `fixed_array` / `char` | 161 | 161 × 1 |
| `CEconEntity::m_AttributeManager` | `ref` / `CAttributeContainer` | 760 | — |
| `CAttributeList::m_Attributes` | `atomic` / `CUtlVectorEmbeddedNetworkVar< CEconItemAttribute >` | 104 | — |

**Handoff correction:** `CBasePlayerWeapon::m_pReserveAmmo` is a fixed array of two int32 values in this dump, not a pointer despite its `m_p` spelling. Do not copy the documented `*(int32_t**)` pattern without a current runtime type/size check. `m_hMyWeapons` is a 24-byte vector object, not a CHandle itself. `m_AttributeManager`, `m_Item`, both attribute lists and `m_EconGloves` are embedded objects, not pointers.

**Attribute-vector check:** `CAttributeList::m_Attributes` is a 104-byte `CUtlVectorEmbeddedNetworkVar`, not the plain CUtlVector header assumed by the C# helper. Independent read-only disassembly of the current signature-resolved SetOrAddAttribute function (file offset `0x1ca66e0`, VA `0x1ca76e0`, `.text` VA minus file offset `0x1000`) reads the element count at CAttributeList `this + 8` (`0x1ca771b`) and the data pointer at `this + 16` (`0x1ca7740`). Its insertion helper at VA `0x1ca74c0` reads count at `this + 8` (`0x1ca757d`), compares capacity at `this + 24` (`0x1ca7584`), writes count at `this + 8` (`0x1ca7599`), and reads data at `this + 16` (`0x1ca75a3`). The saved schema places the m_Attributes object at list + 8, so the internal relative header offsets are count + 0, data + 8, capacity + 16. Absolute field offsets still must be resolved at runtime. The old count + 16 helper writes the capacity field; do not test it against a fake that merely reproduces those assumptions.

`m_rank` holds six 4-byte elements; pin slot `[5]` is therefore inside the array. Controller music is 4 bytes while inventory-services `m_unMusicID` is 2 bytes. Name-tag writes must remain within 161 bytes including termination. Runtime layout and network notifications still need the authorized game-server integration tests.

## Reproduce

```bash
python3 handoff/weaponskins/analysis/implementation-20261008/extract_fixture.py
```

The extractor verifies VPK entry CRC32/length, parses full source and excerpt, and checks that retained definition and English token spans match verbatim. It overwrites only this excerpt, this source note, and the explicitly authorized local full items payload.
