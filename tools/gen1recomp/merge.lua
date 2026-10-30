-- Merges what Pokemon Stadium changed on a cart back into a Gen1Recomp slot.
--
--   luajit <this file> <slot.lua> <before.sav> <after.sav> <out.lua> [game]
--
-- Run with the working directory at a pinned Gen1Recomp checkout (its src/
-- modules are required from ./) and its ROM cache on LUA_PATH, e.g.
--   LUA_PATH=%APPDATA%\pokemon-love2d\red\?.lua;;
--
-- before.sav is the cart exported from slot.lua before Stadium ran; after.sav
-- is the same cart after Stadium wrote to it. Both are decoded with
-- Gen1Recomp's own importer, so any difference between them is a change
-- Stadium made, not a conversion artifact. Only sections that differ are
-- copied into the slot; everything else (modData, flags, map state, meta, ...)
-- is left exactly as it was.
--
-- Pokemon keep their slot-only fields (e.g. the nuzlocke mod's nuzlockeDead,
-- nuzlockeId, catch data), matched by the nuzlocke mod's own fingerprint:
-- species plus DVs, plus OT ID. A Pokemon Stadium did not alter is kept
-- verbatim from the slot.
--
-- Based on Gen1Recomp's tools/save_convert/convert.lua.
-- Gen1Recomp: Copyright (C) 2026 BOIS CLUB GAMES, LLC, Pokemon Gen 1
-- Recompilation Project, GPLv3 with additional terms
-- (https://github.com/bryanthaboi/gen1recomp).

package.path = "./?.lua;" .. package.path

local SaveConvert = require("src.save_convert.SaveConvert")
local SaveSerializer = require("src.core.SaveSerializer")
local Version = require("src.core.Version")

-- Slot sections the cart models and Stadium can change.
local SECTIONS = { "party", "boxes", "currentBox", "inventory", "pcItems", "pokedex" }
local MON_SECTIONS = { party = true, boxes = true }

local function readFile(path, mode)
  local f = assert(io.open(path, mode or "r"), "cannot open " .. path)
  local content = f:read("*a")
  f:close()
  return content
end

local function deepEqual(a, b)
  if type(a) ~= type(b) then return false end
  if type(a) ~= "table" then return a == b end
  for k, v in pairs(a) do
    if not deepEqual(v, b[k]) then return false end
  end
  for k in pairs(b) do
    if a[k] == nil then return false end
  end
  return true
end

local function copy(v)
  if type(v) ~= "table" then return v end
  local t = {}
  for k, x in pairs(v) do t[k] = copy(x) end
  return t
end

local function fingerprint(mon)
  local d = mon.dvs or {}
  return table.concat({ tostring(mon.species), tostring(d.hp), tostring(d.attack),
    tostring(d.defense), tostring(d.speed), tostring(d.special), tostring(mon.otId) }, "|")
end

-- Every Pokemon in a save, party and boxes, in a stable order.
local function eachMon(save, fn)
  for _, mon in ipairs(save.party or {}) do fn(mon) end
  for b = 1, 12 do
    for _, mon in ipairs((save.boxes or {})[b] or {}) do fn(mon) end
  end
end

-- fingerprint -> list of mons (duplicates are possible, e.g. two caught twins)
local function indexMons(save)
  local index = {}
  eachMon(save, function(mon)
    local fp = fingerprint(mon)
    index[fp] = index[fp] or {}
    table.insert(index[fp], mon)
  end)
  return index
end

local function take(index, fp)
  local list = index[fp]
  if list and #list > 0 then return table.remove(list, 1) end
  return nil
end

local function importCart(path, game)
  local bytes = readFile(path, "rb")
  assert(#bytes == SaveConvert.SAVE_SIZE, path .. " is not a 32768-byte .sav")
  return assert(SaveConvert.importSav(bytes, Version.saveFormat, game))
end

local function main(slotPath, beforePath, afterPath, outPath, game)
  local slot = assert(SaveSerializer.decode(readFile(slotPath)))
  game = game or slot.version or "red"
  local before = importCart(beforePath, game)
  local after = importCart(afterPath, game)

  local slotMons = indexMons(slot)
  local beforeMons = indexMons(before)
  local report = {}

  -- Rebuild a mon from the after-cart: the slot's own table when Stadium left
  -- the cart data alone, otherwise the new cart data plus slot-only fields.
  local function resolve(mon)
    local fp = fingerprint(mon)
    local original = take(slotMons, fp)
    local beforeMon = take(beforeMons, fp)
    if original and beforeMon and deepEqual(beforeMon, mon) then
      return original
    end
    if not original then
      table.insert(report, ("  new %s (not in slot: traded or from Stadium)"):format(fp))
      return copy(mon)
    end
    table.insert(report, ("  changed %s"):format(fp))
    local merged = copy(mon)
    for k, v in pairs(original) do
      if merged[k] == nil and (beforeMon == nil or beforeMon[k] == nil) then
        merged[k] = copy(v)
      end
    end
    return merged
  end

  local changed = {}
  for _, section in ipairs(SECTIONS) do
    if not deepEqual(before[section], after[section]) then
      table.insert(changed, section)
      if MON_SECTIONS[section] then
        local value = {}
        if section == "party" then
          for i, mon in ipairs(after.party or {}) do value[i] = resolve(mon) end
        else
          for b = 1, 12 do
            local box = {}
            for i, mon in ipairs((after.boxes or {})[b] or {}) do box[i] = resolve(mon) end
            value[b] = box
          end
        end
        slot[section] = value
      else
        slot[section] = copy(after[section])
      end
    end
  end

  -- Pokemon that left the cart entirely (moved into Stadium's own storage).
  for fp, list in pairs(slotMons) do
    for _, mon in ipairs(list) do
      local inPartyOrBoxes = false
      for _, section in ipairs(changed) do
        if MON_SECTIONS[section] then inPartyOrBoxes = true end
      end
      if inPartyOrBoxes then
        table.insert(report, ("  left cart %s%s"):format(fp,
          mon.nuzlockeId and (" (" .. mon.nuzlockeId .. ")") or ""))
      end
    end
  end

  local out = assert(io.open(outPath, "wb"))
  out:write(SaveSerializer.encode(slot))
  out:close()

  print(("changed sections: %s"):format(#changed > 0 and table.concat(changed, ", ") or "none"))
  for _, line in ipairs(report) do print(line) end
  print("wrote " .. outPath)
end

-- `roundtrip <slot.lua> <out.lua>`: decode + re-encode only, to prove the
-- serializer reproduces a slot byte-for-byte before trusting a merge.
if arg[1] == "roundtrip" and arg[2] and arg[3] then
  local slot = assert(SaveSerializer.decode(readFile(arg[2])))
  local out = assert(io.open(arg[3], "wb"))
  out:write(SaveSerializer.encode(slot))
  out:close()
  print("wrote " .. arg[3])
elseif arg[1] and arg[2] and arg[3] and arg[4] then
  main(arg[1], arg[2], arg[3], arg[4], arg[5])
else
  io.stderr:write("usage: luajit merge.lua <slot.lua> <before.sav> <after.sav> <out.lua> [game]\n"
    .. "       luajit merge.lua roundtrip <slot.lua> <out.lua>\n")
  os.exit(1)
end
