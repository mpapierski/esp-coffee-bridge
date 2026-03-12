# Nivona BLE Protocol Notes

This document summarizes the Nivona Android app protocol recovered from `de.nivona.mobileapp` 3.8.6.

Reverse-engineering artifacts from the APK now live under [`../.analysis/research/`](../.analysis/research/).

The real APK used here was [`../.analysis/research/downloads/de.nivona.mobileapp-3.8.6.apk`](../.analysis/research/downloads/de.nivona.mobileapp-3.8.6.apk), package `de.nivona.mobileapp`, version `3.8.6`.

ESP32 bridge firmware and webapp notes live in [BRIDGE.md](BRIDGE.md).

## Confidence

- High confidence:
  - BLE-only transport
  - Service/characteristic UUIDs
  - Device Information Service usage
  - High-level command names
  - Numeric/string/process payload layouts
  - Recipe selector IDs for the families listed below
  - Statistics register IDs for the families covered below
  - Brew payload layout
  - Customer-key bootstrap constants
- Medium confidence:
  - `HE`, `HN`, and `HS` semantics outside the traced call sites
  - Exact meaning of the anonymous `FrameDefinition` flags `a` / `b`
  - Per-model recipe selector mapping outside the families listed below
- Low confidence:
  - `HS`, `HA`, and `HI` beyond the limited call sites and flags documented here
  - Exhaustive writable/register coverage across all model families

## App Architecture

The Android Java/Kotlin output is only a Xamarin bridge. The real logic lives in managed assemblies:

- `Arendi.BleLibrary.dll`
- `Arendi.DotNETLibrary.dll`
- `Eugster.EFLibrary.dll`
- `EugsterMobileApp.Droid.dll`
- `EugsterMobileApp.dll`

## Transport

- Bluetooth mode: BLE/GATT only
- No RFCOMM or classic Bluetooth code was found
- App-facing transport classes:
  - `Eugster.EFLibrary.CoffeeMachine`
  - `Eugster.EFLibrary.Frame`
  - `Eugster.EFLibrary.FrameDefinitionManager`
  - `EugsterMobileApp.Droid.CoffeeMachineConnector.BleCoffeeMachineConnector`

## Services And Characteristics

### Standard Device Information Service

- Service: `180A`
- Characteristics:
  - `2A29` manufacturer name
  - `2A24` model number
  - `2A25` serial number
  - `2A27` hardware revision
  - `2A26` firmware revision
  - `2A28` software revision

### Nivona Custom Service

- Service: `0000AD00-B35C-11E4-9813-0002A5D5C51B`

Characteristics under `AD00`:

- `0000AD01-B35C-11E4-9813-0002A5D5C51B`
  - control-point write
- `0000AD02-B35C-11E4-9813-0002A5D5C51B`
  - notify stream / incoming frames
- `0000AD03-B35C-11E4-9813-0002A5D5C51B`
  - bulk TX / request data write
- `0000AD04-B35C-11E4-9813-0002A5D5C51B`
  - discovered by app, not used in traced logic
- `0000AD05-B35C-11E4-9813-0002A5D5C51B`
  - discovered by app, not used in traced logic
- `0000AD06-B35C-11E4-9813-0002A5D5C51B`
  - device-name read/write

## Discovery Filter

`CoffeeMachineManager<T>::CheckDiscovered` checks advertisement structure type `0x0D` and validates:

- `ReadUInt16LittleEndian(data, 1) == 29`
- `ReadUInt16LittleEndian(data, 2) == customerId`

The Android connector uses `customerId = 65535`.

## Customer-Key Bootstrap

Before normal app traffic, the Android connector derives and installs a customer key with:

- `salt = Eugster.EFLibrary.KeyManager::get_Salt()`
  - 16 random bytes from the EFLibrary runtime
- APK-private customer seed, 48 bytes:
  - `18ADA420647A8789FBEBFA89F166380B166512D84F2F67EF87A51BB685188B31D9ADECFF536359BA30657420E4FF5C9C`
- exact initialized AES key bytes used by `BleCoffeeMachineConnector::Encrypt(...)`, 32 bytes:
  - `834A3E980D65FACC5946B9442824072DB0D77DD0BE0983B3CAEE0AAF9472A924`
- note:
  - the `<PrivateImplementationDetails>` field name visible in IL is misleading here; the initialized bytes recovered via `RuntimeHelpers.InitializeArray(...)` are the `83 4A ...` value above

Derived key:

- AES-256-CBC
- PKCS7 padding
- `customer_key = BleCoffeeMachineConnector::Encrypt(seed48, salt16)`
- equivalently:
  - `customer_key = AES_CBC_Encrypt(seed48, android_aes_key32, iv=salt16)`
- output length:
  - `64 bytes`

Verified sample for the APK analyzed here:

- sample salt:
  - `AF10ED3DB6C80F66F9556EB2FEDF0896`
- derived customer key:
  - `A4916B3F75B384737785C839B192CED226FD0878F7E593548C327D64E39281DA66EBE1C69D49654E934EA5280821000E35D126437104E0E5DEAAF0E6F657A7C1`

`Eugster.EFLibrary.KeyManager::SetCustomerKey(customer_key64)` then feeds the EFLibrary internal key manager, which uses the same fixed AES-256-CBC key above together with the current salt to install the runtime state.

Recovered exact runtime working key used by the request serializer and encrypted receive parser:

- source in vendor code:
  - `a.by::a(a.be::a())` after `KeyManager.SetCustomerKey(...)`
- length:
  - `32`
- value for the APK analyzed here:
  - ASCII:
    - `NIV_060616_V10_1*9#3!4$6+4res-?3`
  - hex:
    - `4E49565F3036303631365F5631305F312A392333213424362B347265732D3F33`

This exact ASCII `NIV_...` value is the key used by the vendor RC4 request transform `a.w::a(...)`.
It is distinct from the 64-byte `customer_key` blob and from the two AES keys above.

## Session Setup

The library contains an application-layer communication-session task using command `HU`.
This task exists in `Eugster.EFLibrary` and is fully reversible offline.
No direct `HU` call site has been found in `EugsterMobileApp.Droid`, but `BleCoffeeMachine` subclasses `Eugster.EFLibrary.CoffeeMachine`, so the base-library session thread may still run indirectly when the selected peripheral is activated and reaches `Ready`.

Recovered facts:

- The app creates a `HU` request with a 6-byte payload.
- The request payload is `seed4 || verifier2`.
- The `HU` response payload is 8 bytes:
  - bytes `0..3`: random seed echo
  - bytes `4..5`: session key
  - bytes `6..7`: verifier
- Reflection-confirmed task construction:
  - public `new Frame("HU", payload)` is not what the session task uses; that public constructor yields `Frame.a = true`, `Frame.b = true`
  - the internal `a.r` task constructs `HU` with `Frame.a = false`, `Frame.b = true`
  - `a.r` expects `new FrameDefinition("HU", 8)`, i.e. `FrameDefinition.a = false`, `FrameDefinition.b = true`
- Failure strings in validation code:
  - `Random key mismatch`
  - `Session key verification failed`

Exact verifier function:

```text
derive_hu_verifier(buf, start, count):
  s = HU_TABLE[buf[start]]
  for i in range(start + 1, start + count):
    s = HU_TABLE[s ^ buf[i]]
  out0 = (s + 0x5D) & 0xFF

  s = HU_TABLE[(buf[start] + 1) & 0xFF]
  for i in range(start + 1, start + count):
    s = HU_TABLE[s ^ buf[i]]
  out1 = (s + 0xA7) & 0xFF

  return bytes([out0, out1])
```

Protocol:

```text
request_payload = seed4 || derive_hu_verifier(seed4, 0, 4)

response_payload must be:
  echoed_seed4 || session_key2 || derive_hu_verifier(response_payload, 0, 6)
```

On successful completion, the task stores the validated 2-byte `session_key2` into the `CoffeeMachine` instance for later request/response handling.

Minimal working live flow for the validated read path:

1. Connect on a bonded/encrypted BLE link.
2. Enable `AD02` notifications.
3. Send encrypted internal `HU` with no prepended session token.
4. Decode the 8-byte `HU` response payload as:
   - `seed4 || session_key2 || verifier2`
5. Store `session_key2`.
6. For post-`HU` reads (`HV`, `HL`, `HX`, `HR`):
   - prepend the 2-byte session key on send
   - encrypt the request body with the runtime RC4 key
7. For post-`HU` responses:
   - decrypt with the same runtime RC4 key
   - verify checksum against `command_ascii + body`
   - treat `body` directly as payload
   - do not strip or expect a 2-byte session echo in the response body

Request-side flag inference from `CoffeeMachine::TransmitFrame(frame)`:

- The exact anonymous `Frame` bool fields are still unnamed in metadata.
- But the transmit path now supports a strong behavioral inference:
  - one flag controls whether the frame is encrypted before send
  - one flag controls whether the current stored 2-byte `CoffeeMachine::a` session key is prepended automatically
- Evidence:
  - `TransmitFrame(frame)` calls the serializer as:
    - `a.bm::a(command, maybe_session_key, payload, encrypt_flag)`
  - for normal public `new Frame(command, payload)` requests:
    - the frame uses the current stored session key when available
    - and encrypts
  - for internal session task `HU`:
    - the frame omits the current session key
    - but still encrypts
- This is the cleanest explanation for the constructor split:
  - public `Frame("HV", payload)` is suitable for post-`HU` app traffic
  - internal `HU` uses a special ctor path so it stays encrypted but does not prepend any prior session

Example verified vector:

```text
seed    = FA 48 D1 7B
request = FA 48 D1 7B 7E 6E

session_key = 12 34
response    = FA 48 D1 7B 12 34 86 7F
```

Actual serialized `HU` request used by the task constructor:

```text
request_payload = FA 48 D1 7B 7E 6E
request_packet  = 53 48 55 E5 80 D0 36 79 81 BC 45
```

The raw helper can also emit the plain variant:

