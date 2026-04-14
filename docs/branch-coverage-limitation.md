# Branch Coverage Limitations

Coverage source: `build/meson-logs/coverage.xml` from the passing Docker CI
coverage run after rebasing `branch_cov` on `origin/develop`.

Scope: production files in the completed coverage iteration. Unit-test-fixable
branches for this set were covered during the iteration. Remaining gaps are
documented as real limitations or compiler-generated instrumentation artifacts;
this document does not require production-code changes.

## `common/dBusAsyncUtils.hpp`

Total branches: 562; covered: 410; branch coverage: 73.0%; uncovered: 152.

| Limitation (type) | Affected functions/area | Approx. % of file's branches not covered | Unblocked by (or Reason) |
| --- | --- | ---: | --- |
| Compiler-generated arcs | Template coroutine awaitable constructors, return-by-value paths, lambda captures, and mock-mode awaitable initialization for `coGetDbusProperty`, `coGetServiceMap`, and `coGetSubTree` | 25.1% (141/562) | Gcov records branches for repeated template instantiations, string/member initialization, coroutine/lambda cleanup, and value returns. Representative UT coverage is done; the rest is instrumentation noise with no production change. |
| External D-Bus error permutations | Async callback error paths in `coGetDbusProperty`, `coGetServiceMap`, and `coGetSubTree` | 2.0% (11/562) | Representative D-Bus failure UT coverage is done. Exhaustively forcing every type instantiation and async mapper failure combination would require test-only hooks or production seams, so no production change. |

## `fw-update/manager.hpp`

Total branches: 186; covered: 111; branch coverage: 59.7%; uncovered: 75.

| Limitation (type) | Affected functions/area | Approx. % of file's branches not covered | Unblocked by (or Reason) |
| --- | --- | ---: | --- |
| Compiler-generated arcs | `Manager` member initialization, local D-Bus/container construction in `getMctpInterfaces()`, and `stdexec` sender/lambda cleanup in `updateFWInventory()` | 14.5% (27/186) | Gcov records constructor/destructor, container, and sender-pipeline cleanup branches. UT coverage for the callable paths is done; remaining arcs are compiler artifacts. |
| D-Bus topology and mapper permutations | `getMctpInterfaces()` mapper failures, `GetManagedObjects` failures, nested mapper-service/object/interface loops, and missing or malformed UUID properties | 21.5% (40/186) | Real runtime topology depends on the system mapper and MCTP services. Representative success and failure UTs are done; exhaustive mapper permutations are a real limitation without production hooks. |
| Exceptional config/update paths | Constructor `parseConfig()` catch and `updateFWInventory()` exception/nonzero callback branches | 4.3% (8/186) | UT coverage covers practical behavior. Fully forcing exceptions through asynchronous sender internals would require artificial production changes. |

## `libpldmresponder/fru.cpp`

Total branches: 325; covered: 168; branch coverage: 51.7%; uncovered: 157.

| Limitation (type) | Affected functions/area | Approx. % of file's branches not covered | Unblocked by (or Reason) |
| --- | --- | ---: | --- |
| Compiler-generated arcs | D-Bus reply reads, `std::variant` extraction, vector/string/TLV moves, `resize()`/`copy()` paths, and response-buffer construction | 11.1% (36/325) | Gcov attributes branches to library and compiler cleanup paths around ordinary object movement and buffer management. UT coverage for functional paths is done; no production change. |
| Hot-plug deletion and PDR repository mutation | `deleteFRURecord()` and `removeIndividualFRU()` deletion, partial-failure, effecter/sensor PDR cleanup, and repository-change event paths | 24.0% (78/325) | These require a live PDR repository with multiple host/BMC record mutation outcomes and event side effects. UT coverage covers safe representative paths; exhaustive mutation/failure combinations are a real limitation. |
| FRU table build/response/OEM edge paths | `buildFRUTable()`, `populateRecords()`, `tableResize()`, `setFRUTable()`, `addHotPlugRecord()`, and FRU metadata/record-by-option response edge handling | 13.2% (43/325) | Remaining cases are mostly malformed/truncated table states, OEM callback combinations, and encoder failure branches. Practical UT coverage is done; additional coverage would need artificial hooks or production behavior changes. |

## `libpldmresponder/pdr_numeric_effecter.hpp`

Total branches: 259; covered: 234; branch coverage: 90.3%; uncovered: 25.

| Limitation (type) | Affected functions/area | Approx. % of file's branches not covered | Unblocked by (or Reason) |
| --- | --- | ---: | --- |
| Compiler-generated arcs | Static JSON default construction, `std::vector` storage, `reinterpret_cast` null check after `resize()`, and repeated template instantiations | 4.2% (11/259) | The null check is effectively defensive because `entry.data()` follows a successful resize. Remaining arcs are compiler and template instrumentation; no production change. |
| JSON/entity-association permutations | `generateNumericEffecterPDR()` entity-path lookup, default entity fallback, and exception fallback around JSON extraction | 3.9% (10/259) | UT coverage exercises representative associated/default entity behavior. Exhaustive malformed JSON exception combinations are a real limitation without production hooks. |
| Switch and D-Bus lookup edge cases | Effecter/range data-size switch fallbacks and `getService()` exception handling | 1.5% (4/259) | UT coverage covers practical enum cases and lookup behavior; the remaining default/error-only branches are low-value defensive paths. |

