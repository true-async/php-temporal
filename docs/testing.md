# Testing

## Unit-style tests (no server needed)

```sh
php run-tests.php -q -p "$(command -v php)" \
  -d extension="$(pwd)/modules/temporal.so" tests/*.phpt
```

These cover extension loading, the class/enum/exception surface, the option
value objects, and the connect-refused path of the async bridge.

## Live tests (need a Temporal frontend)

Tests under `tests/live/` are guarded by `--SKIPIF--` and skip automatically when
no server is reachable. Bring up a local dev server with Docker:

```sh
docker run -d --name temporal-dev -p 7233:7233 -p 8233:8233 \
  temporalio/temporal server start-dev --ip 0.0.0.0 --log-level error
```

This is the in-memory Temporal dev server (frontend on `127.0.0.1:7233`, Web UI
on `http://127.0.0.1:8233`). Then:

```sh
php run-tests.php -q -p "$(command -v php)" \
  -d extension="$(pwd)/modules/temporal.so" tests/live/*.phpt
```

Override the target with `TEMPORAL_ADDRESS` / `TEMPORAL_NAMESPACE` env vars.
Tear the server down with `docker rm -f temporal-dev`.

## AddressSanitizer

The cross-thread bridge is the race-prone part; run the suite under ASAN when
touching it. Build the extension against an ASAN PHP (CFLAGS
`-fsanitize=address -fno-omit-frame-pointer -g -O0`, LDFLAGS
`-fsanitize=address` on php-src, then the same CFLAGS on the extension's
configure) and run with the allocator handed to ASAN:

```sh
USE_ZEND_ALLOC=0 ASAN_OPTIONS=detect_leaks=0 php run-tests.php -q \
  -p "$(command -v php)" -d extension="$(pwd)/modules/temporal.so" tests/
```

`016-cancel-inflight-rpc.phpt` is the test to stress here (cancellation racing
the in-flight callback); the 2026-06-12 baseline: full suite + 10 iterations of
016, no reports.
