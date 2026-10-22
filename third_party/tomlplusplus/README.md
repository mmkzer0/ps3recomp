# toml++ (vendored)

Single-header TOML 1.0 parser, used by `runtime/config/config.cpp` to read a
title's configuration file (see `docs/CONFIG.md`).

- Upstream: <https://github.com/marzer/tomlplusplus>
- Version: **v3.4.0**, tag `v3.4.0`, commit `30172438cee64926dc41fdd9c11fb3ba5b2ba9de`
- Files: `toml.hpp` (the amalgamated header from the repository root) and
  `LICENSE`, both unmodified.
- SHA-256 of `toml.hpp`: `6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e`
- License: MIT (Mark Gillard), see `LICENSE`. The rest of ps3recomp is MIT too.

The header is included only by `runtime/config/config.cpp`, which defines
`TOML_EXCEPTIONS=0` and turns off the formatters and unreleased features before
including it, so a malformed file comes back as a `parse_error` value that the
config loader turns into a named stop rather than as a C++ exception.

To update: replace `toml.hpp` and `LICENSE` from the new tag, update the version,
commit and hash above, and run `ctest -L config`.
