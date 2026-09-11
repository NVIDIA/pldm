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
firmware update targets. mctpd offers no per-endpoint PLDM-type filtering, so
the opt-out is expressed as entity-manager configuration and consumed by pldmd.

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
keeps naming the same physical device across an EID reassignment.

**Assumption:** entity-manager's `PLDMExclusion` configuration is already on
the bus by the time pldmd handles a newly discovered endpoint. An MCTP
discovery pass only starts once ObjectMapper itself is up, so this holds for
the normal startup and hot-plug paths this feature targets. There is no
retry, no signal watch for a late-published or later-withdrawn exclusion, and
no tracking of state already discovered before an exclusion took effect:
`Manager::handleMctpEndpoints()` reads
`xyz.openbmc_project.Configuration.PLDMExclusion.ExcludedInventory` (unioned
across every object publishing the interface) once, on its first call, and
caches the result for the life of the daemon rather than re-querying
ObjectMapper on every discovery batch. Each call matches that call's
endpoints against its own `configured_by` target and simply never hands a
matched EID to discovery.
For a matched endpoint, pldmd sends no PLDM command (`GetPLDMTypes`,
`QueryDeviceIdentifiers`, `GetFirmwareParameters`), does not record it as a
discovered endpoint, creates no firmware inventory for it, and never selects
it as a firmware update target. Platforms that publish no such record
exclude nothing and behave exactly as before.

The update-time descriptor refresh (`UpdateManager::processStream()`,
triggered when a firmware package is processed) reaches endpoints through a
separate path that is not limited to what discovery already knows about: its
refresh set is the union of `descriptorMap`'s keys and every statically
configured EID from the MCTP transport config (`StaticEndpointID` and bridge
pool ranges), so a device not yet discovered at update time is still
refreshed. An excluded EID never enters `descriptorMap`, but can still be
statically configured, so this refresh path re-checks the same exclusion set
(via `Manager::isEidExcludedFromFwUpdate()`) before refreshing each endpoint,
using the network ID `handleMctpEndpoints()` cached the last time it saw that
EID. An EID this pldmd instance has never discovered has no cached network ID
and is not excluded by this check either - consistent with the
`configured_by`-unresolved case above, since such a device cannot yet have a
`configured_by` association to match against.

Because there is no retry or retraction, an exclusion published or changed
after pldmd has already handled a given endpoint has no effect on that
endpoint until pldmd restarts or that endpoint is rediscovered (e.g. an
mctpreactor restart re-publishing `configured_by`). This is a narrower
guarantee than the general case, chosen because it needs no persistent
exclusion state, no D-Bus match rules on the entity-manager configuration
objects, and no bookkeeping to retract already-discovered state.
