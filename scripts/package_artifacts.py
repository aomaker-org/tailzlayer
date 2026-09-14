# Path: labs/tailzlayer/scripts/package_artifacts.py
# Purpose: Metadata manifest packager for extreme build artifacts.
# Max Column: 80 Columns (comments/docs) / 120 Chars (code)

import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys


def get_cmd_output(cmd):
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return res.stdout.strip()
    except Exception as e:
        return f"unknown ({e})"


def calculate_sha256(filepath):
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def package_build_artifacts(source_files, output_root="artifacts/builds"):
    git_hash = get_cmd_output(["git", "rev-parse", "HEAD"])
    if not git_hash or "unknown" in git_hash:
        git_hash = "uncommitted"

    git_branch = get_cmd_output(["git", "rev-parse", "--abbrev-ref", "HEAD"])
    compiler_info = get_cmd_output(["g++", "--version"]).splitlines()[0]
    timestamp = datetime.datetime.now(datetime.timezone.utc).isoformat()

    target_dir = os.path.join(output_root, git_hash[:12])
    os.makedirs(target_dir, exist_ok=True)

    manifest_artifacts = []
    for src in source_files:
        if os.path.exists(src):
            filename = os.path.basename(src)
            dest = os.path.join(target_dir, filename)
            shutil.copy2(src, dest)
            sha256_hash = calculate_sha256(dest)
            manifest_artifacts.append({
                "filename": filename,
                "sha256": sha256_hash,
                "size_bytes": os.path.getsize(dest),
            })

    manifest_data = {
        "git_commit_hash": git_hash,
        "git_branch": git_branch,
        "timestamp_utc": timestamp,
        "compiler": compiler_info,
        "artifacts": manifest_artifacts,
    }

    manifest_path = os.path.join(target_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest_data, f, indent=2)

    print(f"[+] Packaged {len(manifest_artifacts)} artifacts locked to commit {git_hash[:12]} at {target_dir}")
    print(f"    Manifest: {manifest_path}")


if __name__ == "__main__":
    files_to_package = sys.argv[1:] if len(sys.argv) > 1 else []
    package_build_artifacts(files_to_package)

# end of file: labs/tailzlayer/scripts/package_artifacts.py
