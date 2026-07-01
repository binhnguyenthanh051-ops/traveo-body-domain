# can_echo_probe

Bench tool to verify the Node A gateway's **CAN FD echo** on the real bus (M2 seam 2,
Phase B). Sends FD frames and asserts the gateway echoes each one back; being on the
bus also provides the ACK the lone gateway node needs. Manual tool (needs the board +
interface) — not a CI/pytest test.

No CANalyzer license required — only the free **Vector XL driver** + `python-can`.

## Files
- `can_echo_probe.py` — the logic.
- `config.json` — all user settings (interface, bus, test). No code edits needed.

## Setup (once)
```bash
pip install "python-can>=4.0"      # the 'vector' backend ships with it
```
- Install the free **Vector XL Driver Library** (Vector Driver Setup).
- In **Vector Hardware Config**, assign the VN1610 channel to an application whose name
  matches `interface.app_name` (default `python-can`). ← the usual first-run gotcha; if
  the bus won't open, it's almost always this.

## Configure — `config.json`
```jsonc
{
  "interface": {
    "backend": "vector",     // python-can backend; "socketcan" etc. also work
    "channel": 0,            // channel index
    "app_name": "python-can",// must match the Vector Hardware Config app mapping
    "serial": null           // device id: null = first device, or a VN1610 serial number
  },
  "bus": {
    "can_fd": true,
    "nominal_bitrate": 500000,   // arbitration phase (ADR-0011 D1)
    "data_bitrate": 2000000,     // FD data phase
    "bitrate_switch": true       // BRS
  },
  "test": {
    "tx_id": "0x200",   // 0x200 = sensor report (also drives bodyctl); hex or decimal
    "count": 10,
    "timeout_s": 1.0,   // per-frame echo wait
    "interval_s": 0.2   // delay between sends
  }
}
```

## Run
```bash
python can_echo_probe.py                 # uses ./config.json
python can_echo_probe.py --config my.json
# -> "10/10 echoes received"
```

## Gateway-side prerequisites
1. Build the app with **`CAN_LOOPBACK_TEST = 0`** (the default) — real-bus mode: echo on,
   loopback + self-transmit off. Flash it.
2. **120 Ω** termination at both ends; the kit's **CAN transceiver enabled**.
3. Wire VN1610 CANH/CANL/GND to the kit's CAN connector.

## If you get `0/N` echoes
- Scope for error frames -> bit-timing mismatch (compare the configurator's timing vs
  `config.json`).
- Confirm the transceiver isn't in standby, and the VN1610's own 120 Ω termination is on.
- `tx_id 0x200` also exercises the `bodyctl` courtesy-light path (door-ajar toggles each
  frame) — but the **echo** is what's asserted.
