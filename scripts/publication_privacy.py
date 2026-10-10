"""Sanitize tracked evidence and audit a publication tree without printing secrets."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SAFE_EMAIL = "contributors@example.invalid"
SAFE_AUTHOR = "UHMAS contributors"
RULES = {
    "windows_absolute_path": re.compile(r"(?i)\b[a-z]:[\\/]"),
    "personal_home_path": re.compile(r"/(?:home|Users)/[^/\s\"']{1,100}"),
    "drive_mounted_workspace": re.compile(r"/mnt/[a-z]/(?:project|Users)/"),
    "email": re.compile(r"(?<![\w.+-])[\w.+-]{1,100}@[\w.-]{1,100}\.[a-zA-Z]{2,24}"),
    "private_key": re.compile(r"-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----"),
    "provider_credential": re.compile(r"\b(?:gh[pousr]_[a-zA-Z0-9]{30,255}|github_pat_[a-zA-Z0-9_]{30,255}|sk-[a-zA-Z0-9_-]{32,255})\b"),
    "credential_assignment": re.compile(r"(?im)^\s*(?:password|passwd|api_key|api-key|secret|\u5bc6\u7801)\s*[=:]\s*[\"']?[^\s\"']{3,100}"),
}
TEXT_SUFFIXES = {".md", ".txt", ".json", ".ps1", ".py", ".sh", ".c", ".h", ".inc", ".toml", ".cmake"}


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *args])


def tracked_files(root: Path) -> list[Path]:
    return [root / n for n in git(root, "ls-files", "-z").decode("utf-8").split("\0") if n]


def matches(text: str) -> dict[str, int]:
    counts = {}
    for key, rx in RULES.items():
        hits = list(rx.finditer(text))
        if key == "email":
            hits = [h for h in hits if not h[0].lower().endswith(("@example.invalid", "@example.com"))]
        if hits:
            counts[key] = len(hits)
    if "\ufffd" in text:
        counts["replacement_character"] = text.count("\ufffd")
    for marker in ("\u951f\u65a4\u62f7", "\u00ef\u00bb\u00bf", "\u9428\u52ec", "\u951b\u5c7e"):
        if marker in text:
            counts["known_encoding_corruption"] = counts.get("known_encoding_corruption", 0) + text.count(marker)
    return counts


def audit(root: Path, *, tracked: bool = False, history: bool = False,
          identifiers: tuple[str, ...] = ()) -> dict:
    files = tracked_files(root) if tracked else sorted(
        f for f in root.rglob("*") if f.is_file() and ".git" not in f.relative_to(root).parts)
    findings = []
    for path in files:
        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            findings.append({"path": path.relative_to(root).as_posix(), "counts": {"non_utf8_or_binary": 1}})
            continue
        counts = matches(text)
        private_hits = sum(text.casefold().count(value.casefold())
                           for value in identifiers if len(value) >= 4)
        if private_hits:
            counts["source_personal_identifier"] = private_hits
        if raw.startswith(b"\xef\xbb\xbf"):
            counts["utf8_bom"] = 1
        if counts:
            findings.append({"path": path.relative_to(root).as_posix(), "counts": counts})
    history_info = {"checked": False}
    if history:
        commits = git(root, "log", "--all", "--format=%an%x00%ae%x00%cn%x00%ce").splitlines()
        identities = [line.decode("utf-8").split("\0") for line in commits]
        bad = sum(fields != [SAFE_AUTHOR, SAFE_EMAIL, SAFE_AUTHOR, SAFE_EMAIL] for fields in identities)
        messages = git(root, "log", "--all", "--format=%B").decode("utf-8", errors="strict")
        message_findings = matches(messages)
        # Inspect each reachable blob once, including historic deleted files.
        object_ids = [line.split(b" ", 1)[0] for line in git(root, "rev-list", "--objects", "--all").splitlines()]
        types = subprocess.check_output(["git", "-C", str(root), "cat-file", "--batch-check=%(objectname) %(objecttype)"], input=b"\n".join(object_ids)+b"\n")
        blobs = [line.split(b" ")[0] for line in types.splitlines() if line.endswith(b" blob")]
        blob_failures = 0
        for oid in blobs:
            raw = git(root, "cat-file", "blob", oid.decode("ascii"))
            try:
                blob_text = raw.decode("utf-8", errors="strict")
                blob_failures += bool(matches(blob_text) or raw.startswith(b"\xef\xbb\xbf") or
                                      any(value.casefold() in blob_text.casefold()
                                          for value in identifiers if len(value) >= 4))
            except UnicodeDecodeError:
                blob_failures += 1
        history_info = {"checked": True, "commit_count": len(commits),
                        "non_neutral_identity_commits": bad, "blob_count": len(blobs),
                        "blobs_with_findings": blob_failures,
                        "commit_message_findings": message_findings,
                        "has_remotes": bool(git(root, "remote").strip())}
    passed = not findings and not history_info.get("non_neutral_identity_commits", 0) and not history_info.get("blobs_with_findings", 0) and not history_info.get("commit_message_findings", {})
    return {"schema": "uhmas.publication-privacy.v1", "passed": passed,
            "scope": "tracked-worktree" if tracked else "publication-tree", "file_count": len(files),
            "findings": findings, "history": history_info,
            "limits": ["Pattern checks are not a proof that all conceivable personal identifiers are absent; publication uses an explicit file allowlist."]}


def redact_evidence_text(root: Path, text: str) -> str:
    native = str(root.resolve())
    forward = root.resolve().as_posix()
    roots = {native, forward, native.replace("\\", "\\\\")}
    if re.match(r"^[A-Za-z]:/", forward):
        roots.add("/mnt/"+forward[0].lower()+forward[2:])
    prefixes = sorted(roots, key=len, reverse=True)
    cleaned = text
    for prefix in prefixes:
        cleaned = cleaned.replace(prefix, "<PROJECT_ROOT>")
    # Generic user-home prefixes must not retain the account name.
    cleaned = re.sub(r"(?i)[a-z]:[\\/]+Users[\\/]+[^\\/\s\"']{1,100}", "<USER_HOME>", cleaned)
    return re.sub(r"/(?:home|Users)/[^/\s\"']{1,100}", "<USER_HOME>", cleaned)


def annotate_evidence_text(cleaned: str) -> str:
    document = json.loads(cleaned)
    metadata = {
        "version": 1, "local_paths_replaced": True,
        "original_numeric_measurements_preserved": True,
        "existing_hashes_refer_to_original_measurement_inputs": True,
        "usable_as_reference_only": True,
    }
    if not isinstance(document, dict):
        raise ValueError("Evidence redaction requires a JSON object")
    if not document:
        return json.dumps({"publication_redaction": metadata}, indent=2)+"\n"
    if "publication_redaction" in document:
        document["publication_redaction"] = metadata
        return json.dumps(document, indent=2, ensure_ascii=False)+"\n"
    # Insert only the new property. Do not reformat millions of historical
    # measurement lines or normalize their number representations.
    newline = "\r\n" if "\r\n" in cleaned else "\n"
    match = re.search(r'(?m)^([ \t]+)"', cleaned)
    indent = match[1] if match else "  "
    lines = json.dumps(metadata, indent=indent, ensure_ascii=False).splitlines()
    block = indent+'"publication_redaction": '+lines[0]+newline
    block += newline.join(indent+line for line in lines[1:])+","
    opening = cleaned.index("{")
    result = cleaned[:opening+1]+newline+block+cleaned[opening+1:]
    json.loads(result)
    return result


def sanitize_tracked_evidence(root: Path) -> dict:
    changed = []
    for path in tracked_files(root):
        if path.suffix.lower() != ".json" or "artifacts" not in path.relative_to(root).parts:
            continue
        raw = path.read_bytes()
        text = raw.decode("utf-8-sig", errors="strict")
        cleaned = redact_evidence_text(root, text)
        if cleaned == text and not raw.startswith(b"\xef\xbb\xbf"):
            continue
        # Do not rewrite original numeric evidence or provenance hashes. Flag
        # byte-level changes so old hashes are not mistaken for new hashes.
        path.write_bytes(annotate_evidence_text(cleaned).encode("utf-8"))
        changed.append({"path": path.relative_to(root).as_posix(),
                        "original_sha256": hashlib.sha256(raw).hexdigest(),
                        "sanitized_sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    return {"changed_files": len(changed), "files": changed}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--tracked", action="store_true")
    parser.add_argument("--history", action="store_true")
    parser.add_argument("--sanitize-evidence", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    result = sanitize_tracked_evidence(root) if args.sanitize_evidence else audit(root, tracked=args.tracked, history=args.history)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2, ensure_ascii=False)+"\n", encoding="utf-8")
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result.get("passed", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())
