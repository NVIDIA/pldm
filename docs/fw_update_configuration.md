# Firmware Update Configuration

PLDM supports firmware updates through two mechanisms:

1. **D-Bus API**: Using the StartUpdate D-Bus interface for firmware updates
2. **Inotify monitoring**: Automatic detection of firmware packages placed in
   `/tmp/images`

The inotify-based firmware update monitoring can be enabled or disabled using
the meson option `fw-update-pkg-inotify`. When enabled, pldmd will automatically
monitor the `/tmp/images` directory for new firmware packages and process them
automatically. When disabled, only D-Bus API-based firmware updates will be
supported. To disable inotify-based firmware update monitoring (default):

```bash
meson setup build -Dfw-update-pkg-inotify=disabled
```

To enable inotify-based firmware update monitoring:

```bash
meson setup build -Dfw-update-pkg-inotify=enabled
```

## Excluding endpoints from firmware update

Some downstream devices advertise PLDM type 5 but are updated through another
path (for example a downstream BMC), so this BMC must not treat them as
firmware update targets. The opt-out is expressed as entity-manager
configuration and consumed by pldmd.

Add a `PLDMExclusion` record to the platform's entity-manager configuration,
naming the inventory path of the device to exclude:

```json
{
    "Name": "Platform_FwUpdate_Exclusion",
    "Type": "PLDMExclusion",
    "ExcludedInventory": [
        "/xyz/openbmc_project/inventory/system/platform/Nvidia_VR_NVL72_BMC/IO_Board_SMA_2"
    ]
}
```

Each entry must be the exact inventory path the excluded device's own
`configured_by` association resolves to — the same path mctpreactor publishes
on the MCTP endpoint's `xyz.openbmc_project.Association.Definitions`
`Associations` property, e.g.:

```text
$ busctl get-property xyz.openbmc_project.MCTPReactor \
    /au/com/codeconstruct/mctp1/networks/1/endpoints/80 \
    xyz.openbmc_project.Association.Definitions Associations
a(sss) 1 "configured_by" "configures" "/xyz/openbmc_project/inventory/system/platform/Nvidia_VR_NVL72_BMC/IO_Board_SMA_2"
```

pldmd matches by this inventory identity rather than by EID, so the opt-out
keeps naming the same physical device across an EID reassignment. A matched
endpoint gets no PLDM command, no discovery, no firmware inventory, and is
never selected as a firmware update target. Platforms with no such record
exclude nothing and behave exactly as before.

**Assumption:** entity-manager's `PLDMExclusion` configuration is already on
the bus by the time pldmd handles a newly discovered endpoint (true for the
normal startup and hot-plug paths, since MCTP discovery only starts once
ObjectMapper is up). pldmd fetches the exclusion set once and caches it for
its lifetime, so there is no retry, no watch for a later-published or
withdrawn exclusion, and no retraction of state already discovered before an
exclusion took effect. An exclusion published or changed after pldmd has
already handled a given endpoint takes effect only on the next pldmd restart
or rediscovery of that endpoint.

## Pre and Post Update Condition Services

PLDM firmware update supports optional pre and post update condition services
that can be executed during the firmware update process. These services are
specified in the configuration files and are used for device-specific handling
such as checking conditions before update or performing cleanup/validation after
update.

`PreUpdateTarget` and `PostUpdateTarget` hold a full systemd unit name, for
example `ABC_PreUpdate.service`. Omit the property when a component has no such
condition; an empty unit name is rejected by the
[schema](../configurations/fw-update-conditions/schema/schema.json) and is
reported at runtime instead of being silently skipped.

### Condition Service Completion and Timeouts

PLDM starts a condition unit with the systemd `StartUnit` method and waits for
its `JobRemoved` signal, as described in the
[code update design](https://github.com/openbmc/docs/blob/master/designs/code-update.md#pre-and-post-update-conditions).
Only the job result `done` is treated as success; any other result fails the
condition and, for a pre-update condition, aborts the update.

PLDM applies no timeout of its own, because systemd reports every dequeued job
through `JobRemoved` exactly once. A condition unit that can hang is therefore
expected to bound its own execution time, which makes systemd terminate the unit
and deliver the resulting failure through `JobRemoved`. The directive to use
depends on the service type: `TimeoutStartSec=` bounds a `Type=oneshot` unit,
for which `RuntimeMaxSec=` is ignored, while `RuntimeMaxSec=` caps the runtime
of a unit that reaches the active state. Without such a bound a hanging
condition unit leaves the firmware update waiting indefinitely.

```ini
[Service]
Type=oneshot
TimeoutStartSec=60
ExecStart=/usr/bin/example-pre-update
```

### Condition Service Arguments Format

A condition service that needs arguments is configured as a systemd template
unit, following the standard `<name>@.service` naming convention. PLDM then
passes all supported named arguments as the instance name of that unit; a unit
name that is not a template receives no arguments at all. There is no
per-service argument selection, so a template unit has to tolerate arguments it
does not use.

The argument string is a comma separated list of `<key>=<value>` pairs:

```text
boardName=<boardName>,applyTime=<applyTimeValue>
```

Where:

- `<boardName>`: Name of the inventory board the device belongs to. Omitted when
  no board name is available.
- `<applyTimeValue>`: One of:
  - `Immediate` - Apply the update immediately
  - `OnReset` - Apply the update on next system reset
  - `OnStart` - Apply the update on next system start

Since `=` and `,` are not valid characters in a systemd unit name, PLDM escapes
the instance name as `systemd-escape` would. A unit recovers the original
argument string with the `%I` specifier; `%i` yields the escaped form.

### Examples

**Example 1: Condition service without arguments** With
`"PostUpdateTarget": "ABC_PostUpdate.service"`, PLDM starts:

```text
ABC_PostUpdate.service
```

**Example 2: Parameterized condition service** With
`"PostUpdateTarget": "ABC_PostUpdate@.service"`, the board name `board1` and
`applyTime` `Immediate`, PLDM starts the escaped instance:

```text
ABC_PostUpdate@boardName\x3dboard1\x2capplyTime\x3dImmediate.service
```

`ABC_PostUpdate@.service` then sees `boardName=board1,applyTime=Immediate`:

```ini
[Service]
Type=oneshot
TimeoutStartSec=60
ExecStart=/usr/bin/example-post-update %I
```

### Post-Condition Service Processing

Post-condition services receive these arguments and can use them for conditional
processing. For example:

- Skip system reset if `applyTime` is not `Immediate`
- Perform device-specific validation based on `applyTime`
- Schedule deferred operations based on the apply time value

The service should interpret these parameters to determine appropriate actions
for the given apply time.