```text
53 48 55 FA 48 D1 7B 7E 6E E8 45
```

but the actual `a.r` session-setup task constructs the encrypted packet above.

`HU_TABLE`:

```text
62 06 55 96 24 17 70 A4 87 CF A9 05 1A 40 A5 DB
3D 14 44 59 82 3F 34 66 18 E5 84 F5 50 D8 C3 73
5A A8 9C CB B1 78 02 BE BC 07 64 B9 AE F3 A2 0A
ED 12 FD E1 08 D0 AC F4 FF 7E 65 4F 91 EB E4 79
7B FB 43 FA A1 00 6B 61 F1 6F B5 52 F9 21 45 37
3B 99 1D 09 D5 A7 54 5D 1E 2E 5E 4B 97 72 49 DE
C5 60 D2 2D 10 E3 F8 CA 33 98 FC 7D 51 CE D7 BA
27 9E B2 BB 83 88 01 31 32 11 8D 5B 2F 81 3C 63
9A 23 56 AB 69 22 26 C8 93 3A 4D 76 AD F6 4C FE
85 E8 C4 90 C6 7C 35 04 6C 4A DF EA 86 E6 9D 8B
BD CD C7 80 B0 13 D3 EC 7F C0 E7 46 E9 58 92 2C
B7 C9 16 53 0D D6 74 6D 9F 20 5F E2 8C DC 39 0C
DD 1F D1 B6 8F 5C 95 B8 94 3E 71 41 25 1B 6A A6
03 0E CC 48 15 29 38 42 1C C1 28 D9 19 36 B3 75
EE 57 F0 9B B4 AA F2 D4 BF A3 4E DA 89 C2 AF 6E
2B 77 E0 47 7A 8E 2A A0 68 30 F7 67 0F 0B 8A EF
```

## Library Connection Sequencing

Recovered sequencing in `Eugster.EFLibrary.CoffeeMachine`:

- Exact `Arendi.BleLibrary 3.4.0.600` peripheral states are:
  - `Idle = 0`
  - `EstablishLink = 1`
  - `DiscoveringServices = 2`
  - `Initialize = 3`
  - `Ready = 4`
  - `TearDownLink = 5`
  - `Update = 6`
- This older APK-shipped runtime has no separate `Negotiations` state.

- `Activate()` creates the frame parser, subscribes the local `FrameReceived` bridge, subscribes to peripheral state changes, and starts a background task-handler thread.
- The Arendi BLE stack drives `Initialize()` before the peripheral reaches `PeripheralState.Ready` (`4`).
- `Initialize()`:
  - looks up DIS `180A`
  - best-effort reads:
    - `2A29` manufacturer name
    - `2A24` model number
    - `2A25` serial number
    - `2A27` hardware revision
    - `2A26` firmware revision
    - `2A28` software revision
  - looks up custom service `0000AD00-B35C-11E4-9813-0002A5D5C51B`
  - binds custom characteristics in this exact order:
    - `AD01` control point
    - `AD03` write path
    - `AD02` notification path
    - `AD06`
    - `AD04`
    - `AD05`
  - required for initialization success:
    - `AD01`
    - `AD03`
    - `AD02`
    - `AD06`
  - optional in this path:
    - `AD04`
    - `AD05`
  - attaches the `NotificationReceived` handler to `AD02`
  - enables notifications on `AD02` via `ChangeNotificationAsync(enable=true, timeout=4000)`
- Notification enable retries up to 5 times, but only for:
  - `BleResult.Timeout`
  - `BleResult.InsufficientAuthentication`
- If notification setup still fails, initialization throws:
  - `Unable to setup notification. Possibly an old pairing on the central device?`
- No managed writes were found in `Initialize()` besides the `AD02` CCC/notification enable path.
- In particular, `Initialize()` does not:
  - send `HU`
  - write `AD01`
  - write `AD03`
  - write `AD04` / `AD05`
  - read `AD06`

Background task thread behavior:

- `CoffeeMachine::Run(task)` enqueues the task into the internal queue and signals event `j`.
- `CoffeeMachine::Dispose()` signals event `m`.
- `CoffeeMachine::d(object)` (timer callback) signals event `l`.
- `CoffeeMachine::c(...)` signals event `k` when peripheral state changes to `PeripheralState.Ready`.
- `CoffeeMachine+i::MoveNext` builds the worker wait array in this exact order:
  - index `0`
    - `m`
    - stop/dispose
  - index `1`
    - `k`
    - `Ready` session wake
  - index `2`
    - `l`
    - timer-driven refresh / retry wake
  - index `3`
    - `j`
    - queued-task wake
- So the worker waits on four events:
  - stop/dispose
  - `Ready`
  - timer-driven refresh / retry
  - queued task
- When peripheral state changes to `PeripheralState.Ready`, the state-change handler signals `k`.
- On the `Ready` wake, the worker immediately creates and executes `a.r` (`HU` session setup).
- If `a.r` succeeds, the worker stores the returned 2-byte session key into `CoffeeMachine::a`.
- If `a.r` fails, the worker logs the failure and issues `DisconnectAsync(3000)`.
- After successful `HU`, the worker immediately creates and executes `CoffeeMachineTaskSoftwareVersion` (`HV`).
- On the queued-task wake, the worker dequeues one `ICoffeeMachineTask` under a monitor lock and executes it.
- If a queued task throws, the worker stores that exception onto the task and then signals the task-completion event.
- There is no observed pre-`HU` `Hp` ping.
- There is no observed `AD01` control-point write before `HU`.
- There is no explicit delay inserted between notification enable and `HU`.
- `a.r` uses a 2500 ms request/response timeout.
- After successful `HU`, the validated 2-byte session key is stored on the `CoffeeMachine`.
- The app then immediately runs `CoffeeMachineTaskSoftwareVersion` (`HV`) before entering the normal queued-task loop.

Request/response task sequencing:

- `CoffeeMachineTask.RequestResponse(...)` installs the `FrameReceived` waiter before transmitting the request frame.
- So this library path is not relying on a post-write subscribe race to receive `HU` / `HV` / `HR` replies.

## Android Connector Behavior

Recovered app-layer behavior in `EugsterMobileApp.Droid.CoffeeMachineConnector.BleCoffeeMachineConnector`:

- Its constructor clears the global `FrameDefinitionManager` list and installs only:
  - `HS`, payload `10`
  - `HR`, payload `6`
  - `HA`, payload `66`
  - `HX`, payload `8`
  - `HL`, payload `20`
  - `HI`, payload `10`
- It also derives and installs the customer key via `KeyManager::SetCustomerKey(...)` before normal traffic.
- `WriteData(command, payload, timeout)` constructs a public `new Frame(command, payload)` and routes it through `BleCoffeeMachineTaskWriteData`.
- `ReadDataFromCoffeeMachine(frame)` routes through `BleCoffeeMachineTaskReadData`, which uses `CoffeeMachineTaskRequestResponse` with `ExpectedResponseFrames = null`.
- With `ExpectedResponseFrames = null`, the request/response task accepts the first parsed `FrameReceived` event and returns its payload; the Android connector call sites do not additionally verify the response command.
- Observed Android connector read call sites:
  - software version:
    - sends `HV` with empty payload
  - numeric reads:
    - sends `HR` with a 2-byte big-endian register id payload
  - process status:
    - sends `HX` with empty payload
- `BleCoffeeMachine::ReadDataFromCoffeeMachine(frame)` does not write GATT directly.
  - it creates `BleCoffeeMachineTaskReadData(frame)`
  - then calls base `CoffeeMachine::RunAsync(task)`
  - after that task completes, it returns `task.Response.Payload`
- `BleCoffeeMachine::WriteDataToCoffeeMachine(frame, timeout)` also does not write GATT directly.
  - it creates `BleCoffeeMachineTaskWriteData(frame, timeout)`
  - then calls base `CoffeeMachine::RunAsync(task)`
  - after completion it returns `true` only when `task.Response.Command == "A"`
- Exact Android task wrappers:
  - `BleCoffeeMachineTaskReadData(frame)`
    - extends `CoffeeMachineTaskRequestResponse`
    - ctor uses:
      - `request = frame`
      - `expectedResponseFrames = null`
      - `responseTimeout = 2500`
  - `BleCoffeeMachineTaskWriteData(frame, timeout)`
    - also extends `CoffeeMachineTaskRequestResponse`
    - ctor uses:
      - `request = frame`
      - `expectedResponseFrames = null`
      - `responseTimeout = timeout ?? 2500`
- No direct `HU` call site was found in `EugsterMobileApp.Droid` or `EugsterMobileApp`.
- `BleCoffeeMachine` instances are still `Eugster.EFLibrary.CoffeeMachine` subclasses, and the connector activates them through `CoffeeMachineManager`.
- `CoffeeMachineManager.set_Selected(...)` sets the selected peripheral's mode to the active value and demotes the others, so the inherited base-library connection/session logic can still run without an explicit Android call site.
- `CoffeeMachine` itself extends `Arendi.BleLibrary.Extention.EnhancedPeripheral`.
- `CoffeeMachine` construction immediately initializes its events/queues and starts the background task thread.
- `CoffeeMachineManager.Resume()` clears the suspended flag and sets the selected peripheral mode to value `1`.
- `CoffeeMachineManager.Suspend()` sets the selected peripheral mode back to value `0`.
- `CoffeeMachineManager.set_Selected(...)` also demotes non-selected peripherals to mode `0`, stores the new selected peripheral, and, when not suspended, sets that selected peripheral to mode `1`.
- In the inspected `Eugster.EFLibrary` IL, only mode values `0` and `1` are used.
- The oldest public NuGet surrogate (`Arendi.BleLibrary` `5.4.1`) names those enum values:
  - `0 = Inactive`
  - `1 = Active`
  - `2 = Manual`
- So the selected/non-selected mode mapping is very likely `Active` / `Inactive`, even though the exact APK-shipped Arendi assembly version is older.

Current implication:

