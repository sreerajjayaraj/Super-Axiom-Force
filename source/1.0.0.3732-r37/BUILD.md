# Super Axiom Force r37

The r37 game-side hooks integrate the Axiom targeting, Instant Axiom, Straight Pull and stalled-release paths in one ASI. The r37 allocator walks actual memory-region boundaries. The baseline was verified on executable layouts 3663 and 3732. Original Super Axiom Force by Bambozu. The vendored SafetyHook, Zydis and Zycore sources include upstream notices.

The source checkout builds its dependencies locally. Open the x64 Native Tools Command Prompt for MSVC 19.52 with Windows SDK and run these commands from this folder.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
    cmake --build build --target SuperAxiomForce

The output is build/SuperAxiomForce.asi. The original r37 build used the MSVC driver and prebuilt static libraries. The source-only CMake command recompiles the libraries, so its file identity is recorded independently.

The verified source-only build produced SHA-256 7314D7DB45CC0739E176E3F0D121A80176C9F82ED5C9FE4CE922AA6087040BE8.
