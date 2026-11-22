-- plus_keys.lua: a keyboard reliability test for the Macintosh Plus testbed (macplus_ps -kbd usp, mac.board=plus).
-- SPDX-License-Identifier: MPL-2.0
-- Presses every key of the M0110A, fast, with chords and rollover, from frame PLUS_KEYS_START (default
-- 12000, the Finder is up), and prints each transition it makes as "[plus_keys] expect XX down|up" with
-- the ADB key code the kernel should hand the Mac. tools/plus_keys_check.py compares that with the
-- kernel's "[PSMAC:PLUS] keyboard: .. -> ADB key XX down|up" lines (run the kernel with mac.pluskeylog=400).
-- One Command chord, Command-T, which only flips Aurora68 Log's "Follow New Lines" (a snapshot either side).
local machine = manager.machine
local start = tonumber(os.getenv("PLUS_KEYS_START") or "12000")
local frames = 0
local caps = false
local fields = {}
local function field(name)
  if fields[name] ~= nil then return fields[name] end
  for tag, port in pairs(machine.ioport.ports) do
    if tag:find(":kbd", 1, true) then
      for fname, f in pairs(port.fields) do
        local first = fname:match("^(%S+)") or fname
        if fname == name or first == name then fields[name] = f return f end
      end
    end
  end
  print("[plus_keys] no key " .. name); fields[name] = false; return false
end
-- MAME field name -> ADB key code
local adb = {
  a=0x00, s=0x01, d=0x02, f=0x03, h=0x04, g=0x05, z=0x06, x=0x07, c=0x08, v=0x09, b=0x0B, q=0x0C,
  w=0x0D, e=0x0E, r=0x0F, y=0x10, t=0x11, ["1"]=0x12, ["2"]=0x13, ["3"]=0x14, ["4"]=0x15, ["6"]=0x16,
  ["5"]=0x17, ["="]=0x18, ["9"]=0x19, ["7"]=0x1A, ["-"]=0x1B, ["8"]=0x1C, ["0"]=0x1D, ["]"]=0x1E,
  o=0x1F, u=0x20, ["["]=0x21, i=0x22, p=0x23, Return=0x24, l=0x25, j=0x26, ["'"]=0x27, k=0x28,
  [";"]=0x29, ["\\"]=0x2A, [","]=0x2B, ["/"]=0x2C, n=0x2D, m=0x2E, ["."]=0x2F, Tab=0x30, Space=0x31,
  ["`"]=0x32, Backspace=0x33, Command=0x37, Shift=0x38, ["Caps Lock"]=0x39, Option=0x3A,
  ["Cursor Left"]=0x3B, ["Cursor Right"]=0x3C, ["Cursor Down"]=0x3D, ["Cursor Up"]=0x3E,
  ["Keypad ."]=0x41, ["Keypad *"]=0x43, ["Keypad +"]=0x45, ["Keypad Clear"]=0x47, ["Keypad /"]=0x4B,
  ["Keypad Enter"]=0x4C, ["Keypad -"]=0x4E, ["Keypad ="]=0x51, ["Keypad 0"]=0x52, ["Keypad 1"]=0x53,
  ["Keypad 2"]=0x54, ["Keypad 3"]=0x55, ["Keypad 4"]=0x56, ["Keypad 5"]=0x57, ["Keypad 6"]=0x58,
  ["Keypad 7"]=0x59, ["Keypad 8"]=0x5B, ["Keypad 9"]=0x5C,
}
local plan = {}     -- frame -> list of {name, value} (or {"snap", what})
local function ev(off, name, v) local f = start + off; plan[f] = plan[f] or {}; table.insert(plan[f], {name, v}) end
local function shot(off, what) ev(off, "snap", what) end
local t = 0
local function tap(name, hold, gap) ev(t, name, 1); ev(t + hold, name, 0); t = t + hold + gap end
-- 1. every letter, digit and punctuation key, quickly: 4 frames down, 4 up
local singles = {"a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z",
                 "1","2","3","4","5","6","7","8","9","0","-","=","[","]",";","'",",",".","/","\\","`","Space","Tab","Backspace"}
