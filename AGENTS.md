# Project instructions

## Dependency integration

- Integrate the MDL SDK and MaterialX through the CMake subproject/`FetchContent` setup in `CMakeLists.txt`.
- Do not search the machine for preinstalled SDKs or libraries, hard-code machine-specific paths, or link arbitrary local binaries.
- Keep dependency fetching opt-in unless the user explicitly asks to change that behavior.
- If changing dependency integration is necessary, preserve the project’s portable CMake configuration and ask before switching to local installations.

## Code quality

- Prefer clear, cohesive components with small, focused functions and descriptive names.
- Keep coupling low and separate command-line parsing, conversion logic, and SDK lifecycle management.
- Use object-oriented design where it clarifies ownership, responsibilities, or state; avoid classes that only add indirection.
- Reduce duplication with shared helpers when they express the same behavior.
- Keep comments sparse; use them to explain non-obvious intent rather than restating the code.
- Make errors explicit and actionable.
- Preserve existing CLI behavior during refactors, and add or update focused tests for behavior and edge cases.
- Treat generated files under build directories as outputs: inspect them when useful, but change their generator or source inputs rather than hand-editing generated artifacts.
