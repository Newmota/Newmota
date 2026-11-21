"""esp platform ownership; no SDK or device imports at registration time."""

from .. import PlatformDefinition

PLATFORM = PlatformDefinition(
    key="esp",
    sdk_key="esp-idf",
    package=__name__,
    adapter_name="EspIdfAdapter",
    flasher_name="EspIdfFlasher",
)
