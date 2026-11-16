local machine = manager.machine
local log = io.open("mame/timerate.txt", "w")
local frames, t0 = 0, os.time()
frame_sub = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames % 120 == 0 then log:write(string.format("wall %4ds  emulated %7.2fs  (frames %d)\n", os.time() - t0, machine.time.seconds + machine.time.attoseconds / 1e18, frames)) log:flush() end
end)
