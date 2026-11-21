"""beken platform ownership; no SDK or device imports at registration time."""

from .. import PlatformDefinition

PLATFORM = PlatformDefinition(
    key="beken",
    sdk_key="beken",
    package=__name__,
    adapter_name="BekenAdapter",
    flasher_name="BekenFlasher",
)
