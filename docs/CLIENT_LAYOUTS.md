# Exact client adapters — ml1860

"Private" below means Valve's undocumented client interfaces. These are
compatibility adapters for genuine Valve libraries, not replacements for the
libraries or their authentication/DRM.
An unrecognized SHA-256 or mismatched method address fails closed. Interface
names and PE timestamps alone never authorize private calls.

The previously supported September build has SHA-256
`caba4826aa3501039d095aee1843a6bfb270fb43a3ab4455b2d6733223579fee`.
ml1860 also supports the owner's device's exact January build:
`71b391fe9f3e2006cbc81a5c75eef3eb4186012deabfdb2c8b7e8d4850ecf640`,
PE timestamp 1769726215. `MADEIRA_DOCK_CLIENT_202601=0` disables this adapter.
`[steam-host] ml1860 session-client-adapter=202601` identifies selection;
it is not authentication or entitlement evidence.

## Provenance and independent inspection

Fetched from Valve on 2026-09-24 via HTTPS:
`https://client-update.akamai.steamstatic.com/steam_client_win32`.
The returned manifest reports version 1769731672 and names
`bins_win32.zip.23e34a6d4b10596a44561a5100dac5585d2517da`, downloaded
from the same host. Archive SHA-256 matches its manifest:
`8b712b2a3412a9066b7725f4e1c5cef9a7ca5b187b6585a5b92d25d09df0ba62`.
Its steamclient64.dll matches the device report byte fingerprint exactly.
Windows Authenticode verification reports Valid, signer Valve Corp.
Downloaded files and analysis output stay ignored in `.build/client-jan2026`.
No Valve DLLs or archives are included in the app/repository distribution.

RTTI gives offset-zero vtables CSteamClient at RVA 0x12ec718,
IClientUserMap at 0x12b8938, IClientAppManagerMap at 0x12c0278.
All 14 called private methods are pinned in `src/client_layout.c`. User and
manager method names were independently checked via their RIP-relative IPC
strings, including LogOn, SetLoginToken, BIsSubscribedApp, GetSubscribedApps,
LaunchApp and GetAppInstallDir. Slots are unchanged between these two builds.

Disassembly comparison of the called wrappers (through their first return)
retains identical argument registers/stack widths and return widths. Engine
user/manager wrappers have identical normalized instructions. Differences in
the others are equivalent signed-negative cleanup tests and reordered loads
before the same underlying call; manager IPC type constants vary by build.
Those constants are implemented by Valve's own matching library, not Dock.
GetAPICallResult still forwards the same buffer/size/callback/failure args.
This inspection is bounded ABI evidence, not full behavioral compatibility.
The launch callback remains strictly checked for ID 1270027, 524 bytes, the
requested GameID and success. Do not loosen it if a future device test fails.

Reproduce the independent RTTI/table checks with Python (no DLL execution):

```
python tools/check-client-layout.py PATH_TO_OFFICIAL_STEAMCLIENT64_DLL
```

Portable sanitizer tests reject unknown/partial/changed fingerprints and
mismatched method pointers. A native Windows probe using the isolated official
January DLL verified all session pointers, with login/launch disabled, and
exited 0. Disabling the January adapter reproduces rejection 30. This does not prove native-token authentication or iOS game launch.
The original supported September table is also checked for regression.

## Device result motivating the change

Log 60 / ml1850 got past the earlier native stale-image crash, loaded both
interfaces, then returned session-unsupported-client=1 / probe-result=30.
It never reached authentication. The apparent infinite wait was separately a
public runtime notification gap on normal Wine exit. ml1860 fixes the common
exit wrapper and exports a bounded whitelist of Dock report fields, including
this adapter revision and fingerprint, in the normal Madeira log.

## ml1990 — custom executable (CEG) preparation

Both pinned builds add one called method, independently identified in each:
IClientUserMap slot 71 wrapper, September RVA `0x84e8b0`, January RVA
`0x82a8c0`. Slot 70 is GetCustomBinariesState (`0x769b60` / `0x74cc60`;
not called). Each wrapper names "RequestCustomBinaries" through a
RIP-relative LEA and serializes IPC function ID `0x08711e2c` and fencepost
`0x08b15ab0`; `tools/check-client-layout.py` now verifies slot, RVA, name
and function ID. Slot 70's wrapper fails that name check (January negative control).