- `a.r` / `HU` is confirmed as a real library session task.
- The shipped Android connector should not be modeled as “explicitly calls `HU` itself”.
- But it also should not be modeled as “definitely does not use `HU`”, because inherited base-library logic may still perform it when the selected `BleCoffeeMachine` becomes active and ready.
- Any live validation work should distinguish:
  - library session-task path (`a.r`, `HU`, stored 2-byte session key)
  - shipped Android connector path (public `Frame(...)` requests, custom parser list, first-frame read semantics)

Practical inference:

- The official app likely gets important runtime state not from storage, but from this inherited activation path:
  - create `BleCoffeeMachine`
  - mark it selected / active through `CoffeeMachineManager`
  - let the inherited `EnhancedPeripheral` / `CoffeeMachine` state machine drive connection, initialization, `Ready`, and any base-library session behavior
- Our direct raw BLE tests currently bypass that object-lifecycle path entirely.

### Task Queue / `RunAsync`

Recovered exact base-task behavior in `Eugster.EFLibrary.CoffeeMachine`:

- Every `CoffeeMachineTask` owns:
  - `Timeout`
  - `Done` (`AutoResetEvent`)
  - `Exception`
- `CoffeeMachine::Run(task)`:
  - resets `task.Done`
  - enqueues the task onto the internal `Queue<ICoffeeMachineTask>`
  - signals worker event `j`
- `CoffeeMachine::RunAsync(task)`:
  - calls `Run(task)`
  - then starts a background `Task.Run(...)` that blocks on `task.Done.WaitOne()`
  - so app callers await worker completion, not a direct BLE operation
- In the `CoffeeMachine` worker thread:
  - queued tasks are dequeued under monitor lock
  - `task.Execute(coffeeMachine)` is awaited
  - if it throws, `task.Exception` is set
  - in both success and failure paths, `task.Done.Set()` is signaled afterward

For request/response tasks specifically:

- `CoffeeMachineTaskRequestResponse.Execute(coffeeMachine)` delegates to the internal helper:
  - `RequestResponse(coffeeMachine, request, expectedResponseFrames, responseTimeout)`
- That helper:
  - installs the `FrameReceived` waiter first
  - transmits the request
  - waits for the matching `Frame`
  - stores that `Frame` into `task.Response`

Practical consequence:

- Android app `HR` / `HX` / `HL` / `HV` reads are not “raw write then wait.”
- They are:
  - queued onto the inherited `CoffeeMachine` worker
  - serialized behind the worker’s own `Ready -> HU -> HV` session path
  - then executed through `CoffeeMachineTaskRequestResponse`
- Our direct bridge probes still bypass this queue/worker path and write straight to `AD03`, which remains the strongest structural mismatch with the official app.

### App `Ready` Semantics

Recovered exact app-level readiness in `BleCoffeeMachineConnector`:

- `ICoffeeMachineConnector.Ready` is not a deep connector/session predicate.
- `BleCoffeeMachineConnector::get_Ready()` simply returns:
  - `false` when `SelectedCoffeeMachine == null`
  - otherwise `SelectedCoffeeMachine.Status == CoffeeMachineStatus.Ready`
- Exact app-level `CoffeeMachineStatus` enum values are:
  - `NotReady = 0`
  - `Connecting = 1`
  - `Ready = 2`
- `BleCoffeeMachineConnector::Peripheral_StateChanged(...)` maps underlying Arendi peripheral states to that app enum as:
  - `PeripheralState.Idle (0)` -> `NotReady`
  - `PeripheralState.Ready (4)` -> `Ready`
  - all other peripheral states -> `Connecting`
- When that handler transitions a machine into app `Ready`, it immediately calls:
  - `ICoffeeMachine.LoadCoffeeMachineSettings()`
- But `Model.CoffeeMachine.LoadCoffeeMachineSettings()` does not perform BLE traffic.
  - it only calls `SettingFactory::GetAvailableSettingsForCoffeeMachine(ExtensionMethods::ToCoffeeMachineModel(serial))`
  - then stores the resulting in-memory settings list on the app model object

Recovered exact serial-to-model decoder used by the app:

- `ExtensionMethods::ToCoffeeMachineModel(string serialNumber)` is the real app-side `model_from_serial`.
- It is not a full serial decoder.
  - it only inspects the serial prefix
  - first `serial[0:4]` for the special 8000-series values
  - otherwise `serial[0:3]`
  - all remaining serial characters are ignored
- If the string length is `< 3`, it returns `Unknown`.
- If the string length is `>= 4`, these 4-digit prefixes are checked first:
  - `8101` -> `NICR8101`
  - `8103` -> `NICR8103`
  - `8107` -> `NICR8107`
- Otherwise the first 3 digits are matched as:
  - `030` -> `NICR1030`
  - `040` -> `NICR1040`
  - `660` -> `Eugster660`
  - `670` -> `Eugster670`
  - `675` -> `Eugster675`
  - `680` -> `Eugster680`
  - `756` -> `Eugster756`
  - `758` -> `Eugster758`
  - `759` -> `Eugster759`
  - `768` -> `Eugster768`
  - `769` -> `Eugster769`
  - `778` -> `Eugster778`
  - `779` -> `Eugster779`
  - `788` -> `Eugster788`
  - `789` -> `Eugster789`
  - `790` -> `Eugster790`
  - `791` -> `Eugster791`
  - `792` -> `Eugster792`
  - `793` -> `Eugster793`
  - `794` -> `Eugster794`
  - `795` -> `Eugster795`
  - `796` -> `Eugster796`
  - `797` -> `Eugster797`
  - `799` -> `Eugster799`
  - `920` -> `NICR920`
  - `930` -> `NICR930`
  - `960` -> `Eugster960`
  - `965` -> `Eugster965`
  - `970` -> `Eugster970`
  - anything else -> `Unknown`

Implications for our reverse engineering:

- The official app does not derive model metadata from the tail of the serial number.
- The official app does not search for model tokens anywhere within the serial.
  - prefix match only
- The app-specific family mapping is therefore deterministic from those leading digits:
  - `660/670/675/680` -> `Eugster600`
  - `756/758/759/768/769/778/779/788/789` -> `Eugster700`
  - `790/791/792/793/794/795/796/797/799` -> `Eugster79X`
  - `920/930` -> `Eugster900`
  - `960/965` -> `Eugster900Light`
  - `030/040` -> `Eugster1000`
  - `8101/8103/8107` -> `NIVO8000`

Implications:

- The app statistics/settings services gate only on a selected machine whose app-level status enum is `Ready`.
- That app `Ready` state is derived directly from underlying `PeripheralState.Ready`, not from any stronger “session established” condition.
- So app code does not appear to wait for successful `HU`, `HV`, or any first accepted `HR`/`HX`/`HL` response before considering the connector `Ready`.
- The settings-load side effect attached to `Ready` is model metadata only; it is not a hidden transport/session priming write.
- But Android connector `Read...` / `Write...` calls still do not talk to GATT directly.
  - `BleCoffeeMachine::ReadDataFromCoffeeMachine(frame)` creates `BleCoffeeMachineTaskReadData` and calls base `CoffeeMachine::RunAsync(task)`.
  - `BleCoffeeMachine::WriteDataToCoffeeMachine(frame, timeout)` creates `BleCoffeeMachineTaskWriteData` and also calls base `CoffeeMachine::RunAsync(task)`.
  - so app-issued `HR` / `HX` / `HL` / `HV` requests are queued onto the same `CoffeeMachine` worker thread that, on the `Ready` wake, first runs internal `HU` and then internal `HV` before draining queued tasks

Practical consequence:

- Even though app `connector.Ready` is only `PeripheralState.Ready`, app `HR` / `HX` / `HL` traffic is still serialized behind the inherited `CoffeeMachine` worker/session machinery.
- Our bridge raw tests bypass that queue/worker path and write directly to `AD03`, which remains a strong candidate explanation for why all reproduced `H*` requests are still ignored live.

## Persisted State

Recovered persistence behavior:

- `Eugster.EFLibrary.CoffeeMachineManagerStorageProvider` is just a file-backed string store.
- `CoffeeMachineManager<T>` serializes only a single string field in its configuration DTO:
  - the selected peripheral UUID
- On restore, the manager only recreates and re-selects the previously selected peripheral by UUID.
- The 2-byte `CoffeeMachine::a` session token is not serialized there.
- The only confirmed write to `CoffeeMachine::set_a(unsigned int8[])` is the successful `a.r` session task storing the validated 2-byte value in memory.

Android connector persistence adds one more file:

- `autoconnect.json`
  - JSON array of serial-number strings
  - used only as a remembered list for auto-selecting a discovered machine whose serial matches

So current reverse-engineering evidence says:

- there is no persisted session-token restore path in the library manager storage
- there is no app-side persisted session-token restore path in `autoconnect.json`
- if the official app reaches working `HV` / `HR` / `HX` traffic, it is doing so through live runtime state, not by loading a cached 2-byte session token from storage

## Public NuGet Cross-Check

Recovered exact APK assembly versions from the extracted managed DLLs:

- `Arendi.BleLibrary`
  - `3.4.0.600`
- `Arendi.DotNETLibrary`
  - `2.2.0.53`

Public NuGet packages do not expose this exact version line:

- `Arendi.BleLibrary` / `Arendi.BleLibrary.Android` on nuget.org start at package version `5.4.1`
- inspected public packages carry assembly versions like:
  - `5.4.1.1366`
  - `11.2.0.6`
  - `13.3.0.109`
- so the APK does not appear to embed a byte-for-byte public NuGet assembly from the currently published package line

Best available public surrogate:

- the oldest public package line (`5.4.1`) is loadable and exposes:
  - `PeripheralMode`
    - `Inactive`
    - `Active`
    - `Manual`
  - `PeripheralState`
    - `Idle`
    - `EstablishLink`
    - `DiscoveringServices`
    - `Negotiations`
    - `Initialize`
    - `Ready`
    - `TearDownLink`
    - `Update`
  - `EnhancedPeripheral` methods including:
    - `set_Mode`
    - `Establish` / `EstablishAsync`
    - `Teardown` / `TeardownAsync`
    - `Update` / `UpdateAsync`
    - `Initialize` / `InitializeAsync`
    - `Activate`
    - `CheckReady`

