# Wire format

The bridge emits head-tracking data over **UDP to `127.0.0.1`** on two adjacent
ports. With `--port N` (default `4242`):

| Port    | Payload                                                   |
| ------- | -------------------------------------------------------- |
| `N`     | OpenTrack binary: six native little-endian `double`s     |
| `N + 1` | UTF-8 JSON object, one datagram per sample               |

Output is loopback-only and **unauthenticated**. Do not forward it to an
untrusted network.

## Port `N` — OpenTrack doubles

Exactly 48 bytes: six IEEE-754 little-endian `double`s, in OpenTrack pose order:

```
x, y, z, yaw, pitch, roll
```

The translation axes `x, y, z` are always `0.0` — the Android Head Tracker
protocol reports orientation only — so the head angles occupy the last three
slots. `yaw`, `pitch`, and `roll` are in **degrees**. This is the exact layout
OpenTrack's "UDP over network" input reads into its pose array
(`[TX, TY, TZ, Yaw, Pitch, Roll]`).

## Port `N + 1` — JSON telemetry

One UTF-8 JSON object per sample, no trailing newline. `version` is currently
`2`.

| Field              | Type                | Units / notes                                              |
| ------------------ | ------------------- | ---------------------------------------------------------- |
| `version`          | int                 | Schema version (`2`).                                      |
| `rotationVector`   | `[x, y, z]`         | Axis-angle orientation, radians.                           |
| `quaternion`       | `[w, x, y, z]`      | Recentered orientation quaternion.                         |
| `yprDegrees`       | `[yaw, pitch, roll]`| Degrees.                                                   |
| `gyroscope`        | `[x, y, z]` or `null` | Angular velocity, rad/s. `null` if the device omits it.  |
| `accelerometer`    | `[x, y, z]` or `null` | Linear acceleration, m/s². `null` if not reported.       |
| `angularVelocity`  | `[x, y, z]` or `null` | **Deprecated** alias of `gyroscope`, kept for v1 readers. |
| `resetCounter`     | int                 | Increments when the headset re-zeros its reference frame.  |
| `packetsPerSecond` | number              | Measured input report rate.                                |
| `receiveLatencyMs` | number              | Device-timestamp latency, or `-1` when unavailable.        |

All vector fields share the configured axis convention (default **YXZ order
with the X and Z axes inverted**); the gyroscope and accelerometer are remapped
to match orientation.

### Example

```json
{"version":2,"rotationVector":[0.012,-0.004,0.31],"quaternion":[0.987,0.006,-0.002,0.155],"yprDegrees":[17.84,-0.46,1.37],"gyroscope":[0.01,0.0,-0.02],"accelerometer":null,"angularVelocity":[0.01,0.0,-0.02],"resetCounter":0,"packetsPerSecond":25.0,"receiveLatencyMs":-1.0}
```

### Minimal Python reader

```python
import json, socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 4243))  # JSON port = bridge --port + 1
while True:
    data, _ = sock.recvfrom(2048)
    sample = json.loads(data)
    yaw, pitch, roll = sample["yprDegrees"]
    print(f"yaw={yaw:7.2f}  pitch={pitch:7.2f}  roll={roll:7.2f}  gyro={sample['gyroscope']}")
```

## Compatibility

`version` increases when fields change meaning or are removed. Additive,
backward-compatible fields may appear without a version bump, so readers should
ignore unknown keys.