Wrapper ABI (both builds, identical instructions except call targets):
rcx this, edx u32 AppID, r8b bool, r9b bool, one stack argument serialized
as 8 bytes; it reads one response byte (must be 11) and a 4-byte result,
returning 55 (k_EResultRemoteCallFailed) on IPC failure. The server
dispatcher (`cmp eax,0x8711e2c`) calls the user object's slot 71 with the
same five values. That implementation (`0x9b0860` Jan / `0x9e0d40` Sep)
immediately writes `0` through the fifth value, so it is a `uint32 *`
(the older 32-bit reference lists it as bytes4), then returns: 2 unknown
or uninstalled app, 10 app busy (updating/running flags), 1 with no
CustomExecutable entries, 9 no install folder, else 1 and the count of
per-file jobs started or already in progress. OpenSteamworks reference
lists the older build as `RequestCustomBinaries(AppId_t, bool, bool, uint)`.

First bool: skip DRM-identifier extraction and force a refresh. With zero
identifiers the job reports success without downloading, so Dock passes
false. Second bool: job flag 0x10, which ends the job with Busy (10) after a
server Busy reply instead of Valve's own retry loop; Dock passes false and
bounds the total wait itself (5 minutes).

Completion: CDRM::OnDRMDownloadJobFinish posts callback **1020025**
(`0xf9079`), 8 bytes: int32 EResult, uint32 AppID, once per job (source file
clientjobrequestcustombinary.cpp; strings "OnDRMDownloadJobFinish: %u (%s)").
Each server reply also posts 1020024 (`0xf9078`), 12 bytes: EResult, AppID,
bool. Identical IDs/sizes in both builds. The launch-time runtime DRM path
posts 1020025 with the same layout (Timeout 16) after a 0x8000DEAD report.

LaunchApp does not itself request custom binaries: the only callers of the
per-file request (`0x4f2a50` Jan) are this IClientUser method and the running
game's legacy DRM IPC fix-up. Hence the explicit request before LaunchApp.
Extraction reads PE resources (STEAM_SPLIT_GUID/STEAM_GUIDD/STEAM_MINSTANCE)
in process. The file list comes from the app manifest's CheckGuid entries.
Delivery of 1020025 to a global-user pipe and server success remain unproven
at runtime: this is static evidence only.

## Offline logon (see OFFLINE.md)

Both pinned builds add three called methods on IClientUserMap, at the same
slots in each: 5 `GetLogonState` (September RVA `0x77e460`, January
`0x7612c0`, IPC function `0xb3679023`), 214 `CanLogonOffline` (`0x73cbb0` /
`0x720330`, `0xe391b9f0`) and 215 `LogOnOffline` (`0x8415d0` / `0x81dad0`,
`0x706f013f`). `tools/check-client-layout.py` verifies slot, RVA, the method
name each wrapper references through a RIP-relative LEA, and the function ID.

Wrapper ABI, identical in both builds apart from call targets and the known
signed-negative cleanup test: slots 5 and 214 take only `this` and read back a
4-byte result; slot 215 takes `this` and one byte (dl, serialized as 1 byte)
and reads back a 4-byte result. Neighbouring slots 216
`ValidateOfflineLogonTicket` and 217 `BGetOfflineLogonTicket` are not called:
Dock never handles the ticket itself.

## Pufflet fork — install spike (January build only)

The fork starts at upstream `72558e416a390f3669457930f09a389e54f49556`.
The official archive/hash above was freshly checked on 2026-10-16. RTTI finds
IClientAppManagerMap at the same RVA `0x12c0278`. Added zero-based slots:

| Slot | Method | January RVA | IPC function ID | Fencepost |
|---|---|---|---|---|
| 0 | InstallApp | `0x812430` | `0xdd9af165` | `0xdfbe9185` |
| 4 | GetAppInstallState | `0x73bae0` | `0xeaf26184` | `0xed0f1d88` |
| 20 | GetUpdateInfo | `0x777870` | `0x27b7a573` | `0x29e98bcd` |
| 76 | GetLibraryFolderPath | `0x75e300` | `0xf27c5d4c` | `0xf4d07931` |
| 77 | AddLibraryFolder | `0x706df0` | `0x16a14c1c` | `0x18f62504` |

Each wrapper references its own method name and `IClientAppManager` through
RIP-relative LEAs. `tools/check-client-layout.py` checks the slot, RVA, name
and function ID (and an UninstallApp negative control). The September adapter
has zero install addresses and refuses these calls. Launch support is unchanged.

Disassembly, through the wrapper's return:

