# Super Axiom Force for Crimson Desert

GitHub-ready source package for the working current-patch SuperAxiomForce deployment.

## Build Identity

- Mod: SuperAxiomForce
- Target game executable: CrimsonDesert.exe 1.0.0.1670
- Target EXE SHA256: 20A035A87FA3628C732A23E1229FAE81B7AB4484507B8043173578AFD037D26A
- Active ASI SHA256: FA10725B947B0A111894139683B96C1B80140BC933E8E886F8B2CA959CD55D91
- Active INI SHA256: 86C84D471003761CD2E08112ADB75D8111A217A780E0CA2B080FC2EDCFE6CFA3

## Features

- Extends Axiom Force working range.
- Raises pull speed to match extended range.
- StraightPull support for direct aerial pulls.
- InstantAxiom support for faster Axiom Bracelet activation and pull timing.
- Runtime scan for Range/PullSpeed when static fallback offsets are stale.
- Process guard: runs only in CrimsonDesert.exe.

## Current Verified Runtime Bindings

- Range/PullSpeed scanner found current patch candidates.
- Active binding from latest log: Range RVA 0x5F5BEE8, PullSpeed RVA 0x5F5CA28.
- StraightPull ready with two patch sites; one was already patched by ImprovedAxiom/compatible hook state.
- InstantAxiom ready with three patch sites: 0x44FF8D, 0x450C02, 0x4512C4.