Use this public package only as a semantic cross-check.
The APK-shipped DLLs and their IL remain the authoritative source for behavior and exact numeric mappings.

Useful semantic cross-checks from the public `5.4.1` XML docs:

- `PeripheralMode.Inactive`
  - peripheral remains disconnected
- `PeripheralMode.Active`
  - peripheral should be connected whenever possible
  - after disconnect, reconnect is attempted automatically
- `PeripheralMode.Manual`
  - connect only on request
  - no automatic connect or disconnect
- `EnhancedPeripheral.Initialize` / `InitializeAsync`
  - runs in `PeripheralState.Initialize`
  - after service discovery
  - intended for derived classes to check services and read/write initial values
- `EnhancedPeripheral.CheckReady`
  - throws unless the peripheral is already in `Ready`
- `EnhancedPeripheral.Activate`
  - is called after the standard peripheral object is assigned
  - for non-virtual peripherals this happens during construction

This lines up with the APK observations:

- `CoffeeMachine` is a derived `EnhancedPeripheral`
- its constructors call the base constructor and then immediately initialize their own session/task machinery
- manager-selected peripherals are driven into mode value `1`, which is very likely `Active`
- so the official app’s selected-machine path likely receives automatic establish / initialize / ready behavior from the inherited runtime, not from any stored protocol token
- In the exact APK-shipped `Arendi.BleLibrary 3.4.0.600` public API:
  - `IEnhancedPeripheral` exposes:
    - `Establish()` / `EstablishAsync(int timeout)`
    - `Teardown()` / `TeardownAsync(int timeout)`
    - `Update(...)`
    - `ConnectTimeout`
    - `DisconnectTimeout`
    - `DiscoverServicesTimeout`
    - `ReconnectTimeout`
    - `RssiInterval`
    - `Mode`
    - `State`
- In the exact APK-shipped `EnhancedPeripheral` implementation:
  - `Activate()` in the base class is a no-op
  - `Establish()` throws if:
    - no underlying `IPeripheral` is assigned
    - or mode is `Inactive`
  - otherwise `Establish()` signals an internal `AutoResetEvent`
  - `Teardown()` also rejects `Inactive` mode and otherwise signals another internal `AutoResetEvent`
  - `set_Mode(...)` stores the new mode and, when a real peripheral exists, hands control to an internal scheduler method
  - exact field decoding from the APK copy shows:
    - underlying `IPeripheral` field
    - mode field of type `PeripheralMode`
    - state field of type `PeripheralState`
    - multiple `AutoResetEvent` fields
    - multiple `Timer` fields

Practical meaning:

- The inherited runtime is not just “connected or not”; it is an event/timer-driven worker around establish / teardown / update transitions.
- The official app path uses that worker indirectly through selected-machine activation.
- Our bridge raw tests currently bypass that worker entirely and talk straight to GATT characteristics.

More exact `Arendi.BleLibrary 3.4.0.600` worker details:

- `Establish()`:
  - rejects:
    - missing underlying `IPeripheral`
    - mode `Inactive`
  - otherwise signals an internal `AutoResetEvent`
- `Teardown()`:
  - also rejects:
    - missing underlying `IPeripheral`
    - mode `Inactive`
  - otherwise signals a different internal `AutoResetEvent`
- `set_Mode(...)`:
  - stores the new `PeripheralMode`
  - if a real `IPeripheral` exists, immediately invokes an internal scheduler method
- exact field decoding from the APK copy shows that scheduler coordinates at least:
  - one `AutoResetEvent` used by `Establish()`
  - one `AutoResetEvent` used by `Teardown()`
  - one additional `AutoResetEvent` used by the periodic RSSI path
  - one `Timer` used for reconnect scheduling
  - one `Timer` used for periodic RSSI scheduling
- the reconnect timer callback signals the same internal establish event used by `Establish()`
- the RSSI timer callback signals the RSSI worker event

More concrete exact mappings from the APK copy:

- `Establish()` signals field `d` (`AutoResetEvent`)
- `Teardown()` signals field `f` (`AutoResetEvent`)
- reconnect timer callback method `e(object)` also signals field `d`
  - so reconnect feeds the same establish path as a manual `Establish()`
- periodic RSSI timer callback method `f(object)` signals field `e` (`AutoResetEvent`)
- method `n()` creates the reconnect timer:
  - due time = `ReconnectTimeout`
  - period = `-1` (one-shot)
- method `q()` creates the RSSI timer:
  - due time = `RssiInterval`
  - period = `RssiInterval` (repeating)

So `Mode = Active` is not just a passive marker.
It enables an internal scheduler that can automatically:

- initiate establish work
- initiate teardown work
- schedule reconnect attempts
- schedule periodic RSSI work

That is the clearest remaining structural difference between the official app path and our direct raw BLE tooling.

What I did not find in `Eugster.EFLibrary` or `EugsterMobileApp.Droid`:

- no app-side override of:
  - `EstablishServiceDiscovery`
  - `EstablishParameter`
  - `EstablishDataLength`
  - `EstablishPhy`
  - `EstablishAttMtu`
- the only explicit inherited-runtime tweak I found in `CoffeeMachine` setup is:
  - `set_RssiInterval(0)`

So current evidence does not support a theory that the official app depends on app-specific negotiated ATT MTU / PHY / data-length values set in managed code.
If those negotiations matter, they are likely just the inherited library defaults rather than Nivona-specific overrides.

## Exact Android Write Path

Recovered Android adapter write behavior from the exact APK-shipped `Arendi.BleLibrary 3.4.0.600`:

- generic async write entry:
  - `a.by<T>::WriteDataAsync`
  - token `0x06000407`
  - RVA `0x184fc`
- Android concrete characteristic type:
  - `a.aj`
  - wraps `Android.Bluetooth.BluetoothGattCharacteristic`
- Android concrete write primitive:
  - `a.aj::j(byte[], int)`
  - token `0x06000107`
  - RVA `0x80c4`

Exact behavior:

- write type is chosen once during Android characteristic construction in `a.aj::.ctor`
  - if characteristic property includes `WriteWithoutResponse (0x04)`:
    - `BluetoothGattCharacteristic.WriteType = NoResponse`
  - otherwise:
    - `BluetoothGattCharacteristic.WriteType = Default`
- I found no later dynamic `set_WriteType` call
- write eligibility is property-based, not permission-based
  - the path checks `Property & (WriteWithoutResponse | Write)` i.e. `0x0c`
  - cached `Permission` is not consulted on the write path
- Android adapter write execution is single-shot
  - `SetValue(data)` once
  - `BluetoothGatt.WriteCharacteristic(...)` once
  - no chunking
  - no long-write / reliable-write path in this flow
- adapter execution is serialized through the `a.bw` operation queue
  - one BLE operation at a time
- timeout behavior:
  - adapter operation timer:
    - 4000 ms
  - async wrapper wait for `DataWritten`:
    - 4000 ms

Implication:

- the official Android client does not appear to split Nivona request frames at the Android adapter layer
- the only write-type variation is `NoResponse` vs default, chosen from characteristic properties at wrapper construction time
- since the live machine reports `AD03 canWriteNoResponse = false`, this write-type branch is not the cause of the silent `HV` / `HR` / `HX` requests on our unit

## Request Packet Format

The vendor request serializer is `a.bm::a(command, session_key, payload, encrypted)`.
Its framing is confirmed:

```text
0x53 || command_ascii || body_or_ciphertext || tail_crc_or_cipher_crc || 0x45
```

Confirmed request rules:

- start byte:
  - `0x53`
- end byte:
  - `0x45`
- command:
  - 2 ASCII bytes
- plain request:
  - `body = session_key || payload` when `session_key` is supplied
  - otherwise `body = payload`
  - `crc = (~sum(command_ascii + body)) & 0xFF`
- encrypted request:
  - `plain = body || crc`
  - `cipher = RC4(working_key, plain)`
  - transmitted packet is `0x53 || command_ascii || cipher || 0x45`

The request transform is standard RC4 with the 32-byte working key above.

Verified request-side transform vectors:

```text
RC4(NIV_060616_V10_1*9#3!4$6+4res-?3, 00 00 47)                   = 1F C8 46
RC4(NIV_060616_V10_1*9#3!4$6+4res-?3, FA 48 D1 7B 7E 6E E8)       = E5 80 D0 36 79 81 BC
RC4(NIV_060616_V10_1*9#3!4$6+4res-?3, 12 34 1B)                   = 0D FC 1A
RC4(NIV_060616_V10_1*9#3!4$6+4res-?3, 12 34 00 D5 00 00 04 D2 74) = 0D FC 01 98 07 EF 50 90 BE
RC4(NIV_060616_V10_1*9#3!4$6+4res-?3, 00 D5 00 00 04 D2 BA)       = 1F 1D 01 4D 03 3D EE
```

Confirmed serialized request vectors:

```text
Hp plain request:
  payload = 00 00
  packet  = 53 48 70 00 00 47 45

HU encrypted request:
  payload = FA 48 D1 7B 7E 6E
  packet  = 53 48 55 E5 80 D0 36 79 81 BC 45

HU plain helper variant:
  payload = FA 48 D1 7B 7E 6E
  packet  = 53 48 55 FA 48 D1 7B 7E 6E E8 45

HV encrypted request with session 12 34:
  plain   = 12 34 1B
  packet  = 53 48 56 0D FC 1A 45

HR encrypted request with session 12 34 and payload 00 D5 00 00 04 D2:
  plain   = 12 34 00 D5 00 00 04 D2 74
  packet  = 53 48 52 0D FC 01 98 07 EF 50 90 BE 45

HR encrypted request without session and payload 00 D5 00 00 04 D2:
  plain   = 00 D5 00 00 04 D2 BA
  packet  = 53 48 52 1F 1D 01 4D 03 3D EE 45
```

