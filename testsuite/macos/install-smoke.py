#!/usr/bin/env python3
"""Check installed Slurm clients and local hardware discovery without daemons.

Usable on Darwin and Linux. Uses temporary configuration; it never submits jobs,
contacts a configured controller, or changes system configuration. Distributed
under Slurm's GNU GPL version 2 or later.
"""

import argparse
import json
import os
import platform
import re
import subprocess
import tempfile
from pathlib import Path

CLIENTS = (
    "sinfo squeue sacct sacctmgr scontrol sprio sdiag sstat sreport swait "
    "sbatch scancel sattach sbcast sshare strigger salloc srun"
).split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--plugin-dir", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    plugins = args.plugin_dir or prefix / "lib/slurm"
    results = {"platform": platform.platform(), "checks": [], "success": True}
    with tempfile.TemporaryDirectory(prefix="slurm-install-smoke-") as scratch:
        config = Path(scratch) / "slurm.conf"
        config.write_text(
            "ClusterName=install-smoke\nSlurmctldHost=localhost\n"
            f"PluginDir={plugins.resolve()}\n"
            "AuthType=auth/none\nCredType=cred/none\n"
            "ProctrackType=proctrack/pgid\nTaskPlugin=task/none\n"
            "NamespaceType=namespace/none\nAccountingStorageType=accounting_storage/slurmdbd\n"
            "SelectType=select/cons_tres\n"
            "NodeName=localhost CPUs=1 RealMemory=1 State=UNKNOWN\n"
            "PartitionName=test Nodes=localhost Default=YES State=UP\n"
        )
        # Do not inherit cluster/configless or user option overrides from a shell.
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("SLURM_", "SBATCH_", "SALLOC_", "SRUN_"))
        }
        env["SLURM_CONF"] = str(config)
        commands = [[prefix / "bin" / name, "--help"] for name in CLIENTS]
        commands += [[prefix / "sbin/slurmd", "-C"]]
        commands += [[prefix / "sbin/slurmctld", "-h"]]
        if (prefix / "sbin/slurmrestd").exists():
            commands.append([prefix / "sbin/slurmrestd", "--help"])
        for command in commands:
            record = {"argv": [str(item) for item in command]}
            try:
                proc = subprocess.run(
                    record["argv"],
                    env=env,
                    cwd=scratch,
                    capture_output=True,
                    text=True,
                    timeout=20,
                    check=False,
                )
                record.update(
                    returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr
                )
                passed = proc.returncode == 0 and bool(proc.stdout or proc.stderr)
                if command[-1] == "-C":
                    for field in ("CPUs", "RealMemory"):
                        match = re.search(rf"\b{field}=(\d+)", proc.stdout)
                        passed = passed and bool(match and int(match[1]) > 0)
                record["success"] = passed
            except (OSError, subprocess.TimeoutExpired) as error:
                record.update(success=False, error=str(error))
            results["checks"].append(record)
            results["success"] &= record["success"]
            print(f"{'PASS' if record['success'] else 'FAIL'} {command[0].name}")
    args.report.write_text(json.dumps(results, indent=2) + "\n")
    return 0 if results["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
