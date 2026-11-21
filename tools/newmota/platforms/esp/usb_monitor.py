"""Attach ESP-IDF monitor to an on-chip USB console without resetting it."""

from __future__ import annotations

from esp_idf_monitor.base.serial_reader import SerialReader
from esp_idf_monitor.idf_monitor import main as idf_monitor_main


def _open_without_control_line_changes(reader: SerialReader, reset: bool) -> None:
    if reset:
        raise RuntimeError("on-chip USB monitor requires --no-reset")

    # The stock monitor changes DTR and RTS around every open. On an ESP32-S3
    # on-chip USB port that is a device reset/bootloader control sequence, not
    # an inert serial-port setup operation. Preserve the running application.
    reader.serial.dtr = False
    reader.serial.rts = False
    reader.serial.open()


def main() -> None:
    SerialReader.open_serial = _open_without_control_line_changes
    idf_monitor_main()


if __name__ == "__main__":
    main()
