"""Frozen energy-port identity inventory; an entry does not imply implementation.

Never reuse these Newmota IDs. Upstream XNRG identifiers are provenance only and
are deliberately not the runtime namespace (BL0906 and BIOPDU both use 24).
"""

from dataclasses import dataclass

UPSTREAM_ENERGY_BASELINE = "c00ece86819fce87a4ad7c440b1995623ec6a5e0"


@dataclass(frozen=True)
class EnergyInventoryEntry:
    """Permanent source provenance plus user scope, not proof of implementation.

    support_scope excludes retired or user-deferred runtime selections without freeing IDs.
    berry_age_scope independently records the requested age-parity scope.
    """

    driver_id: int
    source: str
    variants: tuple[str, ...]
    transport: str
    support_scope: str = "included"
    berry_age_scope: str = "included"
    scope_reason: str = ""


ENERGY_INVENTORY = (
    EnergyInventoryEntry(1, "xnrg_01_hlw8012.ino", ("HLW8012",), "edge capture"),
    EnergyInventoryEntry(2, "xnrg_02_cse7766.ino", ("CSE7766", "CSE7759", "HLW8032"), "UART"),
    EnergyInventoryEntry(3, "xnrg_03_pzem004t.ino", ("PZEM004T",), "UART"),
    EnergyInventoryEntry(4, "xnrg_04_mcp39f501.ino", ("MCP39F501",), "UART"),
    EnergyInventoryEntry(5, "xnrg_05_pzem_ac.ino", ("PZEM AC",), "Modbus RTU"),
    EnergyInventoryEntry(6, "xnrg_06_pzem_dc.ino", ("PZEM DC",), "Modbus RTU"),
    EnergyInventoryEntry(
        7,
        "xnrg_07_ade7953.ino",
        ("ADE7953 Shelly 2.5 I2C",),
        "I2C",
        support_scope="excluded_by_user",
        berry_age_scope="excluded_by_user",
        scope_reason="ESP8266 Shelly 2.5 model excluded; unrelated ADE7953 variants remain independently selectable.",
    ),
    EnergyInventoryEntry(35, "xnrg_07_ade7953.ino", ("ADE7953 EM",), "I2C"),
    EnergyInventoryEntry(36, "xnrg_07_ade7953.ino", ("ADE7953 Plus 2PM / Gen3",), "I2C"),
    EnergyInventoryEntry(37, "xnrg_07_ade7953.ino", ("ADE7953 Pro 1PM",), "SPI"),
    EnergyInventoryEntry(38, "xnrg_07_ade7953.ino", ("ADE7953 Pro 2PM",), "SPI"),
    EnergyInventoryEntry(39, "xnrg_07_ade7953.ino", ("ADE7953 Pro 4PM",), "SPI"),
    EnergyInventoryEntry(8, "xnrg_08_sdm120.ino", ("SDM120",), "Modbus RTU"),
    EnergyInventoryEntry(9, "xnrg_09_dds2382.ino", ("DDS2382",), "Modbus RTU"),
    EnergyInventoryEntry(10, "xnrg_10_sdm630.ino", ("SDM630",), "Modbus RTU"),
    EnergyInventoryEntry(11, "xnrg_11_ddsu666.ino", ("DDSU666",), "Modbus RTU"),
    EnergyInventoryEntry(
        12,
        "xnrg_12_solaxX1.ino",
        ("Solax X1 inverter and meter gateway",),
        "UART/optional DE",
        support_scope="deferred_by_user",
        berry_age_scope="explicit_request",
        scope_reason="Runtime driver deferred until explicit user request; permanent ID and source provenance retained.",
    ),
    EnergyInventoryEntry(13, "xnrg_13_fif_le01mr.ino", ("LE01MR",), "Modbus RTU"),
    EnergyInventoryEntry(14, "xnrg_14_bl09xx.ino", ("BL0939 dual",), "UART"),
    EnergyInventoryEntry(41, "xnrg_14_bl09xx.ino", ("BL0940 single",), "UART"),
    EnergyInventoryEntry(42, "xnrg_14_bl09xx.ino", ("BL0942 single baud/sign variants",), "UART"),
    EnergyInventoryEntry(
        15,
        "xnrg_15_teleinfo.ino",
        ("Teleinfo Historique", "Teleinfo Standard"),
        "UART 7E1/module enable",
        support_scope="deferred_by_user",
        berry_age_scope="explicit_request",
        scope_reason="Runtime driver deferred until explicit user request; permanent ID and source provenance retained.",
    ),
    EnergyInventoryEntry(16, "xnrg_16_iem3000.ino", ("iEM3000",), "Modbus RTU"),
    EnergyInventoryEntry(17, "xnrg_17_ornowe517.ino", ("ORNO WE517",), "Modbus RTU"),
    EnergyInventoryEntry(18, "xnrg_18_sdm72.ino", ("SDM72",), "Modbus RTU"),
    EnergyInventoryEntry(19, "xnrg_19_cse7761.ino", ("CSE7761 Dual R3",), "UART/zero cross"),
    EnergyInventoryEntry(40, "xnrg_19_cse7761.ino", ("CSE7761 PowCT",), "UART"),
    EnergyInventoryEntry(20, "xnrg_21_sdm230.ino", ("SDM230",), "Modbus RTU"),
    EnergyInventoryEntry(
        21,
        "xnrg_22_bl6523.ino",
        ("BL6523 passive dual receive",),
        "2 UART RX",
        support_scope="excluded_by_user",
        berry_age_scope="excluded_by_user",
        scope_reason="Exotic passive dual-UART device excluded at user request; ID 21 reserved.",
    ),
    EnergyInventoryEntry(
        22,
        "xnrg_23_ade7880.ino",
        ("ADE7880 generic chip (three phase, neutral)",),
        "I2C/edge capture",
    ),
    EnergyInventoryEntry(23, "xnrg_24_bl0906.ino", ("BL0906 Athom 1–6 channels",), "UART"),
    EnergyInventoryEntry(
        24,
        "xnrg_24_esp32_biopdu.ino",
        ("BIOPDU-625x12 mux + MCP23008",),
        "Modbus RTU/I2C/GPIO mux",
        support_scope="deferred_by_user",
        berry_age_scope="explicit_request",
        scope_reason="Runtime driver deferred until explicit user request; ID and source provenance retained.",
    ),
    EnergyInventoryEntry(
        25, "xnrg_25_v9240.ino", ("V9240 single phase",), "UART 8O1/low-baud reset"
    ),
    EnergyInventoryEntry(
        26,
        "xnrg_26_mk_sky_blu.ino",
        ("MakeSkyBlue S3 V118/V119",),
        "UART/optional broadcast TX",
        support_scope="deferred_by_user",
        berry_age_scope="explicit_request",
        scope_reason="Runtime driver deferred until explicit user request; permanent ID and source provenance retained.",
    ),
    EnergyInventoryEntry(
        27,
        "xnrg_28_berry.ino",
        ("Berry virtual provider and safe instance API",),
        "Berry/EnergyService",
    ),
    EnergyInventoryEntry(28, "xnrg_29_modbus.ino", ("Configured Modbus",), "Modbus RTU"),
    EnergyInventoryEntry(30, "xnrg_30_dummy.ino", ("Dummy",), "virtual"),
    EnergyInventoryEntry(
        31,
        "xdrv_45_shelly_dimmer.ino",
        ("Shelly Dimmer 1/2 external controller (disabled by upstream ESP32 globals)",),
        "UART/reset/BOOT0; optional AN3155 STM32 firmware upload",
        support_scope="excluded_by_user",
        berry_age_scope="excluded_by_user",
        scope_reason="Shelly Dimmer and controller firmware upgrade removed at user request; ID 31 remains permanently reserved.",
    ),
    EnergyInventoryEntry(
        32,
        "xdrv_16_tuyamcu_v1.ino;xdrv_16_tuyamcu_v2.ino",
        ("Tuya v1", "Tuya v2"),
        "integrated UART",
    ),
    EnergyInventoryEntry(34, "xnrg_01_hlw8012.ino", ("BL0937", "HJL"), "edge capture"),
    EnergyInventoryEntry(
        33, "xsns_02_analog.ino", ("ADC voltage/current energy",), "configured ADC sampling"
    ),
)

# Compatibility is broader than protocol decoding. These surfaces remain part
# of the port even when no descriptor or hardware protocol directly owns them.
ENERGY_COMPATIBILITY_SURFACES = (
    "xdrv_03_energy.ino",
    "xdrv_52_3_berry_energy.ino",
    "be_energylib.c",
    "commands/calibration/configuration/address/reset/phase mapping",
    "Status 8/9",
    "MQTT SENSOR",
    "events",
    "web/readings",
    "display",
    "today/yesterday/total/tariffs/local clock",
    "validity/staleness",
    "threshold/delta reporting",
    "meter-to-relay protections/hold windows",
)
