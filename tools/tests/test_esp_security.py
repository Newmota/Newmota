"""ESP probe security state from esptool output (no devices)."""

from __future__ import annotations

import unittest

from newmota.platforms.esp.device import _esp32_security, _security_from_info

ESP32_FLASH_ID = "Chip type:          ESP32-D0WD-V3 (revision v3.1)\nDetected flash size: 4MB\n"


def read_mem(rdata0: int, rdata6: int) -> str:
    return f"0x3ff5a000 = 0x{rdata0:08x}\n0x3ff5a018 = 0x{rdata6:08x}\n"


class EspSecurityTest(unittest.TestCase):
    def test_security_info_states(self):
        output = "Secure Boot: Disabled\nFlash Encryption: Enabled\n"
        self.assertEqual(
            _security_from_info(output),
            {"secure_boot": False, "flash_encryption": True, "secure_download_mode": False},
        )
        self.assertEqual(
            _security_from_info("Chip is in Secure Download Mode\n"),
            {"secure_boot": None, "flash_encryption": None, "secure_download_mode": True},
        )

    def test_esp32_without_security(self):
        self.assertEqual(
            _esp32_security(ESP32_FLASH_ID + read_mem(0, 0)),
            {"secure_boot": False, "flash_encryption": False, "secure_download_mode": False},
        )

    def test_esp32_flash_encryption_counts_crypt_bits(self):
        self.assertTrue(
            _esp32_security(ESP32_FLASH_ID + read_mem(0b1 << 20, 0))["flash_encryption"]
        )
        self.assertFalse(
            _esp32_security(ESP32_FLASH_ID + read_mem(0b11 << 20, 0))["flash_encryption"]
        )

    def test_esp32_secure_boot_v1_and_v2(self):
        self.assertTrue(_esp32_security(ESP32_FLASH_ID + read_mem(0, 1 << 4))["secure_boot"])
        self.assertTrue(_esp32_security(ESP32_FLASH_ID + read_mem(0, 1 << 5))["secure_boot"])
        older = "Chip is ESP32-D0WD (revision v1.0)\n"
        self.assertFalse(_esp32_security(older + read_mem(0, 1 << 5))["secure_boot"])

    def test_esp32_missing_words_are_unknown(self):
        self.assertEqual(
            _esp32_security(ESP32_FLASH_ID),
            {"secure_boot": None, "flash_encryption": None, "secure_download_mode": None},
        )


if __name__ == "__main__":
    unittest.main()