for _, k in ipairs(singles) do tap(k, 4, 4) end
t = t + 30; shot(t, "after the single keys (typed into the Filter field)"); t = t + 10
-- 2. faster still: 2 frames down, 2 up
for _, k in ipairs({"q","w","e","r","t","y"}) do tap(k, 2, 2) end
t = t + 30
-- 3. Shift chords: Shift down, three letters, Shift up
ev(t, "Shift", 1); t = t + 6; tap("a", 4, 4); tap("b", 4, 4); tap("c", 4, 4); ev(t, "Shift", 0); t = t + 30
shot(t, "after Shift-a b c (expect ABC at the end of the Filter text)"); t = t + 10
-- 3b. Command-T: Aurora68 Log's "Follow New Lines" (the checkbox flips)
shot(t, "before Command-T"); t = t + 10
ev(t, "Command", 1); t = t + 10; tap("t", 4, 4); ev(t, "Command", 0); t = t + 60
shot(t, "after Command-T (the Follow checkbox flipped)"); t = t + 10
-- 4. Option chord, Caps Lock locked and unlocked around two letters
ev(t, "Option", 1); t = t + 6; tap("e", 4, 4); ev(t, "Option", 0); t = t + 20
-- Caps Lock latches (MAME's PORT_TOGGLE, as the real key): pressed once it locks (down), pressed again it lets go (up)
ev(t, "Caps Lock", 1); ev(t + 4, "Caps Lock", 0); t = t + 14; tap("d", 4, 4); tap("f", 4, 4)
ev(t, "Caps Lock", 1); ev(t + 4, "Caps Lock", 0); t = t + 30
-- 5. rollover: the next key down before the last is up
ev(t, "j", 1); ev(t + 3, "k", 1); ev(t + 6, "j", 0); ev(t + 9, "l", 1); ev(t + 12, "k", 0); ev(t + 15, "l", 0); t = t + 45
-- 6. arrows and the keypad (keypad = / * + are sent with a shift the keyboard adds itself)
for _, k in ipairs({"Cursor Left","Cursor Right","Cursor Up","Cursor Down",
                    "Keypad 0","Keypad 1","Keypad 2","Keypad 3","Keypad 4","Keypad 5","Keypad 6","Keypad 7","Keypad 8","Keypad 9",
                    "Keypad .","Keypad Enter","Keypad Clear","Keypad -","Keypad +","Keypad *","Keypad /","Keypad ="}) do tap(k, 4, 6) end
t = t + 30
-- 7. Return last
tap("Return", 4, 4)
local last = start + t + 60

plus_keys_sub = emu.add_machine_frame_notifier(function()   -- a global: a local would be collected
  frames = frames + 1
  if frames == 1 then print(string.format("[plus_keys] start at frame %d, ends at %d", start, last)) end
  if frames % 600 == 0 then machine.video:snapshot() end
  local evs = plan[frames]
  if evs then
    for _, e in ipairs(evs) do
      if e[1] == "snap" then
        machine.video:snapshot(); print(string.format("[plus_keys] snap (frame %d): %s", frames, e[2]))
      else
        local f = field(e[1])
        if f then
          f:set_value(e[2])
          if e[1] == "Caps Lock" then          -- a latching key: each press flips it
            if e[2] == 1 then caps = not caps; print(string.format("[plus_keys] expect 39 %s (Caps Lock)", caps and "down" or "up")) end
          else
            print(string.format("[plus_keys] expect %02x %s (%s)", adb[e[1]] or 0xff, e[2] == 1 and "down" or "up", e[1]))
          end
        end
      end
    end
  end
  if frames == last then print("[plus_keys] done") end
end)
