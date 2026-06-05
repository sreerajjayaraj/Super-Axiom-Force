# Build Notes - SuperAxiomForce current patch

Use a Visual Studio x64 developer shell from the `src` directory.

```powershell
cl /nologo /std:c++17 /O2 /EHsc /LD SuperAxiomForce.cpp /link /OUT:SuperAxiomForce.dll psapi.lib
copy /Y SuperAxiomForce.dll SuperAxiomForce.asi
```

The verified deployment artifact is included in `dist/SuperAxiomForce.asi`.

Deploy through DMM only. Do not directly copy the ASI into `bin64` unless explicitly doing a manual rollback/test with user approval.
