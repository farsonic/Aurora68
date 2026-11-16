local machine = manager.machine
local frames = 0
frame_sub = emu.add_machine_frame_notifier(function() frames = frames + 1 if frames % 600 == 0 then machine.video:snapshot() end end)