## Frame Definitions

Built-in library frame definitions:

- `A`, payload `0`
- `N`, payload `0`
- `HU`, payload `8`
- `HV`, payload `11`
- `Hp`, payload `24`

Android connector adds:

- `HS`, payload `10`
- `HR`, payload `6`
- `HA`, payload `66`
- `HX`, payload `8`
- `HL`, payload `20`
- `HI`, payload `10`

Constructor semantics recovered by reflection:

- the internal bool fields are anonymous (`a`, `b`) in the APK; the shipped delegates and parser/send paths now pin down their behavior
- `new Frame(command, payload)` yields:
  - `a = true`
  - `b = true`
- `new FrameDefinition(command, payload)` yields:
  - `a = false`
  - `b = true`
- built-in `A`, `N`, and `Hp` are:
  - `a = false`
  - `b = false`
- built-in `HU` and `HV`, and all Android-added response defs above, are:
  - `a = false`
  - `b = true`

Exact helper mappings from the shipped DLL by reflection:

- `Frame.a`
  - `Eugster.EFLibrary.Frame::get_a`
  - on send, this controls whether the current stored 2-byte `CoffeeMachine::a` token is prepended
- `Frame.b`
  - `Eugster.EFLibrary.Frame::get_b`
  - on send, this is the encrypt flag
- `FrameDefinition.a`
  - `Eugster.EFLibrary.FrameDefinition::get_a`
  - on receive, this enables a special extra-2-byte branch immediately after the command
- `FrameDefinition.b`
  - `Eugster.EFLibrary.FrameDefinition::get_b`
  - on receive, this enables in-place RC4 decode of the bytes after the command before checksum verification

The receive parser:

- is `a.x`
- uses exact constants from the shipped runtime:
  - start byte `0x53`
  - end byte `0x45`
  - max buffered bytes `128`
  - inter-byte timeout `1000 ms`
- buffers bytes from the start byte up to but not including the terminating `0x45`
- matches commands as either:
  - 1 ASCII byte at offset `1` for `A` / `N`
  - or 2 ASCII bytes at offsets `1..2` for `H*`
- expects a fixed stored length:
  - `1 + command_len + (FrameDefinition.a ? 2 : 0) + payload_len + 1`
  - the final `+1` is the checksum byte
- validates checksum as:
  - `(~sum(buffer[1 .. checksum-1])) & 0xFF == checksum`
- resets and logs on:
  - inter-byte timeout
  - buffer overflow
  - checksum failure
- raises `FrameReceived` only after building a `Frame(command, payload, def.a, def.b)` and then resets the parser state

What is confirmed offline:

- request framing uses `0x53 ... 0x45`
- response frame definitions above are the active ones used by the Android connector
- the generic Android write path treats response command `A` as write success
- the parser does accept request-shaped `0x53 || cmd || body || crc || 0x45` envelopes when the definition flags match the bytes on the wire
- the active Android-added definitions are all public `new FrameDefinition(...)`, so they all require `FrameDefinition.b = true`, i.e. encrypted receive-side decoding

Useful exact-runtime parser probes:

- accepted by the exact parser when the definition is internal plain `FrameDefinition("HR", 6, false, false)`:
  - `53 48 52 00 D5 00 00 04 D2 BA 45`
- rejected by the exact parser when the definition is the public Android-style `new FrameDefinition("HR", 6)`:
  - the same bytes above
  - because the public definition has `b = true` and the parser enters the encrypted receive path before checksum validation
- accepted by the exact parser with a matching plain definition:
  - `53 5A 31 AA BB CC DD 66 45`
  - emitted as `Frame("Z1", payload=AA BB CC DD, a=false, b=false)`
- accepted by the exact parser with the real Android-style encrypted definition:
  - `53 48 52 1F 1D 01 4D 03 3D EE 45`
  - emitted as `Frame("HR", payload=00 D5 00 00 04 D2, a=false, b=true)`
- rejected by the exact parser when the encrypted `HR` frame includes a prepended session token:
  - `53 48 52 0D FC 01 98 07 EF 50 90 BE 45`
  - this shows the active Android-added receive path is encrypted but not session-prefixed

Live validation against the paired machine now confirms the same rule:

- post-`HU` requests are encrypted and do prepend the current 2-byte session token on send
- post-`HU` responses are encrypted but do not echo that 2-byte session token in the response body
- so the practical decode rule for the validated Android-style commands is:
  - decrypt bytes `packet[3:-1]` with the runtime RC4 key
  - split `body || crc`
  - verify `crc == (~sum(command_ascii + body)) & 0xFF`
  - treat `body` directly as the command payload

What remains unresolved in `a.x`:

- the exact intended semantics of the unused `FrameDefinition.a = true` branch
  - it adds two required bytes immediately after the command
  - it compares them against the parser’s current 2-byte token in reversed index order
  - but no shipped active definition in the APK exercises that path

So the request serializer is fully reproduced, and the `a.r` task-level `HU` request/response payload validation is fully reproduced. For the validated Android connector commands `HU`, `HV`, `HL`, `HX`, and `HR`, the encrypted receive-side bytes and session behavior are now confirmed live. What remains unresolved is mainly the unused parser branch above and the still-unvalidated command families `HS`, `HA`, and `HI`.

The library checksum is:

- `crc = (~sum(frame_bytes)) & 0xFF`

## High-Level Commands

### Confirmed

- `Hp`
  - ping
- `HU`
  - session setup
- `HV`
  - read software version
- `HL`
  - read serial number
- `HR`
  - read numeric value
- `HA`
  - read string value
- `HW`
  - write numeric value
- `HB`
  - write string value
- `HX`
  - read process status
- `HD`
  - reset/set default numeric value
- `HY`
  - confirm user input on host side
- `HZ`
  - cancel process
- `HE`
  - start/trigger machine action with structured payload

### Acknowledgement / Capability

- `A`
  - ACK frame, treated as write success
- `N`
  - negative/error frame
- `HS`
  - registered 10-byte parser frame in the Android connector; no direct send/use site is confirmed yet
- `HI`
  - feature/capability read used by `IsFeatureSupported`
  - current app only checks `response[0] & 0x01` for `ImageTransfer`

## Payload Layouts

### `HR` read numeric

Request payload:

- 2-byte big-endian register ID

Response payload:

- 6 bytes total:
  - echoed 2-byte big-endian register ID
  - 4-byte big-endian signed integer

Setting-value decode rule recovered from the Android app:

- `ReadSettingAsync` reads the full 32-bit signed result, then converts it back to bytes and keeps the low 16 bits as the selected UI value code
- in practice, settings labels map to the low 16-bit big-endian code
  - example:
    - raw `Int32 = 4`
    - low 16-bit code = `00 04`
    - UI label can then resolve to `4 h` for the appropriate auto-off register
- normal counters and percentage registers still use the full 32-bit integer directly

### `HW` write numeric

Request payload:

- 2-byte big-endian register ID
- 4-byte big-endian signed integer value

### `HA` read string

Request payload:

- 2-byte big-endian register ID

Response payload:

- 66 bytes total:
  - echoed 2-byte big-endian register ID
  - 64-byte string field
- Newer model families `1`, `4`, `6`, `7`:
  - UTF-16LE string after skipping the first 2 bytes
- Older families:
  - legacy 1-byte string mode after skipping the first 2 bytes
  - app-side decode behavior:
    - cast each byte directly to a 1-byte character
    - keep only `char.IsLetter(...) || char.IsDigit(...)`
    - practical effect:
      - spaces and punctuation are stripped from the displayed name
      - example live payload bytes `MOJA KAWA` display as `MOJAKAWA`

### `HB` write string

Request payload:

- 2-byte big-endian register ID
- 64-byte string field

Encodings:

- Newer model families `1`, `4`, `6`, `7`:
  - UTF-16LE bytes, truncated/padded to 64 bytes
- Older families:
  - legacy 1-byte encoding, truncated/padded to 64 bytes
  - app-side encode behavior:
    - drop every non-letter/non-digit before sending
    - encode each remaining character as a single byte when `<= 0xFF`
    - characters above `0xFF` fall back to `?`

### `HX` read process status

Request payload:

- empty

Response payload:

- 8 bytes total
- 4 big-endian `Int16` values:
  - `process`
  - `sub_process`
  - `message`
  - `progress`

APK-backed interpretation from the Android app:

- `process` is the coarse machine state used by the app when deciding whether it may start a drink
- `message` is an overlay for blocking errors or operator prompts
- `sub_process` and `progress` are parsed, but are not meaningfully surfaced by the Android UI in the artifacts inspected so far
- confirmed process values used by the app:
  - `8` = ready on the `700`-style path used by the live `756`
  - `11` = drink preparation already in progress on the `700`-style path
  - `3` = ready on the alternate family path
  - `4` = drink preparation already in progress on the alternate family path
- confirmed numeric message values productized through the localization layer:
  - the app's `LocalizationService::IsCoffeemachineErrorMessageAvailable(error)` is literally `error <= 6`
  - so only codes `0..6` are treated as numeric localized machine errors
  - confirmed values:
    - `0` = generic fallback "machine not ready" string in the app
    - `1` = brewing unit removed
    - `2` = trays missing
    - `3` = empty trays
    - `4` = fill up water
    - `5` = close powder shaft
    - `6` = fill coffee beans
- confirmed message values handled as explicit brew-flow special cases, outside the numeric localization table:
  - `11` = move cup to frother and open valve
  - `20` = flush required
- important nuance:
  - locale key `App_CoffeemachineMessage_0 = Machine not ready` exists in the APK
  - but the app also uses key `0` as a generic fallback error string when no better mapping is available
  - so live wire value `message = 0` should be treated as “no special message” in a ready state, not automatically as “machine not ready”
- startup and brew-loop behavior in `CustomizeCoffeeActivity::MakeCoffee()`:
  - before starting a drink, the app reads `HX`
  - if `process != prepareResultCode` after `HE`, the app checks `IsCoffeemachineErrorMessageAvailable(message)`
  - when that returns false, it falls back to `App_CoffeemachineMessage_0`
  - therefore unknown numeric message codes are not productized into their own text by the APK
