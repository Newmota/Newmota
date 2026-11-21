"""pico platform ownership; no SDK or device imports at registration time."""

from .. import PlatformDefinition

PLATFORM = PlatformDefinition(
    key="pico",
    sdk_key="pico-sdk",
    package=__name__,
    adapter_name="PicoSdkAdapter",
    flasher_name="PicoFlasher",
)
