# Contributing to UAV-DAQ

Thanks for your interest. UAV-DAQ: STM32F401RE flight data acquisition with SD logging, OLED and UART telemetry, plus a Python ground station.

## Getting started

Firmware: import the project in STM32CubeIDE (the HAL, FatFs and linker script are regenerated from the `.ioc` with STM32CubeMX; see `firmware/README.md`). Ground station:

```
cd ground_station
python -m uavdaq_gs monitor --replay ../data/telemetry.csv --every 50 --quiet
```

## Testing

```
make -C firmware/tests                              # firmware logic on host, gcc + ASan/UBSan, simulated SD card
cd ground_station && python -m unittest discover -s tests -v
cd ground_station && ruff check . && ruff format --check . && mypy --ignore-missing-imports uavdaq_gs
```

CI also runs cppcheck on the firmware sources. The firmware is not cross-compiled in CI, so changes to it must be tested on the board.

## Pull requests

- Open an issue first for larger changes.
- Keep PRs focused and fill in the pull request template.
- Follow `.editorconfig` and match the style of surrounding code.
- Firmware changes must say which board they were tested on.

## Conduct

Participation is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
