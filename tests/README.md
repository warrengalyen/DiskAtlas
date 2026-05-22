# DiskAtlas tests

Unity-based unit and integration tests for `diskatlas_core`, built when `DISKATLAS_BUILD_TESTS=ON` (default).

## Build and run

```powershell
cmake -B build -DDISKATLAS_BUILD_TESTS=ON
cmake --build build --target diskatlas_tests
.\bin\diskatlas_tests.exe
```

Or via CTest (working directory is the repo root):

```powershell
ctest --test-dir build --output-on-failure
```

On Linux/macOS, `diskatlas_tests` links `diskatlas_core` **statically** (`diskatlas_core_static`)
so CTest does not load `bin/libdiskatlas_core.so` at runtime. Debian package builds skip
`dh_auto_test` (see `debian/rules`); Linux CI runs tests in the primary `build/` tree
(`.github/workflows/build-deb.yml`).

## Layout

| Path | Purpose |
|------|---------|
| `main_all_tests.c` | Unity runner |
| `test_*.c` | Tests by area (scan, CSV, wildcard, index, duplicates, MFT on Windows) |
| `support/test_helpers.c` | Fixtures path, scan wait, golden manifests |
| `fixtures/` | Checked-in trees, CSV, MFT binaries, expected manifests |

`DISKATLAS_FIXTURES_DIR` is defined at compile time to the absolute `tests/fixtures` path.

## Fixtures

- **scan_tree/** — directory scan integration (see `expected/scan_tree_manifest_*.txt`)
- **dup_tree/** — duplicate clustering (same basename + size in two folders)
- **csv/** — import/export round-trip and error cases
- **mft/** — synthetic NTFS record blobs (no live volume)

Regenerate MFT binaries after editing `gen_mft_records.py`:

```powershell
python tests/fixtures/gen_mft_records.py
```

Regenerate scan_tree file contents:

```powershell
.\tests\fixtures\setup_tree.ps1
```

`fixtures/tmp/` is gitignored and used for CSV round-trip output.

## Reproducibility

- Golden manifests use path **suffixes** (`scan_tree/...`) so absolute scan paths differ by machine.
- Scans use `DISKATLAS_SCAN_OPTION_SKIP_DUPLICATE_CLUSTERING` unless testing duplicates.
- Node lists are sorted by path before comparison.
- MFT tests use fixed binary fixtures only (no `\\.\X:` access).

## Coverage

- Directory scanning (`scan_start` on `fixtures/scan_tree`)
- Wildcard filter (`diskatlas_utf8_matches_filter`)
- CSV import/export round-trip (`diskatlas_scan_import_csv` / `diskatlas_scan_export_csv`)
- Index tree sizes (`diskatlas_index_build_tree`)
- Duplicate groups (scan `dup_tree`)
- MFT parse helpers (`da_ntfs_fixup_record`, `da_ntfs_parse_resident_attrs`) — Windows only
