"""Versions: the GitHub releases (tags without uploaded files), their order, and getting a
version's source (its tag's zip, unpacked into the app-data folder). See the plan, 2.3a."""
import io
import json
import os
import pathlib
import re
import shutil
import urllib.error
import urllib.request
import zipfile

from .paths import app_data_dir
from .report import InstallError

REPO = "itsPLK/fastcars-recomp-nx2"
API = os.environ.get("FASTCARS_RELEASES_API", f"https://api.github.com/repos/{REPO}/releases")
ZIP = os.environ.get("FASTCARS_ZIP_URL", f"https://codeload.github.com/{REPO}/zip/refs/tags/{{tag}}")
USER_AGENT = "fastcars-installer"

_SEMVER = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?")


def _split(version):
    """(major, minor, patch, pre-release or "") without a build commit ("-1a2b3c4d[-dirty]")."""
    m = _SEMVER.match(version or "")
    if not m:
        return None
    pre = m.group(4) or ""
    pre = re.sub(r"(^|-)[0-9a-f]{7,40}(-dirty)?$", "", pre)  # a build version's commit
    pre = re.sub(r"(^|-)dirty$", "", pre)
    return tuple(int(x) for x in m.groups()[:3]) + (pre,)


def parse(version):
    """Sort key for a version like "v1.1.0-rc1" (or a build version "v1.1.0-rc1-<commit>"), or
    None. A pre-release sorts before its release (v1.1.0-rc1 < v1.1.0), and numbers inside it
    compare as numbers (rc2 < rc10)."""
    parts = _split(version)
    if parts is None:
        return None
    major, minor, patch, pre = parts
    if not pre:
        return (major, minor, patch, 1, ())
    ids = tuple((0, int(t), "") if t.isdigit() else (1, 0, t.lower())
                for t in re.findall(r"\d+|[A-Za-z]+", pre))
    return (major, minor, patch, 0, ids)


def base_version(version):
    """"v1.1.0-rc1-1a2b3c4d" -> "v1.1.0-rc1" (the release part of a build version)."""
    parts = _split(version)
    if parts is None:
        return version
    major, minor, patch, pre = parts
    return f"v{major}.{minor}.{patch}" + (f"-{pre}" if pre else "")


def is_prerelease(version):
    parts = _split(version)
    return bool(parts and parts[3])


def fetch_releases(timeout=10):
    """[{tag, name, notes, prerelease, date}] newest first, drafts left out. Raises InstallError
    when GitHub can't be reached."""
    req = urllib.request.Request(API + "?per_page=100", headers={"User-Agent": USER_AGENT,
                                                                "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise InstallError(f"Could not get the list of versions from GitHub ({e}).")
    out = []
    for rel in data:
        tag = rel.get("tag_name") or ""
        if rel.get("draft") or parse(tag) is None:
            continue
        out.append({"tag": tag, "name": rel.get("name") or tag, "notes": rel.get("body") or "",
                    "prerelease": bool(rel.get("prerelease")) or is_prerelease(tag),
                    "date": (rel.get("published_at") or "")[:10]})
    out.sort(key=lambda r: parse(r["tag"]), reverse=True)
    return out


def sources_dir():
    return app_data_dir() / "sources"


def source_for(tag):
    """The unpacked source of a version (downloaded once). Returns its folder."""
    if not re.fullmatch(r"v[0-9A-Za-z.\-]+", tag):
        raise InstallError(f"Not a version: {tag}")
    dest = sources_dir() / tag
    if (dest / "fastcars.py").is_file():
        return dest
    url = ZIP.format(tag=tag)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
    except (urllib.error.URLError, OSError) as e:
        raise InstallError(f"Could not download {tag} ({e}).")
    tmp = sources_dir() / (tag + ".part")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in z.infolist():
            # GitHub's zips hold one top folder ("<repo>-<version>/"): strip it.
            parts = pathlib.PurePosixPath(info.filename).parts[1:]
            if not parts or any(p in ("..", "") for p in parts):
                continue
            target = tmp.joinpath(*parts)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)
            mode = (info.external_attr >> 16) & 0o777
            if mode & 0o111:
                os.chmod(target, mode)
    if not (tmp / "fastcars.py").is_file():
        shutil.rmtree(tmp, ignore_errors=True)
        raise InstallError(f"The download of {tag} doesn't look like this project's source.")
    shutil.rmtree(dest, ignore_errors=True)
    tmp.rename(dest)
    return dest
