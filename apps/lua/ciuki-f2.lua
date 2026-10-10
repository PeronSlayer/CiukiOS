-- SPDX-License-Identifier: MIT
-- Project supplement; upstream's unmodified _U suite is a separate mandatory run.
-- Byte offsets in the file case are zero based.
local failures = 0

local function case(name, run)
  local ok, detail = pcall(run)
  if not ok then
    failures = failures + 1
    -- Printable, bounded fields for the controller; no controller framing here.
    detail = "error=" .. tostring(detail):gsub("[^%w_.:/-]", "_"):sub(1, 160)
  end
  assert(io.stdout:write("case=", name, " ok=", ok and "1" or "0", " ", detail, "\n"))
  assert(io.stdout:flush())
end

case("file-roundtrip", function()
  local root = assert(os.getenv("TMPDIR"), "TMPDIR required")
  local path = root .. "/ciuki-f2-roundtrip.bin"
  local renamed = root .. "/ciuki-f2-renamed.bin"
  local bytes = {}
  for i = 0, 65535 do bytes[i + 1] = string.char(i % 251) end
  local original = table.concat(bytes)
  local f <close> = assert(io.open(path, "w+b"))
  assert(f:write(original))
  assert(f:flush())
  assert(f:seek("set", 8192) == 8192)
  assert(f:read(4096) == original:sub(8193, 12288))
  for i = 8192, 12287 do bytes[i + 1] = string.char(255 - i % 251) end
  local expected = table.concat(bytes)
  assert(f:seek("set", 8192) == 8192)
  assert(f:write(expected:sub(8193, 12288)))
  assert(f:close())
  local r <close> = assert(io.open(path, "rb"))
  local actual = assert(r:read("a"))
  assert(#actual == 65536)
  for i = 1, 65536 do assert(actual:byte(i) == expected:byte(i), "byte " .. (i - 1)) end
  assert(r:close())
  assert(os.rename(path, renamed))
  local moved <close> = assert(io.open(renamed, "rb"))
  assert(moved:read("a") == expected)
  assert(moved:close())
  assert(os.remove(renamed))
  assert(io.open(path, "rb") == nil and io.open(renamed, "rb") == nil)
  -- FNV-1a over the compared final bytes; controller also hashes captured output.
  local hash = 2166136261
  for i = 1, #actual do hash = ((hash ~ actual:byte(i)) * 16777619) & 0xffffffff end
  return string.format("bytes=65536 rewritten=4096 fnv1a32=%08x rename=1 remove=1", hash)
end)

case("allocation", function()
  local function value(cycle, index)
    local prefix = string.format("%03d:%04d:", cycle, index)
    return prefix .. string.rep(string.char(33 + (cycle + index) % 90), 128 - #prefix)
  end
  for cycle = 1, 100 do
    local live = {}
    for i = 1, 4096 do live[i] = value(cycle, i) end
    collectgarbage("collect")
    for i = 1, 4096 do assert(#live[i] == 128 and live[i] == value(cycle, i)) end
    live = nil
    collectgarbage("collect")
  end
  return "cycles=100 live_strings=4096 string_bytes=128 reference_errors=0"
end)

case("time-utc", function()
  local dates = {{2000, 2, 29, 951782400}, {2040, 1, 1, 2208988800}}
  for _, date in ipairs(dates) do
    local t = assert(os.time{year=date[1], month=date[2], day=date[3],
                            hour=0, min=0, sec=0, isdst=false})
    assert(t == date[4], "UTC epoch")
    local utc = assert(os.date("!*t", t))
    local localtime = assert(os.date("*t", t))
    for _, result in ipairs{utc, localtime} do
      assert(result.year == date[1] and result.month == date[2] and result.day == date[3])
      assert(result.hour == 0 and result.min == 0 and result.sec == 0)
      assert(os.time(result) == t)
    end
  end
  local clock = os.clock()
  assert(type(clock) == "number" and clock >= 0)
  return "dates=2 epoch_2000=951782400 epoch_2040=2208988800 clock_nonnegative=1"
end)

case("console", function()
  assert(io.stdout:write("ciuki-f2 stdout 1\n")); assert(io.stdout:flush())
  assert(io.stderr:write("ciuki-f2 stderr 2\n")); assert(io.stderr:flush())
  assert(io.stdout:write("ciuki-f2 stdout 3\n")); assert(io.stdout:flush())
  assert(io.stderr:write("ciuki-f2 stderr 4\n")); assert(io.stderr:flush())
  return "lines=4 ordered=1"
end)

os.exit(failures == 0 and 0 or 1, true)
