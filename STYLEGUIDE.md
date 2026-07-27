# Style guide

Canonical coding conventions for this repository. Adapted from the CallsApp
client's `STYLEGUIDE.md` for a standalone, Qt-free native tool — the QML and
Qt-ownership sections there don't apply here.

## Naming

| What | Style | Example |
|------|--------|---------|
| Types (`class`, `struct`, `enum`, aliases) | `PascalCase` | `Manifest`, `FileOp::Kind::Replace` |
| Functions | `camelCase` | `diffManifests()`, `runApply()` |
| Non-static `class` data members | `m` + `PascalCase` | `mHandle`, `mCancelled` |
| Public `struct` data fields (DTO/POD) | `camelCase` | `installDir`, `parentPid` |
| Parameters / locals | `camelCase` | `installDir`, `expectedSha256` |
| Constants / `constexpr` | `k` + `PascalCase` | `kAttempts`, `kApplyMutexName` |
| Namespace | lowercase | `updater` |
| Source files | `snake_case` | `file_downloader.cpp`, `sha256.cpp` |
| CLI flags | `--kebab-case` | `--install-dir`, `--parent-pid` |

- Getters: `foo()`, `isFoo()`, `hasFoo()`, `canFoo()` — not `getFoo`.
- Avoid `m_foo` and trailing `_` on members.

## Ownership

- Prefer `std::unique_ptr` for ownership; non-owning is a raw pointer or reference.
- Windows handles (`HANDLE`, `BCRYPT_HASH_HANDLE`, `EVP_PKEY*`) are not
  smart-pointer-friendly directly — wrap them in a small RAII type with a
  custom deleter (see `Hasher` in `sha256.cpp`, `ApplyMutex` in `apply.cpp`)
  rather than managing them by hand at each call site.

## Error handling

- Prefer `std::expected<T, std::string>` for recoverable failures (filesystem
  ops, verification, network) over exceptions — this is a small CLI tool
  whose failures are logged and turned into an exit code, not caught partway
  up a call stack.
- Exceptions are acceptable at true parse boundaries (`nlohmann::json`
  throwing on malformed input) but should be caught close to that boundary.

## Exceptions to naming

- Wire/manifest JSON keys stay `snake_case` (`min_supported_version`); the
  parsed C++ struct fields are `camelCase` (`minSupportedVersion`).
- Third-party and vendored code (`deps/`) keeps its native style.

## Other

- `[[nodiscard]]` on functions whose return value must not be silently dropped.
- No `using namespace std` in headers.
- Include order: own header → project → third party → std.
- Comments in English (why, not what).
- No dependency on Qt, OpenSSL DLLs, or any other DLL the updater might be
  asked to replace — this binary swaps files next to itself and must not
  depend on them at runtime (`/MT`, CNG for SHA-256, vendored Ed25519).

Existing code may still mix styles; new and touched code should follow this guide.