- observed unknown raw message:
  - live bridge testing on the `756` saw `message = 42` after canceling a brew
  - there is no `App_CoffeemachineMessage_42` locale key in any shipped locale dump
  - because the app only treats `0..6` as localized numeric errors, `42` would fall back to the generic “machine not ready” text on Android
  - current status: `42` remains an unknown raw machine code, plausibly cancel or abort related, but not identified by the APK itself

### `HY` confirm user input

Request payload:

- 4 zero bytes

### `HZ` cancel process

Request payload:

- 4 zero bytes

### `HD` set default numeric value

Request payload:

- 2-byte big-endian register ID

### `HI` feature / capability read

Request payload:

- empty

Response payload:

- 10 bytes total
- current app only checks:
  - `response[0] & 0x01`
  - bit `0x01` means `ImageTransfer`

### `HE` brew / machine action

The traced normal brew path uses:

- command: `HE`
- payload length: `18`

Observed payload layout for the standard `MakeCoffee` path:

- `payload[1] = 0x04` if model family is `1` (`NIVO8000` path in this APK)
- `payload[1] = 0x0B` otherwise
- `payload[3] = job_product_parameter & 0xFF`
- `payload[5] = 0x01`
- all other bytes zero
- the optional `noOfCups` argument is ignored by the APK path

Important managed-app behavior:

- the Android app does not normally jump straight to `HE`
- `CoffeeMachineService::MakeCoffee(...)` first calls a fallback path for chilled `8000` recipes
- if that fallback does not apply, it calls `RecipeService::SendTemporaryRecipe(recipe)` and only then sends the standard `HE` payload
- `SendTemporaryRecipe(...)`:
  - clones the selected recipe object
  - rebases every recipe-item command id into the machine's temporary-recipe address space using `SelectedCoffeeMachine.CommandIdTemporaryRecipeType - recipe.RecipeBaseAddress`
  - `SelectedCoffeeMachine.CommandIdTemporaryRecipeType` is initialized to `9001` in the managed app's `CoffeeMachine` constructor
  - for `MyCoffee`, subtracts an additional `3` from that delta
  - appends a temporary `RecipeItemLookUp` whose selected value is the original `job_product_parameter`
  - calls `SendRecipeToCoffeeMachine(...)`
- `SendRecipeToCoffeeMachine(...)` then writes the recipe items one by one:
  - lookup items
  - fluid items
  - name items
  - enabled flag
  - type
  - icon
- practical implication:
  - app-started standard drinks use the current recipe-item values from the app model, not just the bare selector in `HE`
  - drink properties like strength, volume, and temperature are therefore expected to come from the temporary-recipe upload, not from the `HE` payload itself
  - inferred model:
    - this is a transient scratch recipe namespace on the machine, not a saved `MyCoffee` slot
    - the app rewrites it on each brew request and then starts the drink with `HE`
    - it is separate from persistent `MyCoffee` storage at `20000+`

Special chilled fallback path:

- only used for chilled standard recipes on `NIVO8000`
- gated by software version `1040A015G15`
- still sends `HE`
- `payload[1] = 0x04`
- `payload[3] = job_product_parameter & 0xFF`
- `payload[5]` remains `0x00`
- skips the temporary-recipe upload step

The app also uses `HE` for settings-level factory resets.

Recovered global reset payloads:

- `Factory settings: Settings`
  - command id `0x0032`
  - wire payload:
    - `HE 00 32 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00`
- `Factory settings: Recipes`
  - command id `0x0033`
  - wire payload:
    - `HE 00 33 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00`
- payload shape:
  - fixed 18-byte `HE` body
  - first 2 bytes are the big-endian command id
  - remaining 16 bytes are `0x00`
- recovered from app settings tables for:
  - `600`
  - `700`
  - `1030`

Individual recipe “reset to default settings” is a separate path:

- Android `SetDefaultNumericValue(itemId)` sends:
  - `HD id_hi id_lo`
- this resets a single recipe item by its item command id
- a universal family-by-family item id table is still not fully recovered here

## Standard Recipe Selector IDs

Recovered from `RecipeFactory`.

### Family 600

- `0` espresso
- `1` coffee
- `2` americano
- `3` cappuccino
- `4` frothy milk
- `5` hot water

### Family 700

- `0` espresso
- `1` cream
- `2` lungo
- `3` americano
- `4` cappuccino
- `5` latte macchiato
- `6` milk
- `7` hot water

### Family 79x

- `0` espresso
- `1` coffee
- `2` americano
- `3` cappuccino
- `5` latte macchiato
- `6` milk
- `7` hot water

### Family 900

- `0` espresso
- `1` coffee
- `2` americano
- `3` cappuccino
- `4` caffe latte
- `5` latte macchiato
- `6` hot milk
- `7` hot water

### Family 1030

- `0` espresso
- `1` coffee
- `2` americano
- `3` cappuccino
- `4` caffe latte
- `5` latte macchiato
- `6` hot water
- `7` warm milk
- `8` hot milk
- `9` frothy milk

### Family 1040

- `0` espresso
- `1` coffee
- `2` americano
- `3` cappuccino
- `4` caffe latte
- `5` latte macchiato
- `6` hot water
- `7` warm milk
- `8` frothy milk

### Family 8000

- `0` espresso
- `1` coffee
- `2` americano
- `3` cappuccino
- `4` caffe latte
- `5` latte macchiato
- `6` milk
- `7` hot water

### Chilled add-ons

- only on chilled `NIVO8000` (`NICR8107`)
- `8` chilled espresso
- `9` chilled lungo
- `10` chilled americano

## Standard Recipe Register Model

The managed app does not treat standard drinks as selector-only actions. Standard recipe content is queryable and forms a family-specific register table.

Confirmed app behavior:

- `RecipeService::GetRecipeItems(...)` builds item models for standard drinks as well as `MyCoffee`
- `RecipeService::LoadRecipeWithRecipeItems(...)` reads numeric recipe-item values from the machine for those standard recipes
- for standard drinks, the base register is:
  - `B = 10000 + selector * 100`
- item values are then read with `HR (B + offset)`

That means the current machine-side standard recipe for a drink like `lungo` on family `700` is readable live, not just inferable from the app defaults.

### Family 600 standard recipe offsets

- `+1` strength
- `+2` profile
- `+3` temperature
- `+4` two cups
- `+5` coffee ml
- `+6` water ml
- `+8` frothy milk ml
- `+9` preparation

### Family 700 / 79x / 8000 standard recipe offsets

- `+1` strength
- `+2` profile
- `+3` temperature
- `+4` two cups
- `+5` coffee ml
- `+6` water ml
- `+7` milk ml
- `+8` frothy milk ml

Example:

- family `700` `lungo` uses selector `2`
- base register `B = 10200`
- current values are therefore readable from:
  - `10201` strength
  - `10202` profile
  - `10203` temperature
  - `10205` coffee ml

### Family 900 / 900 Light standard recipe offsets

- `+1` strength
- `+2` profile
- `+3` preparation
- `+4` two cups
- `+5` coffee temperature
- `+6` water temperature
- `+7` milk temperature
- `+8` frothy milk temperature
- `+9` coffee ml
- `+10` water ml
- `+11` milk ml
- `+12` frothy milk ml
- `+13` overall temperature

Known write quirk:

- displayed fluid ml values are multiplied by `10` before `HW` writes on these families

### Family 1030 / 1040 standard recipe offsets

- `+1` strength
- `+2` profile
- `+3` preparation
- `+4` two cups
- `+5` coffee temperature
- `+6` water temperature
- `+7` milk temperature
- `+8` frothy milk temperature
- `+9` coffee ml
- `+10` water ml
- `+11` milk ml
- `+12` frothy milk ml

## Temporary Standard Recipe Scratch Space

The standard-drink brew flow writes a transient recipe snapshot before sending `HE`.

- scratch recipe type register: `9001`
- selector/type is written to `9001`
- standard recipe item writes are rebased into the scratch area as:
  - `9001 + offset`
- for example on family `700`, a temporary `lungo` brew with overridden strength/temperature writes:
  - `9001` selector/type
  - `9002` strength
  - `9003` profile
  - `9004` temperature
  - `9006` coffee ml

Interpretation:

- this scratch area is separate from persistent `MyCoffee` storage at `20000+`
- app behavior indicates it is a transient per-brew recipe model used to carry the current standard-drink values like strength, aroma/profile, volume, and temperature

## Statistics Register IDs

The base table below is specifically recovered from `StatisticsFactory::GetAvailableStatisticsForCoffeeMachine8000Er`.
Other families differ materially; see the notes after the 8000 table.

### Family 8000

Recipe/stat counters via `HR`:

- `200` espresso
- `201` coffee
- `202` americano
- `203` cappuccino
- `204` caffe latte
- `205` macchiato
- `206` warm milk
- `207` hot water
- `208` my coffee
- `209` steam / frothy drinks
- `210` powder coffee
- `213` total beverages
- `214` clean coffee system
- `215` clean frother
- `216` rinse cycles
- `219` filter changes
- `220` descaling
- `221` beverages started via app

Maintenance/status via `HR`:

- descale:
  - percent `600`
  - warning `601`
- brew unit cleaning:
  - percent `610`
  - warning `611`
- frother cleaning:
  - percent `620`
  - warning `621`
- filter:
  - percent `640`
  - warning `641`
  - dependency `642`

### Other family notes

- `1000` (`NICR1030` / `NICR1040`) differs:
  - `201 = lungo`
  - `206 = warm milk`
  - `207 = hot milk` only on `NICR1030`
  - `208 = milk foam`
  - `209 = hot water`
  - higher counters continue at `211..224`
  - filter dependency register is `101`
