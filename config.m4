dnl config.m4 for extension temporal
dnl
dnl Native asynchronous Temporal client/worker for PHP TrueAsync, built on the
dnl official Temporal Rust Core (sdk-core-c-bridge), vendored as a git submodule
dnl under third_party/sdk-rust.

PHP_ARG_ENABLE([temporal],
  [whether to enable Temporal support],
  [AS_HELP_STRING([--enable-temporal],
    [Enable Temporal support])],
  [no])

if test "$PHP_TEMPORAL" != "no"; then

  dnl TrueAsync requires a thread-safe build; the core runs its own Tokio
  dnl threads and wakes the reactor across threads.
  if test "$PHP_THREAD_SAFETY" = "no"; then
    AC_MSG_ERROR([temporal requires a ZTS (thread-safe) PHP build for TrueAsync.])
  fi

  TEMPORAL_CORE_DIR="$abs_srcdir/third_party/sdk-rust"
  TEMPORAL_CORE_INCLUDE="$TEMPORAL_CORE_DIR/crates/sdk-core-c-bridge/include"
  TEMPORAL_CORE_LIBDIR="$TEMPORAL_CORE_DIR/target/release"

  dnl The Rust core C bridge (cdylib) is built ahead of time with cargo; we
  dnl link against the prebuilt shared object (see docs/installation.md):
  dnl   cargo build --release -p temporalio-sdk-core-c-bridge \
  dnl     --manifest-path third_party/sdk-rust/Cargo.toml
  AC_MSG_CHECKING([for the prebuilt Temporal core c-bridge library])
  if test ! -f "$TEMPORAL_CORE_LIBDIR/libtemporalio_sdk_core_c_bridge.so"; then
    AC_MSG_ERROR([Temporal core c-bridge is not built. Build it first:
  cargo build --release -p temporalio-sdk-core-c-bridge \
    --manifest-path third_party/sdk-rust/Cargo.toml])
  fi
  AC_MSG_RESULT([found])

  PHP_ADD_INCLUDE([$TEMPORAL_CORE_INCLUDE])

  dnl Link the cdylib and bake an rpath so the loader finds it from the cargo
  dnl target dir (dev build; packaging can install it system-wide later).
  PHP_ADD_LIBRARY_WITH_PATH([temporalio_sdk_core_c_bridge],
    [$TEMPORAL_CORE_LIBDIR], [TEMPORAL_SHARED_LIBADD])
  TEMPORAL_SHARED_LIBADD="$TEMPORAL_SHARED_LIBADD -Wl,-rpath,$TEMPORAL_CORE_LIBDIR"

  PHP_SUBST([TEMPORAL_SHARED_LIBADD])

  PHP_NEW_EXTENSION([temporal],
    [temporal.c temporal_core.c temporal_client.c],
    [$ext_shared])
fi
