# AirPods Listening Controls

Listening controls are intentionally split into a driver-free application layer and a privileged
Windows transport. The application UI must not imply that a mode changed until the AirPods confirms
it.

## Current implementation status

- `ListeningModeController` owns capability, pending, confirmed, timeout, and failure state.
- Release builds use an unavailable transport and hide the controls.
- Debug builds use a simulated AirPods Pro 3 transport for UI development.
- `AapProtocol` encodes the documented handshake, feature-enable, notification-registration, and
  three noise-control packets. Its stream decoder accepts split and coalesced notifications.
- No production driver or broker is packaged. This is a release gate, not a runtime fallback.

## AirPods Pro 3 feasibility gate

Run the transport probe only on an isolated Windows hardware-test system. Record the AirPods model
and firmware, Windows build, Bluetooth adapter and driver, application commit, and probe version.
The evidence must demonstrate all of the following before a production transport is implemented:

1. Windows enumerates the Apple AAP service PDO identified by
   `{74ec2172-0bad-4d01-8f77-997b2be0722a}`.
2. L2CAP PSM `0x1001` accepts the handshake and notification request.
3. Transparency, Adaptive, and Noise Cancellation produce audible changes and matching confirmed
   notifications.
4. Stem-originated changes are reported.
5. Reconnect, sleep/resume, Bluetooth restart, and unpair/re-pair do not strand the channel.
6. The behavior passes on Windows 10 build 19041 or newer and supported Windows 11 builds.

The UARP service `{4715650b-5e9d-4ac2-b898-a4fc0aa5df78}` may be evaluated as a reconnect fallback,
but must not ship without recorded evidence that it is required.

## Production architecture gate

The approved design uses a KMDF profile driver owned exclusively by a minimally privileged broker
service. The Qt application communicates through a versioned, high-level named-pipe API containing
only `GetCapabilities`, `GetListeningMode`, `SetListeningMode`, and `SubscribeState`. Raw AAP packets
and arbitrary Bluetooth addresses are not part of the application IPC contract.

The driver interface must be limited to `SYSTEM` and the broker service SID, derive the remote
identity from its bound PDO, cap messages at 512 bytes, validate every buffer, allow one broker
owner, and cancel all I/O during cleanup or device removal.

Public packaging requires Microsoft HLK/WHCP certification. CI must reject unsigned or test-signed
drivers from production installers. Secure Boot remains enabled and the installer must never enable
Windows Test Mode.

## Release behavior

- Installed builds keep the controls hidden until compatible, Microsoft-signed driver and broker
  versions report ready.
- Driver or broker upgrade failure aborts the installed-edition upgrade and removes newly staged
  components.
- Portable builds never install or open the driver and always hide listening controls.
- The rest of AirPodsDesktop retains its Windows 10 build 17763 minimum; listening controls require
  build 19041 or newer.