- `700` / `79x` differs:
  - `201 = cream` on `700`, but `coffee` on `79x`
  - `204 = cappuccino` exists only on non-`79x` `700`
  - these families stop at `208 = my coffee`
  - filter dependency register is `105`
  - live-verified on model `756` / family `700`:
    - `214 = cleaning brewing unit`, value `31`
    - `215 = cleaning frother`, value `1`
    - `216 = rinse cycles`, value `5080`
    - `219 = filter changes`, value `0`
    - `220 = descaling`, value `3`
    - `221 = beverages via app`, value `2`
    - `600 = descaling progress`, value `59`
    - `601 = descaling warning`, value `0`
    - `610 = brewing unit cleaning progress`, value `13`
    - `611 = brewing unit cleaning warning`, value `0`
    - `620 = frother cleaning progress`, value `13`
    - `621 = frother cleaning warning`, value `0`
    - `640 = filter progress`, value `0`
    - `641 = filter warning`, value `0`
    - `105 = filter dependency`, value `0`
  - `79x` maintenance registers beyond `105` are not live-verified yet
- `600` is sparser:
  - recipe stats present are `200`, `201`, `203`, `204`, `206`, `207`, `208`
  - filter dependency register is `105`
- `900` differs from `8000`:
  - `206 = milk`
  - `207 = hot water`
  - `208 = my coffee`
  - higher counters continue at `211..221`
  - filter dependency register is `101`

## Settings Register IDs

These values are recovered from the Android app setting factories. The bridge decodes them in `src/nivona.cpp` and exposes them through both the saved-machine API (`/api/machines/{serial}/settings`) and the low-level probe path (`/api/protocol/settings-probe`) by reading `HR` and mapping the low 16-bit code to the UI label.

### Family 8000

- `101` water hardness
  - `00 00` soft
  - `00 01` medium
  - `00 02` hard
  - `00 03` very hard
- `103` off-rinse
  - `00 00` off
  - `00 01` on
- `104` auto-off
  - `00 00` 10 min
  - `00 01` 30 min
  - `00 02` 1 h
  - `00 03` 2 h
  - `00 04` 4 h
  - `00 05` 6 h
  - `00 06` 8 h
  - `00 07` 10 h
  - `00 08` 12 h
  - `00 09` 14 h
  - `00 10` 16 h
- `105` coffee temperature
  - `00 00` off
  - `00 01` on

### Family 600 / 700 / 79x

- `101` water hardness
  - `00 00` soft
  - `00 01` medium
  - `00 02` hard
  - `00 03` very hard
- `102` temperature
  - `00 00` normal
  - `00 01` high
  - `00 02` max
  - `00 03` individual
- `103` off-rinse
  - `00 00` off
  - `00 01` on
  - not exposed by the app on `79x` models
- `104` auto-off
  - `00 00` 10 min
  - `00 01` 30 min
  - `00 02` 1 h
  - `00 03` 2 h
  - `00 04` 4 h
  - `00 05` 6 h
  - `00 06` 8 h
  - `00 07` 10 h
  - `00 08` 12 h
  - `00 09` off
- `106` profile
  - `00 00` dynamic
  - `00 01` constant
  - `00 02` intense
  - `00 03` individual
  - gated by Aroma Balance / Profile support
  - recovered app logic marks model `758` as lacking this profile setting

### Family 1030

- `102` water hardness
  - `00 00` soft
  - `00 01` medium
  - `00 02` hard
  - `00 03` very hard
- `103` off-rinse
  - `00 00` off
  - `00 01` on
- `109` auto-off
  - `00 00` 10 min
  - `00 01` 30 min
  - `00 02` 1 h
  - `00 03` 2 h
  - `00 04` 4 h
  - `00 05` 6 h
  - `00 06` 8 h
  - `00 07` 10 h
  - `00 08` 12 h
  - `00 09` off
- `113` profile
  - `00 00` dynamic
  - `00 01` constant
  - `00 02` intense
  - `00 03` individual
- `114` coffee temperature
  - `00 00` normal
  - `00 01` high
  - `00 02` max
  - `00 03` individual
- `115` water temperature
  - `00 00` normal
  - `00 01` high
  - `00 02` max
  - `00 03` individual
- `116` milk temperature
  - `00 00` high
  - `00 01` max
  - `00 02` individual
- additional app-known numeric settings on this family:
  - `20` clock / time value
  - `104` cup heater:
    - `00 00` eco
    - `00 01` active
  - `105` milk products active:
    - `00 00` off
    - `00 01` on
  - `106` direct-start deactivated:
    - `00 00` off
    - `00 01` on
  - `107` touch lock:
    - `00 00` off
    - `00 01` on
  - `110` auto-on deactivated:
    - `00 00` off
    - `00 01` on
  - `111` auto-on hour
  - `112` auto-on minute

### Family 1040

The `1040` family keeps the same base registers as `1030`, with these value-set extensions:

- `113` profile
  - `00 00` dynamic
  - `00 01` constant
  - `00 02` intense
  - `00 03` quick
  - `00 04` individual
- `116` milk temperature
  - `00 00` normal
  - `00 01` high
  - `00 02` hot
  - `00 03` max
  - `00 04` individual
- `117` milk foam temperature
  - `00 00` warm
  - `00 01` max
  - `00 02` individual
- `118` power-on rinse
  - `00 00` off
  - `00 01` on
- `119` power-on frother time
  - `00 00` 10 min
  - `00 01` 20 min
  - `00 02` 30 min
  - `00 03` 40 min

### Family 900

- `102` water hardness
  - `00 00` soft
  - `00 01` medium
  - `00 02` hard
  - `00 03` very hard
- `103` off-rinse
  - `00 00` off
  - `00 01` on
- `104` save energy
  - `00 00` off
  - `00 01` on
- `105` tank-light master enable
  - `00 00` off
  - `00 01` on
- `106` tank-light color
  - `00 00` red
  - `00 01` orange
  - `00 02` yellow
  - `00 03` green
  - `00 04` cyan
  - `00 05` blue
  - `00 06` magenta
  - `00 07` white
  - `00 08` disco
- `107` tank-light brightness
  - `00 00` normal
  - `00 01` bright
  - `00 02` maximum
- `108` touch lock
  - `00 00` off
  - `00 01` on
- `109` auto-off
  - `00 00` 10 min
  - `00 01` 30 min
  - `00 02` 1 h
  - `00 03` 2 h
  - `00 04` 4 h
  - `00 05` 6 h
  - `00 06` 8 h
  - `00 07` 10 h
  - `00 08` 12 h
  - `00 09` 14 h
  - `00 0A` 16 h
- `110` auto-on deactivated
  - `00 00` off
  - `00 01` on
- `111` auto-on hour
- `112` auto-on minute

### Family 900 Light

- `102` water hardness
  - `00 00` soft
  - `00 01` medium
  - `00 02` hard
  - `00 03` very hard
- `104` save energy
  - `00 00` off
  - `00 01` on
- `109` auto-off
  - `00 00` 10 min
  - `00 01` 30 min
  - `00 02` 1 h
  - `00 03` 2 h
  - `00 04` 4 h
  - `00 05` 6 h
  - `00 06` 8 h
  - `00 07` 10 h
  - `00 08` 12 h
  - `00 09` 14 h
  - `00 0A` 16 h
- `110` auto-on deactivated
  - `00 00` off
  - `00 01` on
- `111` auto-on hour
- `112` auto-on minute

## MyCoffee / Recipe Name Findings

Recovered recipe-name and slot-layout facts:

- named recipes use `HA` / `HB`
  - request register = 2-byte big-endian string register
  - response = echoed register + 64-byte name field
- newer families store the name payload as UTF-16LE
- MyCoffee slot size is `100` bytes per recipe slot
- MyCoffee slot base:
  - `B = 20000 + (MyCoffeeId - 1) * 100`
  - example:
    - slot `1` base = `20000`
    - slot `2` base = `20100`
    - slot `1` name register = `20002`
    - slot `2` name register = `20102`
- recovered slot counts:
  - `660`: `1`
  - `670` / `675` / `680`: `5`
  - `756` / `758` / `759` / `768` / `769` / `778` / `779`: `1`
  - `788` / `789`: `5`
  - `790` / `791` / `792` / `793` / `794` / `795` / `796` / `797` / `799`: `5`
  - `920` / `930`: `9`
  - `960` / `965` / `970`: `9`
  - `1030` / `1040`: `18`
  - `8101` / `8103` / `8107`: `9`

Recovered slot-relative offsets:

### Family 600 MyCoffee class

- `+0` enabled
- `+1` icon
- `+2` recipe name register
- `+3` recipe type
- `+4` strength
- `+5` profile
- `+6` temperature
- `+7` two cups
- `+8` coffee amount
- `+9` water amount
- `+11` frothy milk amount
- `+12` preparation

### Family 700 / 79x MyCoffee class

- `+0` enabled
- `+1` icon
- `+2` recipe name register
- `+3` recipe type
- `+4` strength
- `+5` profile
- `+6` temperature
- `+7` two cups
- `+8` coffee amount
- `+9` water amount
- `+10` milk amount
- `+11` frothy milk amount
- `79x` uses the same numeric register layout as `700`

### Family 900 MyCoffee class

- `+0` enabled
- `+1` icon
- `+2` recipe name register
- `+3` recipe type
- `+4` strength
- `+5` profile
- `+6` preparation
- `+7` two cups
- `+8` coffee temperature
- `+9` water temperature
- `+10` milk temperature
- `+11` frothy milk temperature
- `+12` coffee amount
- `+13` water amount
- `+14` milk amount
- `+15` frothy milk amount
- `+16` overall temperature
- fluid writes on `900` / `900 Light` multiply the displayed ml value by `10` before sending `HW`

### Family 1030 MyCoffee class

- `+0` enabled
- `+1` icon
- `+2` recipe name register
- `+3` recipe type
- `+4` strength
- `+5` profile
- `+6` preparation
- `+7` two cups
- `+8` coffee temperature
- `+9` water temperature
- `+10` milk temperature
- `+11` frothy milk temperature
- `+12` coffee amount
- `+13` water amount
- `+14` milk amount
- `+15` frothy milk amount

