# SuperAxiomForce v1.12.2 - Crimson Desert 1.0.0.1855 config-r4

## Purpose
Fixes SuperAxiomForce ignoring INI values on the current game build.

## Root cause
Both the live bin64 INI and DMM INI had a UTF-8 BOM prefix (`EF BB BF`) before `[AxiomForce]`. The ASI was using `GetPrivateProfileStringA`, which did not match the section name and returned empty values. Because every key lookup missed, the mod used compiled defaults:

- `Range=500`
- `PullSpeed=1000`
- `StraightPull=true`
- `InstantAxiom=true`

That matches the observed symptom where `InstantAxiom=false` in `bin64\SuperAxiomForce.ini` still behaved as enabled in-game.

## Change
Ported the existing BOM-safe INI parser/config logging into the current 1.0.0.1855 SuperAxiomForce source:

- strips UTF-8 BOM from parsed lines
- parses `[AxiomForce]` and keys manually from the exact live INI path
- flushes Windows INI cache before reading
- logs the exact INI path plus raw key values read from the file

No hook offset or gameplay feature behavior was changed in this build.

## Expected log after DMM redeploy
With the current synced INI, startup should log values like:

```text
Config loaded: path=D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\SuperAxiomForce.ini exists=true Enabled=true StraightPull=false InstantAxiom=false raw={Enabled:true Range:100.0 PullSpeed:200.0 StraightPull:false InstantAxiom:false}
Range applied ... target=100
PullSpeed applied ... target=200
InstantAxiom DISABLED ...
```

## Build
- Source: `E:\CD_Mods_Master\work\SuperAxiomForce-1.0.0.1855-config-r4-20260625\src\SuperAxiomForce`
- Output: `E:\CD_Mods_Master\builds\SuperAxiomForce\v1.12.2-CD-1.0.0.1855-config-r4-20260625\SuperAxiomForce.asi`
- SHA256: `34776EF96B704A486B356CC0C9C9EB338DC56D5B1B6E50CA3E98318B928FBF85`

## Deployment
- Deployed ASI to DMM only: `E:\Downloads\Compressed\CD_Mods\DMM\mods\SuperAxiomForce.asi`
- Synced active live INI from `bin64` into DMM mods to avoid redeploy overwriting the tested values.
- `bin64` ASI was not directly replaced. DMM must redeploy it.
