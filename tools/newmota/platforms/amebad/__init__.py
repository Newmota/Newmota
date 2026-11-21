"""amebad platform ownership; no SDK or device imports at registration time."""

from .. import PlatformDefinition

PLATFORM = PlatformDefinition(
    key="amebad",
    sdk_key="amebad",
    package=__name__,
    adapter_name="AmebadAdapter",
    flasher_name="AmebadFlasher",
)
