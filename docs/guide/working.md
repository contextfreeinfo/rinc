# Working on Rinc

## Tests

`ctest` in a desktop build runs every app headless, runs home and the console (typing at it with
`-type`), checks `rinc -m`, compares an exported game's screen with rinc's, checks [`pc.rio`](https://github.com/rincpc/rinc/blob/main/src/core/pc.rio) against the host procs Rinc
registers, and runs every `rio` example in these docs with a headless rinc (`rinc main.rio -frames 2`),
using the runner from Rio (`lib/rio/tests/docs.py`). The examples follow
[Rio's conventions](https://contextfreeinfo.github.io/rio/spec/language.html#reading-the-examples): an
`output` block after one is its exact `log` output.

## Rio at the same time

`RIO_DIR` picks the Rio to build against. It defaults to the `lib/rio` submodule; point it at a working
copy to change both without committing in between (`tools/web.py` takes `--rio=path`):

```sh
cmake -S . -B build-desktop -DRINC_PLATFORM=desktop -DRIO_DIR=../rio
```

When a Rio change is pushed, move the submodule to it: `git -C lib/rio pull`, and commit `lib/rio` here.

## CI and releases

`.github/workflows/ci.yml` builds and tests on every push: the web players' wasm (with zig), the
desktop rinc for Windows, macOS (universal) and Linux with those built in (each runs the tests), and
`rinc.uf2` for the Fruit Jam (Arm's toolchain and the Pico SDK). Each is kept as a download on the run. A
tag like `v0.1.0` also publishes them as a GitHub release. Each night that `main` has changed, and
whenever the ci workflow is run by hand (Actions > ci > Run workflow), they're published as the rolling
[nightly](https://github.com/rincpc/rinc/releases/tag/nightly) prerelease.

`.github/workflows/pages.yml` makes the [demo site](https://rincpc.com/) on each push to `main`: a page
for each app, an index, and these docs under `/docs`.
