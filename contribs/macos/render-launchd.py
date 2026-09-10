#!/usr/bin/env python3
"""Render launchd examples. This does not install or load services.

Distributed under Slurm's GNU GPL version 2 or later.
"""

import argparse
import plistlib
from pathlib import Path


def substitute(value, replacements):
    if isinstance(value, str):
        for key, replacement in replacements.items():
            value = value.replace(f"@{key}@", replacement)
        return value
    if isinstance(value, list):
        return [substitute(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: substitute(item, replacements) for key, item in value.items()}
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--config-dir", type=Path, required=True)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--slurm-user", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    replacements = {
        "PREFIX": str(args.prefix.resolve()),
        "CONFIG_DIR": str(args.config_dir.resolve()),
        "STATE_DIR": str(args.state_dir.resolve()),
        "SLURM_USER": args.slurm_user,
    }
    if not args.slurm_user or any(char.isspace() for char in args.slurm_user):
        parser.error("--slurm-user must be a nonempty account name")
    templates = sorted(Path(__file__).parent.glob("org.slurm.*.plist.in"))
    if len(templates) != 3:
        parser.error("expected the three Slurm plist templates beside this script")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    outputs = [args.output_dir / path.stem for path in templates]
    if any(path.exists() for path in outputs):
        parser.error("output plists already exist; use a new output directory")
    for template, output in zip(templates, outputs):
        config = substitute(plistlib.loads(template.read_bytes()), replacements)
        with output.open("xb") as stream:
            plistlib.dump(config, stream, sort_keys=False)
        print(output)


if __name__ == "__main__":
    main()
