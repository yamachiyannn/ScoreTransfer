# Third-party notices

ScoreTransfer downloads and compiles the following dependencies at build time:

- **Dear ImGui v1.90.9** — MIT License. Copyright (c) 2014-2024 Omar Cornut.
  Source: https://github.com/ocornut/imgui
- **SQLite amalgamation 3.53.4** — SQLite is in the public domain.
  Source: https://www.sqlite.org/download.html
- **SokuLib** — included as the `SokuDev/ModTemplate` Git submodule and governed by its own license and repository notices.
  Source: https://github.com/SokuDev/SokuLib
- **Windows Imaging Component (WIC)** — used through the Windows SDK and system libraries (`windowscodecs`, `ole32`); no WIC source is copied into this repository.

Dear ImGui and SQLite are statically linked into `ScoreTransfer.dll`. The `bg.png` image remains a standalone runtime asset next to the DLL and is not embedded into the binary.
