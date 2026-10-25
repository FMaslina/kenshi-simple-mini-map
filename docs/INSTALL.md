# Installing MiniMap

## 1. Prerequisites

- Kenshi **1.0.65** (Steam or GOG).
- **RE_Kenshi** installed in the game folder (provides `RE_Kenshi.dll`).
- **KenshiLib** installed in the game folder (provides `KenshiLib.dll`,
  `MyGUIEngine_x64.dll`, `OgreMain_x64.dll`).

## 2. Build

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  .\MiniMap.slnx /m /p:Configuration=Release /p:Platform=x64
```

This produces `x64\Release\MiniMap.dll`.

> **Do not disable Whole Program Optimization.** The project sets
> `<WholeProgramOptimization>true</WholeProgramOptimization>` in `MiniMap.vcxproj`
> and this is required, not an optimization preference. Without `/GL` the address
> of any KenshiLib function resolves to a local import thunk inside `MiniMap.dll`,
> and `KenshiLib::GetRealAddress()` asserts with
> *"Incorrect address in KenshiLib::GetRealAddress()"* on startup.

## 3. Deploy

Create a mod folder inside the game's `mods` directory and copy two files into it:

```
<Kenshi>\mods\MiniMap\
    MiniMap.dll
    RE_Kenshi.json
```

(`RE_Kenshi.json` simply lists `MiniMap.dll`; it is already in this repo.)

Default paths:

- Steam: `C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\MiniMap\`
- GOG:   `<GOG install>\mods\MiniMap\`

PowerShell example:

```powershell
$kenshi = 'C:\Program Files (x86)\Steam\steamapps\common\Kenshi'
$dest = Join-Path $kenshi 'mods\MiniMap'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item .\x64\Release\MiniMap.dll $dest -Force
Copy-Item .\RE_Kenshi.json          $dest -Force
```

## 4. Enable

1. Launch Kenshi through **RE_Kenshi**.
2. In the mods list, enable **MiniMap** (RE_Kenshi plugins appear there).
3. Start / load a game — the minimap appears in the top-left corner.

## 5. Verify

- `MiniMap:` lines should appear in `<Kenshi>\RE_Kenshi_log.txt`.
- A `MiniMap.log` file is also written next to the game executable.
- Drop a marker on the big map (M) — it should show up on the minimap.
- **Middle-click** somewhere on the big map: the minimap gets a marker (and a
  rename box appears for a name). Middle-clicking it again, or walking to it,
  removes it.

## Removing

Delete `<Kenshi>\mods\MiniMap\` and disable the mod in the launcher. Your markers
live in `MiniMapMarkers.txt` inside that folder (delete it too if you no longer
want them).
