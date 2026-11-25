from pathlib import Path

root = Path(__file__).resolve().parents[1]
core = (root / 'src/SuperAxiomForce/SuperAxiomForce.cpp').read_text()
targeting = (root / 'targeting/src/UnifiedTargeting.cpp').read_text()
trace = (root / 'targeting/src/TargetTraceRuntime.h').read_text()
native_hooks = (root / 'targeting/src/TargetTraceHooks.h').read_text()
installer = root / 'src/GameMidHook.h'
issues = []
if any(identifier in targeting for identifier in ('HashModule', 'TimeDateStamp', 'CheckSum', 'kGameSha')):
    issues.append('Unified targeting still admits a fixed executable identity')
if 'Flash605.h' in targeting or 'PollOptionalMenu' in core:
    issues.append('SAF still loads the Flash adapter that writes DesertLink descriptors')
if 'quiesced_patch::Write' in native_hooks or '#include "TargetTraceHooks.h"' not in trace:
    issues.append('Unified trace still writes its own executable hooks')
if not installer.is_file() or 'safetyhook::MidHook::create' not in installer.read_text():
    issues.append('No validated SafetyHook installer')
for issue in issues:
    print('FAIL ' + issue)
if issues:
    raise SystemExit(1)
print('PASS targeting uses verified native hooks with game-family admission and read-only mod compatibility')
