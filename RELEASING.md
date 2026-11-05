# Releasing

1. Bump `VERSION` in `CMakeLists.txt` (the tag build fails when the tag and this don't match), and `FH3MOD_API_VERSION` in `include/fh3mod.h` if the API grew.
2. Add the version to `CHANGELOG.md`, and update `README.md` and `docs/api.md` if anything players or mod makers see changed.
3. Build (`./scripts/build.sh`), copy `dist/` to a clean Azimuth folder, start the game and check `fh3-mods\platform.log` for errors.
4. Commit, then tag and push the tag:

```sh
git tag v1.0.0
git push origin v1.0.0
```

The `Build` workflow builds again, checks the tag against `CMakeLists.txt` and attaches `FH3-Mod-Platform-v1.0.0.zip` (`dist/`) to the release of that tag; with no release yet it creates a draft with generated notes. Paste the CHANGELOG section into the release notes and publish.

Mods compare their registered `platform_version` with this version (and the update check compares GitHub's latest tag with the version a mod registers), so keep the tag format `vX.Y.Z`.
