# Conan profiles

`profiles/cpp20` layers the required C++20 setting over Conan's detected local
`default` profile. Detect that profile once per toolchain:

```shell
conan profile detect --force
```

CI uses fixed operating-system images and the same overlay. Dependency versions
are exact in the root recipe. A compiler-image update must be reviewed together
with its detected Conan settings and the static/shared test matrix.

`profiles/windows-msvc` selects `NMake Makefiles` for local Visual Studio
toolchains newer than the installed CMake knows by generator name. Invoke Conan
through `scripts\\with-msvc.cmd` when using that profile. CI uses its runner's
native, CMake-supported Visual Studio generator instead.
