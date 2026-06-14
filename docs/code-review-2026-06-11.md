# Ревью кода — 2026-06-11

Состояние на коммит `6136ed6` (Core\Worker: workflow activation transport).
Ручной разбор всего расширения (temporal.c, temporal_core.c/h,
temporal_client.c, temporal_internal.h, stub/arginfo, config.m4, тесты) плюс
сверка с вендоренным c-bridge (`third_party/sdk-rust/crates/sdk-core-c-bridge`).

## Вердикт

Качество растёт от коммита к коммиту, архитектурные решения зрелые и
соответствуют зафиксированному дизайну (docs/DESIGN.md). Найдены **два реальных
дефекта lifetime-семантики** на границе с c-bridge — латентные, но именно того
класса (UAF / abort), от которого проект сознательно защищается. Их стоит
закрыть до начала Phase 3 (workflow dispatcher).

## Что сделано правильно

- **Архитектура «тонкий транспорт + PHP SDK»** — совпадает с устройством
  .NET/Ruby SDK поверх того же c-bridge; решения в DESIGN.md не расходятся с
  кодом.
- **Изоляция cbindgen-заголовка** в единственном TU (`temporal_core.c`) из-за
  конфликта неотпрефиксованных enum-констант с php.h — явно задокументировано
  (`temporal_core.h:10-15`).
- **Кросс-поточный мост дисциплинирован**: refcounted `temporal_call_t`
  (awaiter + in-flight), результат копируется наружу под мьютексом, `trigger`
  обнуляется под тем же мьютексом, поздний callback идёт по orphan-пути и сам
  освобождает результат, dispose триггера — только на reactor-потоке.
  Tokio-callback не трогает Zend/TSRM. Один паттерн
  `temporal_run_* + temporal_call_collect` на все пять операций.
- **Владение памятью** прокомментировано на каждой границе; error-пути в
  `rpcCall`/poll/complete освобождают `fail`/`details` во всех ветках
  (проверено).
- **Гигиена репозитория**: трекаются только исходники; arginfo синхронен со
  stub (перегенерирован gen_stub.php — diff пуст); тесты разделены на
  offline/live со skipif; README/CHANGELOG соответствуют коду.
- **Детали**: ретраи отданы SDK-слою во избежание двойного retry-бюджета
  (`temporal_core.c:290-292`); identity по умолчанию `<pid>@<host>`;
  `ServiceException::$statusDetails` несёт сериализованный `google.rpc.Status`.

## Проблема 1 — use-after-free после отмены корутины (критично)

`temporal_call_collect` (`temporal_client.c:247`) при отмене возвращается
немедленно, оставляя операцию в полёте. Но в c-bridge spawned-задача
захватывает ссылку, выведенную из сырого указателя:

- `temporal_core_client_rpc_call` использует `client` внутри Tokio-задачи
  (`client.rs:502-506`);
- `temporal_core_worker_poll_*` использует `worker.runtime` в error-ветке
  (`worker.rs:669-690`).

Refcount защищает только `temporal_call_t`, **не хэндл**. Сценарий:

1. Корутина отменяется внутри `rpcCall` (или poll) → метод бросает исключение.
2. Последняя ссылка на `Connection`/`Worker` умирает → `free_obj` →
   `tphp_connection_free`/`tphp_worker_free` → `Box::from_raw` drop.
3. Tokio-задача всё ещё в полёте и читает освобождённую память → UAF.

**Фикс** (его же описывает DESIGN.md §4, но токены не подключены): подключить
`temporal_core_cancellation_token_new/cancel/free`; при отмене корутины звать
`cancel` и до-suspend'иваться до `completed` (callback гарантированно
срабатывает и дёргает триггер). Тогда ни одна операция не переживает collect,
и хэндл нельзя освободить раньше времени. Альтернатива — счётчик in-flight
операций на объекте с отложенным free.

**Тест**: live-тест, где корутина паркуется в `rpcCall`/poll, отменяется, объект
уничтожается; прогон под ASAN ловит детерминированно.

## Проблема 2 — abort процесса из userland (критично)

После `finalizeShutdown()` bridge оставляет `worker.worker = None`
(`worker.rs:948`). Любой последующий вызов `pollActivityTask` /
`pollWorkflowActivation` / `initiateShutdown` / повторный `finalizeShutdown`
упирается в `.unwrap()` на `None` → Rust-паника через FFI → abort процесса.

PHP-сторона проверяет только `self->worker != NULL`, а finalize этот указатель
не сбрасывает (он нужен для `worker_free`). Краш воспроизводится двумя
строками: `$w->finalizeShutdown(); $w->pollActivityTask();`

**Фикс**: флаг `bool finalized` в `temporal_worker_obj`; после успешного
finalize отклонять все операции (poll/complete/initiate/finalize) с
исключением. `tphp_worker_free` в free_obj остаётся как есть.

## Мелочи

- `temporal_core.h:98` — комментарий «activity-only worker» устарел: код
  включает и workflow-таски (doc drift после `6136ed6`).
- Захардкоженные тюнинги воркера (слоты 100/100/100, poller maximums 5/2/1,
  sticky timeout 10s) — нормально для фазы, заслуживают TODO на вынос в опции.
- Значение metadata с `\n` внутри ломает wire-форму `key\nvalue`
  (`temporal_flatten_metadata`) — нет валидации/отказа.
- `calloc`/`malloc` в мосте не проверяются на NULL (`temporal_call_new`,
  `tphp_*_ctx`); на OOM — NULL deref.
- Отрицательный `timeout_ms` (zend_long) молча кастится в uint32.
- MSHUTDOWN при осиротевшей in-flight операции: drop Tokio-рантайма отменяет
  задачи без вызова callback'а → `temporal_call_t` утекает. Краевой случай.

## Рекомендация

Закрыть проблемы 1 и 2 (обе в зоне «hard, race-prone part», ради которой
C-ядро держат маленьким) и добавить cancellation-тест под ASAN — до того, как
наращивать Phase 3.