- InstallApp: rcx this; edx AppID; r8d library index; r9b unscheduled. IPC sends
  4/4/1-byte arguments, returns a 4-byte EAppUpdateError (0 accepted). Dock
  passes false to request an immediate download. A zero response alone is
  insufficient: the wrapper also returns zero on IPC read failure.
- GetAppInstallState: rcx this, edx AppID; returns 4-byte state flags.
- GetUpdateInfo: rcx this, edx AppID, r8 pointer. The wrapper reads a bool and
  **exactly 0xf4 (244) bytes** into that pointer (`0x7779c7`); there is no size
  argument. Dock allocates exactly 244 bytes. The older OpenSteamworks record
  is **not** this layout: runtime decoding with its offsets 8/16 produced
  impossible counters and failed closed (65). Independent inspection of the
  server dispatch (`0x77de1a`), CUserAppManager slot 20 (`0x4b04a0`), and
  CAppUpdateDepotsJob's secondary interface (offset 456, slot 3 `0x5383f0`)
  establishes update flags at byte 4 and aggregate work bytes total/done at
  **60/68**. The aggregator `0x5ac960` adds these at `0x5ad3ad` (0x3c) and
  `0x5ad3a4` (0x44); the layout script checks those instructions. They are
  aggregate work counters across update phases, not compressed CDN bytes.
  Dock reads only these fields with memcpy and bounds checks, never raw data.
- GetLibraryFolderPath: rcx this, edx index, r8 output, r9d capacity. It sends
  index/capacity, reads a 4-byte count then a bounded string. Dock compares
  the returned path to the explicitly requested absolute library directory.
- AddLibraryFolder: rcx this, rdx NUL-terminated path. IPC serializes the
  string and returns a signed 4-byte library index; negative means failure.

The new ABI typedefs are independently written C, not copied declarations.
Immediate-download semantics were initially reference-led; the official client
independently confirms the wrapper names, sizes, argument registers and the
corrected aggregate work-counter offsets. Runtime evidence belongs in HANDOFF.md.


## Pufflet persistent session — queue control and verification

January only, under the same complete DLL SHA-256 gate. Added zero-based
IClientAppManagerMap slot 56 `ChangeAppDownloadQueuePlacement`, RVA `0x721b60`,
IPC function `0x9bd943a2`, fencepost `0x9e21ce58`. Independently inspected wrapper:
rcx=this, edx=u32 AppID, r8d=i32 placement; serializes 4/4 bytes and reads a
one-byte bool. CUserAppManager slot 56 (`0x4a9410`) forwards to `0x4b56d0`.
That implementation compares placement to 6 (paused) and 5 (last/resume);
`0x4b5d16` selects app-state bit `0x200` for placement 6 and removes the queue
record, without uninstall/deletion. Placement 5 appends it to the queue.
The older [OpenSteamworks enum](https://github.com/fire64/opensteamworks/blob/master/EAppDownloadQueuePlacement.h)
was a naming reference; the called wrapper and branches above are independent
exact-client evidence. Cancel uses pause-and-forget in Dock, never uninstall.

Slot 27 `StartValidatingApp`, RVA `0x855290`, IPC function `0x507803eb`,
fencepost `0x52a57c33`: rcx=this, edx=u32 AppID, serializes 4 bytes and returns
one-byte bool. CUserAppManager slot 27 (`0x4b76d0`) requires installed state,
sets validation request bytes at `0x4b781f`, marks dependent app requests, then
queues placement 1 at `0x4b78f1`. It asks Valve's client to verify/restore its
content; Dock neither hashes nor edits game files. A successful bool is only
request acceptance. The poller must observe work leave state 4 before trusting
API+ACF completion. Fast state transitions are sampled from the main pump,
separately from the one-second UI progress cadence.

`tools/check-client-layout.py` checks both added slots/RVAs, own-method names,
IPC IDs, a queue-method UninstallApp negative control, and the exact paused-bit
selection instructions. The September table has zero addresses and refuses
these calls. Live behavior and limitations belong in HANDOFF.md.


The persistent adapter also checks slot 40 `BIsDownloadingEnabled`, January RVA
`0x715520`, IPC function `0x2936b610`, fencepost `0x2b789bab`. The wrapper takes
only `this` and reads one bool. CUserAppManager's method at `0x4a6080` consults
its current download mode and gates; it is queried read-only, never changed by
Dock. A running game's policy can suspend queued updates without setting the
per-app paused bit. The UI therefore reports a pause while playing only when
a game is running, the user did not pause that job, and either the client's
download-enabled answer is false or its explicit paused bit is set. Static
slot/name/IPC checks and pure classification assertions cover this path.
