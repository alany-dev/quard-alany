# Repository Guidelines

## Project Structure & Module Organization
- `build.sh` compiles QEMU (`riscv64-softmmu`) and installs binaries to `output/qemu/`.
- `run.sh` launches the `quard-star` machine using the locally built binary.
- `qemu-8.0.2/` contains the QEMU source tree; most board-level changes belong here.
- `riscv64-elf-ubuntu-24.04-gcc/` provides the cross-toolchain used by this environment.
- `doc/` stores setup notes and build references; keep procedural documentation here.
- `os/` is reserved for OS/firmware integration work. Build artifacts should stay in `output/` and never be committed unless explicitly required.

## Build, Test, and Development Commands
- `sudo ./build.sh` — configures and builds QEMU, then installs into `output/qemu/`.
- `./run.sh` — starts the headless RISC-V64 emulator (`-nographic`, `-M quard-star`).
- `cd qemu-8.0.2 && make -j$(($(nproc)/2+1))` — incremental rebuild when iterating on QEMU source.

Install required packages before first build (see `README.md` for the full dependency list).

## Coding Style & Naming Conventions
- Follow existing language conventions in each area (Bash in root scripts, upstream style inside `qemu-8.0.2/`).
- Keep shell scripts POSIX-friendly where practical; use clear variable names like `SHELL_FOLDER`.
- Name new scripts/files in lowercase with hyphen/underscore (`sync-toolchain.sh`, `board_notes.md`).
- Keep docs concise and task-oriented; mirror existing `doc/` naming patterns.

## Testing Guidelines
- No automated test suite is configured at repo root.
- Minimum validation for code changes:
  1. Rebuild (`sudo ./build.sh` or incremental `make`).
  2. Boot emulator with `./run.sh`.
  3. Confirm QEMU starts without immediate errors.
- For board/device changes, include a short manual verification note in the PR.

## Commit & Pull Request Guidelines
- Match current history style: short, imperative summaries (English or Chinese), e.g., `init qemu riscv env` or `修复 quard-star 启动参数`.
- Keep commits focused (build scripts, docs, and QEMU changes separated when possible).
- PRs should include: purpose, key changes, run/verify steps, and terminal output snippets for build/boot results.
- Link related issues/tasks and call out environment assumptions (Ubuntu version, required packages).

## Security & Configuration Tips
- Avoid committing generated binaries from `output/` unless necessary for release artifacts.
- Review shell scripts for `sudo` usage and keep privileged steps explicit.
- Prefer reproducible commands and pinned paths to reduce environment drift.
