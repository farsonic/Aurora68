local n, frames = 0, 0
per_sub = emu.register_periodic(function() n = n + 1 end)
frame_sub = emu.add_machine_frame_notifier(function() frames = frames + 1 end)
stop_sub = emu.add_machine_stop_notifier(function() local f = io.open("mame/periodic_rate.txt","w") f:write(string.format("periodic calls %d over %d frames (%.0f per emulated second)\n", n, frames, n / (frames/60))) f:close() end)
