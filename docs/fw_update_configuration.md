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