## `platform-mc/state_set/ethIBPortLinkState.hpp`

Total branches: 166; covered: 143; branch coverage: 86.1%; uncovered: 23.

| Limitation (type) | Affected functions/area | Approx. % of file's branches not covered | Unblocked by (or Reason) |
| --- | --- | ---: | --- |
| Compiler-generated arcs | D-Bus interface construction, `getStringStateType()` return, Telemetry Aggregator calls, association vector reserve/copy, and `std::regex_replace()` object-path sanitization | 7.2% (12/166) | Gcov records library cleanup and value-return arcs around D-Bus/TAL/string operations. Representative UT coverage is done; remaining arcs are instrumentation artifacts. |
| Derived sensor association state | `isDerivedSensorAssociated()` and switch-bandwidth association/update paths | 5.4% (9/166) | UT coverage covers practical association behavior. Remaining combinations depend on optional OEM-derived sensor wiring and do not require production changes. |
| Sensor event metadata variants | Link-down event metadata and impacted-component mapping | 1.2% (2/166) | Representative LinkUp/LinkDown/Error behavior is covered. Remaining metadata-only permutations are UT-low-value and do not unblock production changes. |

## `platform-mc/terminus.cpp`

Total branches: 615; covered: 492; branch coverage: 80.0%; uncovered: 123.

| Limitation (type) | Affected functions/area | Approx. % of file's branches not covered | Unblocked by (or Reason) |
| --- | --- | ---: | --- |
| Compiler-generated arcs | Constructor/member initialization, loops over vectors of smart pointers, UTF string construction, `std::make_shared`, `std::format`, static map initialization, and `stdexec` spawn cleanup | 9.3% (57/615) | Gcov records STL/string/smart-pointer/format/sender cleanup branches. UT coverage for observable behavior is done; remaining arcs are compiler-generated. |
| PDR parser exceptional paths | Auxiliary-name UTF conversion exceptions, outer parse exceptions, numeric PDR decode failures, OEM PDR copy paths, and non-compliant numeric-effecter fallback handling | 3.9% (24/615) | Representative valid and invalid PDR UT coverage is done. Fully corrupting every parser boundary or firmware workaround branch would require synthetic hooks or production changes. |
| Inventory and naming permutations | `findInventory()`, entity-type tag building, terminus/entity prefixing, backward-compatible pre-prefixed names, and CPU-index naming variants across numeric/state sensors and effecters | 5.2% (32/615) | UT coverage handles practical naming and association cases. The remaining combinations depend on firmware inventory topology and legacy naming formats, so they are real limitations without production changes. |
| Lifecycle and refresh edges | `interfaceAdded()`, `setOffline()`, auxiliary lookup returns, and refresh-task reentry/empty-scope branches | 1.6% (10/615) | Unit-testable paths were covered where practical. Remaining behavior depends on D-Bus signal timing, populated device lists, or async task state transitions. |

## `pldmd/pldmd.cpp`

Total branches: 471; covered: 354; branch coverage: 75.2%; uncovered: 117.

| Limitation (type) | Affected functions/area | Approx. % of file's branches not covered | Unblocked by (or Reason) |
| --- | --- | ---: | --- |
| Compiler-generated arcs | OEM event-handler lambda table, duplicate preprocessor build paths, unique-pointer deleter setup, D-Bus object construction, and lambda cleanup | 14.4% (68/471) | Gcov records branches for generated lambda bodies, initializer-list cleanup, RAII deleters, and duplicate compiled configurations. Subprocess UT coverage is done; remaining arcs are compiler artifacts. |
| Startup fatal-allocation paths | PDR repository and entity-tree allocation failures, TAL init logging, and HostPDR handler setup branches | 3.8% (18/471) | These require forcing low-level C allocation failures or platform initialization failures in `main()`. UT coverage covers safe startup behavior; no production hook was added. |
| Transport/event-loop error-only paths | EPOLLERR-only handling, MCTP error queue parsing, FWUP transport-error storage, non-EPOLLIN returns, invalid fd, send failure, and D-Bus name request failure | 4.7% (22/471) | Representative subprocess and handler UT coverage is done. Fully exercising kernel/socket error queue and event-loop timing combinations is a real limitation without production changes. |
| Request/response exception paths | `processRxMsg()` edge lengths, unsupported command response packing failure, and `notifyFwUpdateSendComplete()` exception handling | 1.9% (9/471) | UT coverage handles practical request, response, and short-header paths. Remaining exception-only branches require artificial failure injection that was intentionally not added to production code. |
