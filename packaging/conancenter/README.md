# ConanCenter submission template for ulog

This directory mirrors the layout ConanCenter expects under
`recipes/<package>/` at
[conan-io/conan-center-index](https://github.com/conan-io/conan-center-index).
Submitting ulog to ConanCenter means copying the contents of
`recipes/` here into a fork of that repo and opening a PR.

## Layout

```
packaging/conancenter/
  recipes/
    ulog/
      config.yml             # version → folder map
      all/
        conandata.yml        # source tarball URL + sha256 per version
        conanfile.py         # ConanCenter-flavoured recipe
        test_package/
          conanfile.py
          CMakeLists.txt
          test_package.cpp
```

## Differences from the repo-root `conanfile.py`

| Aspect | Repo root `conanfile.py` | This recipe |
|---|---|---|
| Source | `exports_sources = …` (bundled) | Fetched from a GitHub tarball via `conandata.yml` |
| Dev options (`build_tests`, `build_bench`) | Yes | Removed — ConanCenter packages are consumer-only |
| `version` | Pinned (`0.1.0`) | Read from the CCI `config.yml` layout |
| License handling | Relies on CMake `install` | Explicit `copy(LICENSE, licenses/)` + `rmdir(lib/cmake)` to pass CCI hooks |
| Compiler gate | None | `check_min_cppstd(17)` + `_compilers_minimum_version` map |

## Release workflow

1. Tag a new version in the ulog repo:

   ```
   git tag v0.2.0
   git push origin v0.2.0
   ```

2. Compute the tarball sha256:

   ```
   curl -L https://github.com/Arenoros/ulog/archive/refs/tags/v0.2.0.tar.gz | sha256sum
   ```

3. Edit `recipes/ulog/config.yml` and `recipes/ulog/all/conandata.yml`
   to add the new entry (keep old versions — ConanCenter wants the
   full history available). Example `config.yml`:

   ```yaml
   versions:
     "0.1.0":
       folder: all
     "0.2.0":
       folder: all
   ```

   `conandata.yml`:

   ```yaml
   sources:
     "0.1.0":
       url: ".../v0.1.0.tar.gz"
       sha256: "<hash>"
     "0.2.0":
       url: ".../v0.2.0.tar.gz"
       sha256: "<hash>"
   ```

4. Copy `recipes/ulog/` into a fork of `conan-center-index` at the
   same path. Open a PR titled `ulog/0.2.0: bump version` (following
   the CCI style). The CCI CI builds + runs `test_package/` across
   the full matrix (gcc/clang/msvc × shared/static × Debug/Release).

## Local verification

Before opening the CCI PR, validate the recipe locally against the
to-be-published version:

```
cd packaging/conancenter/recipes/ulog/all
# --version matches the version key in conandata.yml
conan create . --version=0.1.0 --build=missing
```

If the recipe builds and `test_package/` exits zero, ConanCenter CI
is likely to accept it. For a final pre-flight, run the CCI-specific
linter:

```
pip install conan-center-index-linter   # if upstream publishes one
```

or scan your recipe manually against the ConanCenter
[docs/adding_packages](https://github.com/conan-io/conan-center-index/tree/master/docs/adding_packages)
checklist.

## Upstream status

Not yet submitted. When you're ready to publish, open an issue in
this repo tagged `conancenter-release` to coordinate the bump +
sha256 refresh.
