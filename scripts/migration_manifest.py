import re
from pathlib import Path


FEATURE_ID_PATTERN = re.compile(r"^[A-Z]{3,5}-[0-9]{3}[A-Z]?$")


def read_utf8(path: Path, description: str) -> str:
    try:
        raw_contents = path.read_bytes()
    except OSError as error:
        raise RuntimeError(
            f"Unable to read {description} '{path}': {error}. Restore the file "
            "and retry."
        ) from error
    if raw_contents.startswith(b"\xef\xbb\xbf"):
        raise RuntimeError(
            f"{description.capitalize()} '{path}' must not contain a UTF-8 BOM."
        )
    try:
        return raw_contents.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise RuntimeError(
            f"{description.capitalize()} '{path}' is not valid UTF-8 at byte "
            f"{error.start}. Restore the file and retry."
        ) from error


def load_manifest_ids(path: Path) -> set[str]:
    contents = read_utf8(path, "capability manifest")
    ids = set(
        re.findall(
            r"^\| `([A-Z]{3,5}-[0-9]{3}[A-Z]?)` \|",
            contents,
            re.MULTILINE,
        )
    )
    if not ids:
        raise RuntimeError(
            f"Capability manifest '{path}' contains no stable IDs. Restore the "
            "manifest and retry."
        )
    return ids


def load_baseline_metadata(path: Path) -> dict[str, str]:
    contents = read_utf8(path, "migration baseline metadata")
    repositories = re.findall(r"^repository:\s*(\S+)\s*$", contents, re.MULTILINE)
    revisions = re.findall(r"^commit:\s*([0-9a-f]{40})\s*$", contents, re.MULTILINE)
    if len(repositories) != 1 or len(revisions) != 1:
        raise RuntimeError(
            f"{path} must contain exactly one repository and one 40-character "
            "lowercase commit entry. Restore the documented metadata and retry."
        )
    return {"repository": repositories[0], "revision": revisions[0]}
