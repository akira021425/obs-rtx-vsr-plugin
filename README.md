# OBS RTX VSR + AI Frame Interpolation

This is an OBS plugin that utilizes NVIDIA RTX Video SDK for VSR (Video Super Resolution), Artifact Reduction, and AI Frame Interpolation.

## Requirements
- Windows 10 / Windows 11 64-bit
- NVIDIA GeForce RTX 4060 (or compatible RTX GPU)
- OBS Studio (28.0+)
- Visual Studio Build Tools / Visual Studio
- CMake
- NVIDIA RTX Video SDK (Future phases)

## Build Instructions (Phase 1)
```bash
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -Dlibobs_DIR="C:/path/to/obs-studio/build/libobs"
cmake --build . --config Release
```

Ensure `libobs_DIR` points to a valid OBS Studio build or SDK directory containing `libobsConfig.cmake`.
