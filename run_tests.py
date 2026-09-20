#!/usr/bin/env python3
"""Run independent probe process groups, preserving failures and enforcing timeouts."""
import argparse
import datetime
import json
import os
import pathlib
import signal
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build"))
    parser.add_argument("--log-dir", type=pathlib.Path)
    parser.add_argument("--io-dir", type=pathlib.Path, required=True)
    parser.add_argument("--device", type=int, default=0, help="ACL logical device ID for host suite; device suite is fixed to HAL 0")
    parser.add_argument("--hal-device", type=int, default=0, help="HAL device ID for pure-HAL diagnostics")
    parser.add_argument("--sizes-mib", type=int, nargs="+", default=[2, 32])
    parser.add_argument("--suite", choices=["all", "host", "device", "hal"], default="all")
    parser.add_argument("--aio", choices=["both", "buffered", "direct", "none"], default="both")
    parser.add_argument("--timeout", type=int, default=120, help="seconds per case, including both processes")
    args = parser.parse_args()
    alignment_mib = 1 if args.suite == "device" else 2
    if args.timeout <= 0 or min(args.device, args.hal_device) < 0 or any(n <= 0 or n % alignment_mib for n in args.sizes_mib):
        parser.error(f"devices must be nonnegative; timeout positive; sizes positive multiples of {alignment_mib} MiB")
    if not args.io_dir.is_dir():
        parser.error("--io-dir must be an existing writable filesystem directory")
    if len(set(args.sizes_mib)) != len(args.sizes_mib):
        parser.error("duplicate sizes are not allowed")
    if args.suite in ("all", "device") and args.device != 0:
        parser.error("device suite is fixed to HAL device 0; use --suite host for other ACL devices")
    if args.suite in ("all", "device") and "ASCEND_RT_VISIBLE_DEVICES" in os.environ:
        parser.error("unset ASCEND_RT_VISIBLE_DEVICES; device suite uses HAL device 0")
    build = args.build_dir.resolve()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    logs = args.log_dir or pathlib.Path("logs") / f"{stamp}-{os.getpid()}"
    logs.mkdir(parents=True, exist_ok=False)
    cases = []
    for size in args.sizes_mib:
        common = ["--device", str(args.device), "--size-mib", str(size)]
        if args.suite in ("all", "host"):
            cases.append((f"host-{size}MiB", [str(build / "host_share"), *common, "--io-dir", str(args.io_dir.resolve()), "--aio", args.aio]))
        if args.suite in ("all", "device"):
            cases.append((f"device-{size}MiB", [str(build / "device_share"), "--size-mib", str(size)]))
    if args.suite in ("all", "hal"):
        for side in ("host", "device"):
            cases.append((f"hal-map-{side}", [str(build / "hal_map_probe"), side, str(args.hal_device), str(args.sizes_mib[0])]))
    metadata = {"date": stamp, "platform": os.uname()._asdict() if hasattr(os.uname(), "_asdict") else list(os.uname()),
                "ASCEND_RT_VISIBLE_DEVICES": os.environ.get("ASCEND_RT_VISIBLE_DEVICES"), "cases": []}
    with (logs / "environment.log").open("w") as output:
        for binary in sorted({cmd[0] for _, cmd in cases}):
            print(f"ldd {binary}", file=output, flush=True)
            subprocess.run(["ldd", binary], stdout=output, stderr=subprocess.STDOUT, check=False)
        subprocess.run(["findmnt", "-T", str(args.io_dir.resolve()), "-o", "TARGET,SOURCE,FSTYPE,OPTIONS"], stdout=output, stderr=subprocess.STDOUT, check=False)
        driver_info = pathlib.Path("/usr/local/Ascend/driver/version.info")
        if driver_info.is_file():
            output.write(driver_info.read_text())
    for name, command in cases:
        print(f"RUN {name}: {' '.join(command)}", flush=True)
        with (logs / f"{name}.log").open("w") as output:
            process = None
            try:
                process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                code = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                code = 124
                print("RUNNER_TIMEOUT process group killed", file=output)
            except KeyboardInterrupt:
                if process is not None:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                raise
            except OSError as error:
                code = 127
                print(f"RUNNER_ERROR {error}", file=output)
            finally:
                # An abnormal parent exit must not leave its importer running.
                if process is not None and process.poll() is not None:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        metadata["cases"].append({"name": name, "command": command, "returncode": code})
        (logs / "summary.json").write_text(json.dumps(metadata, indent=2))
        print(f"{'PASS' if code == 0 else 'FAIL'} {name} exit={code}; log={logs / (name + '.log')}", flush=True)
    return 1 if any(case["returncode"] for case in metadata["cases"]) else 0


if __name__ == "__main__":
    sys.exit(main())
