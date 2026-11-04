"""Runs the Perpheads clothing definition Lua (unpacked from the demo's gluapack) and exports
the clothing-type registry as JSON: type index -> model, slots, skins (material remaps)...

Usage: python export_clothing.py <lua_root> <out.json>
"""
import json
import os
import re
import sys

import lupa

lua_root = sys.argv[1]
out_path = sys.argv[2]
base = os.path.join(lua_root, 'gamemodes', 'darkrp', 'gamemode', 'modules', 'clothing')


def glua(text: str) -> str:
    """Rewrite the GLua-only operators so stock Lua can load the file."""
    text = re.sub(r'!=', '~=', text)
    text = re.sub(r'&&', ' and ', text)
    text = re.sub(r'\|\|', ' or ', text)
    text = re.sub(r'(?m)(^|\s)//', r'\1--', text)
    text = re.sub(r'!(?!=)', ' not ', text)
    text = re.sub(r'\bcontinue\b', 'break', text)
    return text


def read(name: str) -> str:
    with open(os.path.join(base, name), encoding='utf-8') as handle:
        return glua(handle.read())


lua = lupa.LuaRuntime(unpack_returned_tuples=True)
lua.execute(
    """
SEX_MALE = 1
SEX_FEMALE = 0
function Color(r, g, b, a) return {r = r, g = g, b = b, a = a or 255} end
function Either(c, a, b) if c then return a else return b end end
function AddCSLuaFile() end
function T(s) return s end
local plain_insert = table.insert
function table.insert(t, a, b)
  if b == nil then t[#t + 1] = a; return #t end
  plain_insert(t, a, b); return a
end
function table.Count(t) local n = 0; for _ in pairs(t) do n = n + 1 end; return n end
function table.Add(a, b) for _, v in ipairs(b) do a[#a + 1] = v end; return a end
function table.HasValue(t, v) for _, x in pairs(t) do if x == v then return true end end return false end
Clothing = {}
"""
)

slots_text = read('sh_clothing.lua').split('IncludeSH("sh_bodyparts.lua")')[0]
lua.execute(slots_text)
lua.execute(read('sh_bodyparts.lua'))
lua.globals().include = lambda name: lua.execute(read(name))
lua.execute(read('sh_clothingtypes.lua'))

export = lua.eval(
    """
function()
  local result = {types = {}, bodyParts = {}, requiredSlots = {}}
  local function copy_list(t) local r = {} for _, v in ipairs(t or {}) do r[#r + 1] = v end return r end
  for index, t in pairs(Clothing.ClothingTypesByID) do
    local skins = {}
    for i, skin in ipairs(t.Skins or {}) do
      local subs = {}
      for k, v in pairs(skin.SubMaterials or {}) do subs[#subs + 1] = {k, v} end
      skins[i] = {name = skin.Name, canTint = skin.CanTint, subMaterials = subs}
    end
    local variants = {}
    if t.GetModel then
      for _, feet in ipairs({false, true}) do
        for _, thighs in ipairs({false, true}) do
          local missing = {}
          missing[Clothing.BODY_SLOT_FEET] = feet or nil
          missing[Clothing.BODY_SLOT_THIGHS] = thighs or nil
          variants[#variants + 1] = {feet = feet, thighs = thighs, model = t.GetModel(missing)}
        end
      end
    end
    result.types[#result.types + 1] = {
      index = index, identifier = t.Identifier or t.Name, model = t.Model, slots = copy_list(t.Slots),
      category = t.Category, sex = t.Sex, government = t.Government or false,
      accessory = t.IsAccessory or false, bodyPart = t.IsBodyPart or false, skins = skins,
      modelVariants = variants, excludedBodyParts = copy_list(t.ExcludedBodyParts),
      optionalBodyPartSlots = copy_list(t.OptionalBodyPartSlots),
      attachBodyPartsForSlots = t.AttchBodyPartsForSlots or false,
      dataNumberBodyGroup = t.DataNumberBodyGroup, dataNumberMaterialBase = t.DataNumberMaterialBase,
      name = t.Name,
    }
  end
  for slot, _ in pairs(Clothing.RequiredSlots) do result.requiredSlots[#result.requiredSlots + 1] = slot end
  return result
end
"""
)()


def convert(value):
    if lupa.lua_type(value) == 'table':
        keys = list(value.keys())
        if keys and all(isinstance(k, int) for k in keys) and sorted(keys) == list(range(1, len(keys) + 1)):
            return [convert(value[k]) for k in sorted(keys)]
        return {str(k): convert(value[k]) for k in keys}
    return value


data = convert(export)
data['types'].sort(key=lambda t: t['index'])
with open(out_path, 'w', encoding='utf-8') as handle:
    json.dump(data, handle, indent=1)
print('types', len(data['types']), 'required slots', data['requiredSlots'])



