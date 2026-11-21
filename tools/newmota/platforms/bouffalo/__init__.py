"""bouffalo platform ownership; no SDK or device imports at registration time."""

from .. import PlatformDefinition

PLATFORM = PlatformDefinition(
    key="bouffalo",
    sdk_key="bouffalo",
    package=__name__,
    adapter_name="BouffaloAdapter",
    flasher_name="BouffaloFlasher",
)
