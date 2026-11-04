"""Exports every SWEP's view-model -> world-model and view-model -> fire-sound pairs
from the server's Lua.

usage: export_weapons.py <lua_root> <out_dir>
Writes <out_dir>/swep_models.json and <out_dir>/swep_sounds.json.

The demo only names a weapon's first-person model (SWEP.ViewModel); the model other players
see in its hands is SWEP.WorldModel and the shot sound is SWEP.Primary.Sound, defined in the
same script.
"""
import json
import pathlib
import re
import sys

PATTERN = r'SWEP\.%s\s*=\s*(?:Model\(|Sound\()?\s*"([^"]+)"'
VECTOR = r'SWEP\.%s\s*=\s*Vector\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)'
NUMBER = r'SWEP\.%s\s*=\s*([-\d.]+)'
# Bases derived from the firearm base (perpheads_gun_base: IronSights float slot 4, Passive
# bool slot 0, Reloading bool slot 1 in its SetupDataTables).
GUN_BASES = {"perpheads_gun_base", "perpheads_pistol_base", "perpheads_rifle_base",
             "perpheads_revolver_base", "perpheads_shotgun_base", "perpheads_sniper_base",
             "perpheads_nomag_base"}


def norm(path):
    path = path.replace("\\", "/").lower()
    return path[:-4] if path.endswith(".mdl") else path


def main(root, out_dir):
    models = {}
    sounds = {}
    info = {}
    for lua in pathlib.Path(root).rglob("*.lua"):
        text = lua.read_text(encoding="utf-8", errors="ignore")
        view = re.search(PATTERN % "ViewModel", text)
        if not view:
            continue
        key = norm(view.group(1))
        world = re.search(PATTERN % "WorldModel", text)
        if world:
            models.setdefault(key, norm(world.group(1)))
        shot = re.search(PATTERN % r"Primary\.Sound", text)
        if shot:
            sounds.setdefault(key, shot.group(1).lower())
        entry = {}
        base = re.search(PATTERN % "Base", text)
        if base:
            entry["base"] = base.group(1)
            entry["gun"] = base.group(1) in GUN_BASES
        name = re.search(PATTERN % "PrintName", text)
        if name and name.group(1):
            entry["name"] = name.group(1)
        for field, out_key in (("IronSightsPos", "ironPos"), ("IronSightsAng", "ironAng")):
            vector = re.search(VECTOR % field, text)
            if vector:
                entry[out_key] = [float(v) for v in vector.groups()]
        fov = re.search(NUMBER % "ViewModelFOV", text)
        if fov:
            entry["fov"] = float(fov.group(1))
        if re.search(r'SWEP\.ViewModelFlip\s*=\s*true', text):
            entry["flip"] = True
        if entry:
            if key not in info:
                info[key] = entry
            else:
                # Several SWEPs share this first-person model: keep only what they agree on,
                # so the viewer never names one weapon after another.
                known = info[key]
                for field in list(known):
                    if known[field] != entry.get(field):
                        del known[field]
                if known.get("base") is None:
                    for field in ("gun", "ironPos", "ironAng", "fov"):
                        known.pop(field, None)
    out = pathlib.Path(out_dir)
    (out / "swep_models.json").write_text(json.dumps(models, indent=1, sort_keys=True))
    (out / "swep_sounds.json").write_text(json.dumps(sounds, indent=1, sort_keys=True))
    (out / "swep_info.json").write_text(json.dumps(info, indent=1, sort_keys=True))
    print(len(models), "view/world pairs,", len(sounds), "view/sound pairs,", len(info), "infos")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

