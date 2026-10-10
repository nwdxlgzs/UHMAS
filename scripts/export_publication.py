"""Export an audited source snapshot with a new, neutral Git history."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from publication_privacy import SAFE_AUTHOR, SAFE_EMAIL, audit, git, tracked_files

SOURCE_DIRS = {"src", "include", "tests", "scripts", "examples"}
SOURCE_SUFFIXES = {".c", ".h", ".inc", ".ps1", ".py", ".sh", ".json", ".cmake"}
ROOT_FILES = {".gitignore", "CMakeLists.txt", "PLAN.MD", "TODO.MD"}


def selected(relative: Path) -> bool:
    if relative.as_posix() in ROOT_FILES:
        return True
    if relative.parts[0] in SOURCE_DIRS:
        return relative.suffix.lower() in SOURCE_SUFFIXES
    return (relative.parent.as_posix() in {"artifacts/benchmarks/baselines", "artifacts/verification"}
            and relative.suffix.lower() == ".json")


def copy(source: Path, target: Path) -> None:
    if source.is_symlink():
        raise RuntimeError("Publication does not copy symbolic links")
    target.parent.mkdir(parents=True, exist_ok=True)
    # Byte copies preserve evidence hashes and strict UTF-8 documents.
    shutil.copyfile(source, target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, default=Path("draft/publish"))
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    output = (root / args.output).resolve()
    if output == root or root.is_relative_to(output):
        raise RuntimeError("Publication destination must not contain the development checkout")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise RuntimeError("Destination is not empty; choose a new --output directory")

    templates = root / "draft"
    if not (templates / "README.md").is_file():
        templates = root
    if not (templates / "README.md").is_file() or not (templates / "docs").is_dir():
        raise RuntimeError("Expected publication README and docs templates")
    paths = [path for path in tracked_files(root) if selected(path.relative_to(root))]
    # Nothing ignored or untracked is silently swept into a publication.
    if not paths or not any(path.name == "uhm.c" for path in paths):
        raise RuntimeError("Tracked source allowlist is incomplete")
    output.mkdir(parents=True, exist_ok=True)
    for path in paths:
        copy(path, output / path.relative_to(root))
    copy(templates / "README.md", output / "README.md")
    for path in sorted((templates / "docs").rglob("*.md")):
        copy(path, output / "docs" / path.relative_to(templates / "docs"))
    if (templates / "PUBLISH.md").is_file():
        copy(templates / "PUBLISH.md", output / "docs/PUBLISH.md")

    # Check local author names and account identifiers in memory; never put
    # their literal values into the report or new repository.
    identities = git(root, "log", "--all", "--format=%an%x00%ae%x00%cn%x00%ce").decode("utf-8").replace("\0", "\n").splitlines()
    identifiers = tuple(sorted({value for value in identities + [os.environ.get("USERNAME", "")]
                                if len(value) >= 4 and value not in {SAFE_AUTHOR, SAFE_EMAIL}}))
    before = audit(output, identifiers=identifiers)
    if not before["passed"]:
        print(json.dumps(before, indent=2, ensure_ascii=False))
        raise RuntimeError("Publication audit failed; no Git history was initialized")

    manifest = {
        "schema": "uhmas.publication-manifest.v1",
        "history": "new-single-neutral-commit",
        "license_added": False,
        "excluded": ["development .git", "build outputs", "ignored logs", "document recovery archives", "agent worktrees"],
        "files": [{"path": path.relative_to(output).as_posix(),
                   "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                  for path in sorted(output.rglob("*")) if path.is_file()],
        "privacy": before,
    }
    (output / "PUBLICATION.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False)+"\n", encoding="utf-8")
    # Do not inherit an author, signing identity, remote, hook or object store
    # from the development checkout or user's process environment.
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update({"GIT_AUTHOR_NAME": SAFE_AUTHOR, "GIT_AUTHOR_EMAIL": SAFE_EMAIL,
                "GIT_COMMITTER_NAME": SAFE_AUTHOR, "GIT_COMMITTER_EMAIL": SAFE_EMAIL,
                "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull})
    def fresh_git(*arguments: str) -> None:
        subprocess.run(["git", "-C", str(output), *arguments], env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    fresh_git("init", "--initial-branch=main", "--template=")
    fresh_git("config", "user.name", SAFE_AUTHOR)
    fresh_git("config", "user.email", SAFE_EMAIL)
    fresh_git("config", "commit.gpgsign", "false")
    fresh_git("config", "core.autocrlf", "false")
    fresh_git("add", "--all")
    fresh_git("commit", "-m", "Initial audited UHMAS source snapshot")
    result = audit(output, tracked=True, history=True, identifiers=identifiers)
    result["manifest_file_count"] = len(manifest["files"])
    result["clean_worktree"] = not bool(git(output, "status", "--porcelain").strip())
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result["passed"] and result["clean_worktree"] and not result["history"]["has_remotes"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
