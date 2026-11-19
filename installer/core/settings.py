"""The game's settings in args.txt on the console: read, and change single options while keeping
everything else (the player's own lines and comments)."""
from .paths import APP_DIR
from .report import InstallError

ARGS = f"{APP_DIR}/args.txt"

SETTINGS = {
    # key: (option, {menu value: args.txt value, None = remove the option}) or None for a number
    "fps_counter": ("--fastcars_native_fps", {"on": "true", "off": None}),
    "log": ("--log_level", {"error": "error", "info": "info", "debug": "debug"}),
    "fps_limit": ("--fastcars_fps_limit", None),
    "cpu_boost": ("--switch2_clocks", {"on": None, "off": "keep"}),
    # render resolution in percent: below 100 the 3D scene renders smaller and is upscaled
    "resolution": ("--fastcars_native_render_scale", None),
    "taa": ("--fastcars_native_taa", {"on": None, "off": "false"}),
}


def parse(text):
    """{option: value} for every --option=value in args.txt (comments ignored)."""
    found = {}
    for line in (text or "").splitlines():
        for word in line.split("#", 1)[0].split():
            if word.startswith("--") and "=" in word:
                k, v = word.split("=", 1)
                found[k] = v
    return found


def read_settings(text):
    found = parse(text)
    return {
        "fps_counter": "on" if found.get("--fastcars_native_fps") == "true" else "off",
        "log": found.get("--log_level", "error"),
        "fps_limit": found.get("--fastcars_fps_limit", "60"),
        "cpu_boost": "off" if found.get("--switch2_clocks") == "keep" else "on",
        "resolution": str(round(float(found.get("--fastcars_native_render_scale", "1")) * 100)),
        "taa": "off" if found.get("--fastcars_native_taa") == "false" else "on",
    }


def apply_setting(text, option, value):
    """Sets (or removes, value None) option in args.txt, keeping everything else."""
    out, done = [], False
    for line in text.splitlines():
        code, sep, comment = line.partition("#")
        words = code.split()
        if any(w.startswith(option + "=") for w in words):
            words = [w for w in words if not w.startswith(option + "=")]
            if value is not None and not done:
                words.append(f"{option}={value}")
                done = True
            if not words and not comment:
                continue
            line = " ".join(words) + (("  " + sep + comment) if sep else "")
        out.append(line)
    if value is not None and not done:
        insert = next((i for i, l in enumerate(out) if l.startswith("# Options")), len(out))
        out.insert(insert, f"{option}={value}")
    return "\n".join(out) + "\n"


# Settings renamed or removed by a release, applied to args.txt on the next install, oldest first:
# {"version": "v1.1.0", "rename": {"--old_option": "--new_option"}, "remove": ["--gone_option"]}
MIGRATIONS = []


def migrate(current, old_template, new_template, migrations=None):
    """args.txt after an update. Renames and removals from MIGRATIONS apply always. With the
    template the installer wrote last time (old_template), an option the player never changed
    (still its old template value) takes the new template's value, options the new template drops
    go when untouched, and options new in the template are added. The player's own changes and
    additions stay."""
    text = current
    for m in (MIGRATIONS if migrations is None else migrations):
        for old, new in (m.get("rename") or {}).items():
            found = parse(text)
            if old in found:
                text = apply_setting(text, old, None)
                if new not in found:
                    text = apply_setting(text, new, found[old])
        for opt in m.get("remove") or ():
            text = apply_setting(text, opt, None)
    if old_template is None or new_template is None:
        return text
    cur, old, new = parse(text), parse(old_template), parse(new_template)
    for opt, value in new.items():
        if opt in old:
            if cur.get(opt) == old[opt] and value != old[opt]:
                text = apply_setting(text, opt, value)
        elif opt not in cur:
            text = apply_setting(text, opt, value)
    for opt, value in old.items():
        if opt not in new and cur.get(opt) == value:
            text = apply_setting(text, opt, None)
    return text


def value_for(key, value):
    option, mapping = SETTINGS[key]
    if key == "resolution":
        p = int(value)
        if not 50 <= p <= 100:
            raise InstallError("Resolution: a percentage from 50 to 100")
        return option, f"{p / 100:g}"
    if mapping:
        if value not in mapping:
            raise InstallError(f"{key}: one of {', '.join(mapping)}")
        return option, mapping[value]
    return option, str(int(value))


def change(target, changes):
    """Applies {key: value} (None values are skipped) to args.txt on the console. Returns the
    settings as they are afterwards."""
    text = target.read_text(ARGS)
    if text is None:
        raise InstallError("No args.txt on the console: install first.")
    changed = False
    for key, value in changes.items():
        if value is None:
            continue
        option, v = value_for(key, value)
        text = apply_setting(text, option, v)
        changed = True
    if changed:
        target.write_text(ARGS, text)
    return read_settings(text)
