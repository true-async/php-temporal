# Installation

`php-temporal` is a native PHP extension. It links the official Temporal Rust
core (the `sdk-core-c-bridge` cdylib), which is vendored as a git submodule
under `third_party/sdk-rust` and built once with `cargo`.

## Requirements

- PHP 8.x built with **ZTS** and the **TrueAsync** runtime (`php-config` on PATH).
- A **Rust** toolchain (`cargo`); the vendored core pins its own toolchain via
  `rust-toolchain.toml`, so `rustup` fetches the right version automatically.
- `protobuf-compiler` (`protoc`) — needed to build the core, and later for the
  generated PHP message classes.
- The usual extension build tools: `phpize`, `make`, a C compiler.

## Build

```sh
# 1. Fetch the vendored Temporal Rust core (pinned to a release tag).
git submodule update --init --recursive

# 2. Build the core C bridge (cdylib). First build is slow (~2-3 min); the
#    artifact lands in third_party/sdk-rust/target/release/.
cargo build --release -p temporalio-sdk-core-c-bridge \
  --manifest-path third_party/sdk-rust/Cargo.toml

# 3. Build the extension against the prebuilt bridge.
phpize
./configure --enable-temporal --with-php-config="$(command -v php-config)"
make -j"$(nproc)"
```

The extension is built at `modules/temporal.so`. config.m4 bakes an rpath to the
cargo target dir, so the loader finds the bridge cdylib from there in a dev
build; packaging can install it system-wide later.

## Smoke test

```sh
php -d extension="$(pwd)/modules/temporal.so" \
  -r 'var_dump(extension_loaded("temporal"), phpversion("temporal"));'
# bool(true)
# string(9) "0.1.0-dev"  => the core runtime created cleanly in MINIT and the
#                           transport classes registered.
```

Or run the full test suite (the `tests/live/` cases SKIP without a Temporal
dev server on `127.0.0.1:7233`):

```sh
php run-tests.php -q -p "$(command -v php)" \
  -d extension="$(pwd)/modules/temporal.so" tests/
```

## Rebuilding after editing config.m4

`phpize` bakes `config.m4` into the generated `configure`. After changing the
source list or link flags in `config.m4`, regenerate before configuring again:

```sh
phpize --clean && phpize
./configure --enable-temporal --with-php-config="$(command -v php-config)"
make
```