### Family 8000 MyCoffee class

- `+0` enabled
- `+2` recipe name register
- decompiled offset enums currently overlap `icon` and `recipe type` at `+3`
  - that overlap needs read/write confirmation on a live `8000` machine
- `+4` strength
- `+5` profile
- `+6` temperature
- `+7` two cups
- `+8` coffee amount
- `+9` water amount
- `+10` milk amount
- `+11` frothy milk amount

### Live family 700 MyCoffee read

Live bridge validation on the paired `756573071020106-----` machine confirms:

- this model exposes `1` MyCoffee slot
- slot `1` base register block:
  - base `20000`
  - enable register `20000`
  - name register `20002`
  - type register `20003`
- verified live reads on 2026-03-11:
  - `HR 20000` -> payload `4E2000000001`
    - enabled = `1`
  - `HA 20002` -> payload
    - `4E224D4F4A41204B41574100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000`
    - raw machine bytes decode to `MOJA KAWA`
    - app/UI legacy decode displays `MOJAKAWA`
  - `HR 20003` -> payload `4E2300000000`
    - type selector = `0`
    - base recipe = `espresso`
  - `HR 20004` -> payload `4E2400000002`
    - strength = `2`
    - on this `756` model with `3` strength levels:
      - app/UI label = strong / `3` beans
  - `HR 20006` -> payload `4E2600000001`
    - temperature = `1`
    - app/UI label = high
  - `HR 20008` -> payload `4E2800000032`
    - coffee amount = `50` ml
- practical note:
  - on this plain `700` family unit, `HA` uses the legacy 1-byte path rather than UTF-16LE
  - the legacy path is a direct byte-to-char cast plus alnum filtering, not plain ASCII decoding
  - the verified live `MOJAKAWA` slot values now match the iOS UI report:
    - `3` beans
    - `50 ml`
    - `high`

## Known Working Serialized Examples

Confirmed request-side vectors:

- `Hp`, payload `00 00`, plain:
  - `53 48 70 00 00 47 45`
- `HU`, payload `FA 48 D1 7B 7E 6E`, encrypted task packet:
  - `53 48 55 E5 80 D0 36 79 81 BC 45`
- `HU`, payload `FA 48 D1 7B 7E 6E`, plain helper packet:
  - `53 48 55 FA 48 D1 7B 7E 6E E8 45`
- `HV`, session `12 34`, encrypted:
  - `53 48 56 0D FC 1A 45`
- `HR`, session `12 34`, payload `00 D5 00 00 04 D2`, encrypted:
  - `53 48 52 0D FC 01 98 07 EF 50 90 BE 45`
- `HR`, no session, payload `00 D5 00 00 04 D2`, encrypted:
  - `53 48 52 1F 1D 01 4D 03 3D EE 45`
- verified live settings-probe bootstrap on family `700` / model `756`:
  - `HU` seed:
    - `12 31 4B F7`
  - `HU` request packet:
    - `53 48 55 0D F9 4A BA 5F EC D6 45`
  - `HU` response packet:
    - `53 48 55 0D F9 4A BA 39 FA 98 1D 95 45`
  - decoded `HU` payload:
    - `12 31 4B F7 3E 15 CC 5F`
  - validated session key:
    - `3E 15`
- verified live family `700` setting read with session `3E 15`:
  - `HR 104` (`auto_off`) request packet:
    - `53 48 52 21 DD 01 25 AD 45`
  - response packet:
    - `53 48 52 1F A0 01 4D 07 EB AD 45`
  - decoded payload:
    - `00 68 00 00 00 04`
  - decoded UI value:
    - `4 h`
- verified live family `700` setting read with session `3E 15`:
  - `HR 106` (`profile`) request packet:
    - `53 48 52 21 DD 01 27 AF 45`
  - response packet:
    - `53 48 52 1F A2 01 4D 07 ED AD 45`
  - decoded payload:
    - `00 6A 00 00 00 02`
  - decoded UI value:
    - `intense`

That is enough to implement a bridge-side sender without relying on the APK runtime.
The remaining unresolved piece is the exact encrypted on-wire response bytes accepted by the Android-added parser definitions.

## Live Device Note

Unpaired live validation on macOS against the machine in range confirmed:

- BLE identity visible via CoreBluetooth:
  - device identifier:
    - `8507E1FE-D202-02DB-4D11-8FABB9F8A8F5`
  - advertised name:
    - `756573071020106-----`
- Readable without pairing:
  - Device Information:
    - manufacturer:
      - `EF`
    - model:
      - `EF-BTLE`
    - firmware revision:
      - `386`
    - software revision:
      - `EF_1.00R4__386`
  - custom characteristic `AD06`:
    - hex:
      - `3735363537333037313032303130362D2D2D2D2D`
    - ASCII:
      - `756573071020106-----`
  - extra service characteristic `1534`:
    - `0000`
- Present on this unit:
  - `2A29`, `2A24`, `2A26`, `2A28`
- Not present on this unit:
  - `2A25`
  - `2A27`
- Not usable without bonding on macOS:
  - `AD02` notifications
  - starting notify fails with:
    - `CBATTErrorDomain Code=15 "Encryption is insufficient."`

This means unpaired communication is sufficient for discovery, service inspection, DIS reads, and a few custom read characteristics, but not for live `HR`/`HA`/`HX` application-protocol responses on this host.

The live machine scanned during this work exposed one additional custom service:

- `00001530-B089-11E4-AD45-0002A5D5C51B`

with characteristics:

- `00001531-B089-11E4-AD45-0002A5D5C51B` (`write,notify`)
- `00001532-B089-11E4-AD45-0002A5D5C51B` (`write-without-response`)
- `00001534-B089-11E4-AD45-0002A5D5C51B` (`read`)

I did not find this service referenced by the Android app logic, so it is documented here only as a live observation.

Later ESP32 generic GATT probing recovered more concrete behavior on this same unit:

- `1534` read returns:
  - `0000`
- `1531` write with temporary notify subscription:
  - writing `0000` triggers notify `100003`
  - writing `0001` triggers notify `100003`
  - writing `0002` triggers notify `100003`
  - `1534` remains `0000` after those writes
- `1532` write-without-response:
  - writing `0000` produced no visible notification
  - `1534` still remained `0000`

Current interpretation:

- `1530` is definitely a real live service on the machine.
- But current evidence does not support it as the missing gate for `AD00` / `HU` / `HR` traffic.
- On this unit it currently behaves more like a small independent side-channel with a stable `1534 = 0000` readback and a generic `1531 -> 100003` notify response than like a session-unlock mechanism.

ESP32 bridge live validation against the paired machine now shows a working end-to-end flow:

- plain `Hp` returns `AD02` notifications immediately
- encrypted internal `HU` succeeds and returns a valid 8-byte response:
  - `seed4 || session_key2 || verifier2`
- the returned session key changes per live session and is then required on subsequent request sends
- once `HU` has succeeded, encrypted public-frame reads work on the same bonded link:
  - `HV`
  - `HL`
  - `HX`
  - `HR`
- the matching app-style probe also works when it reuses a valid stored session key established by `HU`
- validated live response rule:
  - post-`HU` responses are encrypted
  - but they are sessionless on the wire and must be decoded without stripping a 2-byte session prefix
- live example decodes from the bridge:
  - `HV` payload:
    - `3035373349303230493131`
  - `HL` payload:
    - ASCII `756573071020106-----`
  - `HX` payload:
    - `0008000000000000`
    - decoded: `process=8`, `sub_process=0`, `message=0`, `progress=0`
    - app interpretation on the `700`-style path: `ready`
  - `HR 213` (`total_beverages`) payload:
    - `00D500000D05`
    - value `3333`
  - `HR 200` (`espresso`) payload:
    - `00C800000347`
    - value `839`
- app-style requests without first establishing a valid live `HU` session still remain silent on this unit
- app-style bridge probes also report the live `AD03` write capabilities on this unit:
  - `canWrite = true`
  - `canWriteNoResponse = false`
  - so the Android runtime’s “prefer WriteWithoutResponse when available” policy degenerates to normal write-with-response here

So current live evidence says:

- BLE transport, pairing, and `AD02` notification delivery are working
- `HU` / `HV` / `HL` / `HX` / `HR` are now live-validated through the bridge
- the breakthrough was correcting the runtime key path and removing the false response-side session-echo assumption
- post-OTA bridge validation on 2026-03-11 also confirms the new settings probe on the same family `700` / model `756` machine:
  - `HR 101` (`water_hardness`) -> `soft`
  - `HR 102` (`temperature`) -> `individual`
  - `HR 103` (`off_rinse`) -> `on`
  - `HR 104` (`auto_off`) -> `4 h`
  - `HR 106` (`profile`) -> `intense`
  - the bridge-side `settings-probe` now works only after performing `HU` first, exactly like the working low-level bridge path

## Open Items

- Global factory reset wire mappings are now recovered
  - settings reset: `HE 00 32` plus `16` zero bytes
  - recipes reset: `HE 00 33` plus `16` zero bytes
  - still confirm whether the same global `0x32` / `0x33` entries exist on `8000`, `900`, and `900 Light`
  - still complete the per-item `HD` reset target tables for recipe-item defaults by family
- Extend live validation to additional command families
  - especially `HA`, `HS`, and any additional `HI` feature bits beyond `ImageTransfer`
- Fully document the EFLibrary internal `z.d()` derivation path from the installed 64-byte `customer_key`
  - the exact final 32-byte RC4 key is now recovered and documented above
  - the remaining gap is the full symbolic explanation of the internal derivation helper chain, not the resulting key material
- Full register map for:
  - diagnostics
  - full recipe slot base/count mapping and writable field coverage
- Older model-family string encoding
  - the bridge now documents and implements the split between UTF-16 and legacy “funny string” mode, but the full legacy character map is still not documented here
- Clarify whether the live-only `00001530-B089-11E4-AD45-0002A5D5C51B` service has protocol relevance or is unrelated to the app path
