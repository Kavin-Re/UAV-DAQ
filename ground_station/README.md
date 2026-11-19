# UAV-DAQ ground station

Pure-Python (standard library only) tools for the UAV-DAQ telemetry link.
`pyserial` is needed only to talk to a real serial port and `matplotlib` only
for `--plot` / `--save-plot`.

```bash
cd ground_station
python -m uavdaq_gs --help

# No hardware: push the sample flight through encoder -> decoder -> CSV
python -m uavdaq_gs monitor --replay ../data/telemetry.csv --every 50 --quiet --log replay.csv

# Same, with 5 % of packets hit by a bit error and checksums enforced
python -m uavdaq_gs monitor --replay ../data/telemetry.csv --every 50 --quiet \
    --checksum --require-checksum --corrupt 0.05

# Send the flight to the board at 5 lines/s (pip install pyserial)
python -m uavdaq_gs stream --port /dev/ttyUSB0 --input ../data/telemetry.csv --every 6

# Tests
python -m unittest discover -s tests -v
```

See [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md) for the wire formats.
