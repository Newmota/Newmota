"""Build, set up, and operate Newmota targets through their native SDKs."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

from .model import PLATFORMS, BuildError, CatalogError, SetupUnavailable
from .progress import format_elapsed

# Commands that build and then finish on their own; a monitor runs until interrupted.
TOTAL_TIME_COMMANDS = {"build", "flash", "update"}


def command_list(_: argparse.Namespace) -> None:
    from .pipeline import print_environments

    print_environments()


def command_validate(_: argparse.Namespace) -> None:
    from .catalog import validate_catalog

    result = validate_catalog()
    for note in result.notes:
        print(f"NOTE: {note}")
    if result.errors:
        raise BuildError("catalog validation failed:\n  " + "\n  ".join(result.errors))
    print("Catalog validation passed")


def command_build(args: argparse.Namespace) -> None:
    from .pipeline import build_selected, select_environments

    selected = select_environments(args.environment, args.all, args.chip, args.matrix, args.board)
    build_selected(selected, args.jobs)


def command_clangd(args: argparse.Namespace) -> None:
    from .pipeline import configure_clangd, select_environments

    selected = select_environments([args.environment], False, None)
    configure_clangd(selected[0], args.jobs)


def command_setup(args: argparse.Namespace) -> int:
    # This command also runs under the isolated host Python, outside the project venv.
    from .catalog import load_environments, load_user_environments, resolve_environment
    from .host import SdkVersionError, check_sdk_version
    from .model import esp_sdk_key
    from .platforms import platform_for_setup

    grouped: dict[tuple[str, str | None], set[str]] = {}
    if args.target:
        for target in args.target:
            parts = target.split(":")
            platform, separator, chip = target.partition(":")
            sdk_key = None
            if len(parts) == 3 and platform == "esp":
                if parts[1] not in {"5.5", "6.1"}:
                    raise CatalogError(f"invalid ESP-IDF version in setup target {target}")
                sdk_key, chip = esp_sdk_key(parts[1]), parts[2]
            if (
                len(parts) not in {2, 3}
                or (len(parts) == 3 and platform != "esp")
                or not separator
                or platform not in PLATFORMS
                or not chip
            ):
                raise CatalogError(f"invalid setup target '{target}'; use PLATFORM:SDK_TARGET")
            grouped.setdefault((platform, sdk_key), set()).add(chip)
    elif args.env:
        for name in args.env:
            item = resolve_environment(name)
            grouped.setdefault(
                (item.platform, item.sdk_key if item.platform == "esp" else None), set()
            ).add(item.sdk_target)
    else:
        platforms = PLATFORMS if args.platform == "all" else (args.platform,)
        environments = load_environments()
        for item in (*environments.values(), *load_user_environments(environments).values()):
            if item.platform in platforms:
                grouped.setdefault(
                    (item.platform, item.sdk_key if item.platform == "esp" else None), set()
                ).add(item.sdk_target)

    failures = []
    for (platform, sdk_key), targets in grouped.items():
        chips = sorted(targets)
        print(f"\n== {platform} {'ensure' if args.ensure else 'setup'} ==", flush=True)
        definition = platform_for_setup(platform)
        try:
            if args.ensure:
                issue = (
                    definition.setup_issue(chips, sdk_key=sdk_key)
                    if sdk_key
                    else definition.setup_issue(chips)
                )
                if issue is None:
                    print(f"{platform} setup is ready")
                    continue
                print(f"{platform} setup required: {issue}")
            if sdk_key:
                definition.setup(chips, args.dry_run, sdk_key=sdk_key)
            else:
                definition.setup(chips, args.dry_run)
            if args.ensure and not args.dry_run:
                issue = (
                    definition.setup_issue(chips, sdk_key=sdk_key)
                    if sdk_key
                    else definition.setup_issue(chips)
                )
                if issue is not None:
                    raise SetupUnavailable(f"{platform} setup remains incomplete: {issue}")
        except SetupUnavailable as error:
            print(f"setup unavailable: {error}", file=sys.stderr)
            failures.append(platform)
    if args.platform in {"esp", "all"}:
        for sdk_key in ("esp-idf", "esp-idf-v6.1"):
            if ("esp", sdk_key) in grouped:
                continue
            print(f"\n== esp checkout {sdk_key} ==", flush=True)
            try:
                if args.ensure and not args.dry_run:
                    try:
                        check_sdk_version(sdk_key)
                        print(f"{sdk_key} checkout is ready")
                        continue
                    except SdkVersionError:
                        pass
                platform_for_setup("esp").setup_checkout(args.dry_run, sdk_key)
            except (OSError, subprocess.CalledProcessError, SdkVersionError) as error:
                print(f"setup unavailable: {error}", file=sys.stderr)
                failures.append(sdk_key)
    if failures:
        print(f"setup incomplete: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0


def command_sdk_check(args: argparse.Namespace) -> int:
    from .host import sdk_main

    return sdk_main(args.sdk, as_json=args.json)


def command_sdk_checkout(args: argparse.Namespace) -> int:
    import subprocess

    from .host import SdkVersionError, sdk_checkout
    from .model import SetupUnavailable

    try:
        sdk_checkout(args.sdk, args.dry_run)
    except (OSError, SdkVersionError, SetupUnavailable, subprocess.CalledProcessError) as error:
        print(f"sdk-checkout failed: {error}", file=sys.stderr)
        return 1
    return 0


def command_sdk_fetch(args: argparse.Namespace) -> int:
    import subprocess

    from .host import SDK_NAMES, SdkVersionError, sdk_fetch
    from .model import SetupUnavailable

    try:
        sdk_fetch(tuple(args.sdk) if args.sdk else SDK_NAMES, args.dry_run)
    except (OSError, SdkVersionError, SetupUnavailable, subprocess.CalledProcessError) as error:
        print(f"sdk-fetch failed: {error}", file=sys.stderr)
        return 1
    return 0


def command_device(args: argparse.Namespace) -> None:
    from . import device

    if args.command == "probe":
        device.probe(args.environment, args.port, args.baud)
    elif args.command == "backup":
        device.backup(args.environment, args.port, args.output, args.baud)
    elif args.command == "monitor":
        device.monitor(args.environment, args.port, args.baud)
    elif args.command == "update":
        device.update_built(args.environment, args.port, args.baud, args.jobs)
    elif args.command in {"flash", "flash-monitor"}:
        device.flash_built(
            args.environment,
            args.port,
            args.baud,
            args.monitor_port,
            args.monitor_baud,
            args.jobs,
            args.monitor,
        )
    else:
        device.flash_existing(
            args.package,
            args.port,
            args.baud,
            args.monitor_port,
            args.monitor_baud,
            args.monitor,
        )


def _positive_baud(value: str) -> int:
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("baud rate must be positive")
    return result


def parser() -> argparse.ArgumentParser:
    from .host import SDK_NAMES

    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="list configured environments").set_defaults(
        handler=command_list
    )
    commands.add_parser("validate", help="validate the complete build catalog").set_defaults(
        handler=command_validate
    )

    build = commands.add_parser("build", help="build SDK-native firmware")
    build.add_argument("environment", nargs="*")
    build.add_argument("--all", action="store_true")
    build.add_argument("--matrix")
    build.add_argument("--board", action="append", default=[])
    build.add_argument("--chip")
    build.add_argument("-j", "--jobs", type=int)
    build.set_defaults(handler=command_build)

    clangd = commands.add_parser("clangd", help="generate and activate native compile commands")
    clangd.add_argument("environment")
    clangd.add_argument("-j", "--jobs", type=int)
    clangd.set_defaults(handler=command_clangd)

    setup = commands.add_parser("setup", help="install pinned SDK tools")
    selection = setup.add_mutually_exclusive_group(required=True)
    selection.add_argument("--platform", type=str.lower, choices=(*PLATFORMS, "all"))
    selection.add_argument("--env", metavar="NAME", nargs="+")
    selection.add_argument("--target", metavar="PLATFORM:SDK_TARGET", nargs="+")
    setup.add_argument("--ensure", action="store_true", help="install only if setup is incomplete")
    setup.add_argument("--dry-run", action="store_true", help="show commands without running them")
    setup.set_defaults(handler=command_setup)

    sdk = commands.add_parser("sdk-check", help="verify SDK pins")
    sdk.add_argument("sdk", nargs="*", metavar="SDK", help=", ".join(SDK_NAMES))
    sdk.add_argument("--json", action="store_true", help="write machine-readable pins")
    sdk.set_defaults(handler=command_sdk_check)

    checkout = commands.add_parser(
        "sdk-checkout", help="clone a pinned SDK into .sdk/ for SDK patch work"
    )
    checkout.add_argument("sdk", choices=SDK_NAMES, metavar="SDK", help=", ".join(SDK_NAMES))
    checkout.add_argument(
        "--dry-run", action="store_true", help="show commands without running them"
    )
    checkout.set_defaults(handler=command_sdk_checkout)

    fetch = commands.add_parser(
        "sdk-fetch", help="download every pinned SDK archive for offline use"
    )
    fetch.add_argument("sdk", nargs="*", metavar="SDK", help=", ".join(SDK_NAMES))
    fetch.add_argument("--dry-run", action="store_true", help="show commands without running them")
    fetch.set_defaults(handler=command_sdk_fetch)

    device_commands = {
        "probe": "inspect a target and physical flash",
        "backup": "read a complete physical flash backup",
        "flash": "build, probe, and flash an environment",
        "flash-monitor": "build, flash, and monitor an environment",
        "update": "build and flash only the application partition",
        "flash-only": "flash an existing package without compiling",
        "flash-only-monitor": "flash an existing package and monitor",
        "monitor": "open the target serial monitor",
    }
    for name, help_text in device_commands.items():
        command = commands.add_parser(name, help=help_text)
        command.set_defaults(handler=command_device, monitor=name.endswith("-monitor"))
        command.add_argument("--port", help="programming or monitor serial port")
        command.add_argument("--baud", type=_positive_baud, help="programming or monitor baud rate")
        if name.startswith("flash-only"):
            command.add_argument("--package", type=Path, required=True)
        else:
            command.add_argument("environment")
        if name == "backup":
            command.add_argument("--output", type=Path, required=True)
        if name in {"flash", "flash-monitor", "update"}:
            command.add_argument("--jobs", type=int)
        if name.startswith("flash"):
            command.add_argument("--monitor-port", help="monitor port; defaults to --port")
            command.add_argument(
                "--monitor-baud", type=_positive_baud, help="serial monitor baud rate"
            )
    return root


def main(argv: list[str] | None = None) -> int:
    from .host import SdkVersionError

    started = float(os.environ.get("NEWMOTA_MAKE_STARTED") or time.time())
    args = parser().parse_args(argv)
    try:
        return args.handler(args) or 0
    except KeyboardInterrupt:
        return 130
    except (BuildError, CatalogError, SetupUnavailable, SdkVersionError, ValueError) as error:
        print(f"{args.command} error: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"{args.command} I/O failed: {error}", file=sys.stderr)
        return 1 if args.command == "setup" else 2
    except subprocess.CalledProcessError as error:
        print(f"{args.command} command failed with exit code {error.returncode}", file=sys.stderr)
        return error.returncode
    finally:
        if args.command in TOTAL_TIME_COMMANDS:
            print(f"\nTotal elapsed: {format_elapsed(time.time() - started)}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
