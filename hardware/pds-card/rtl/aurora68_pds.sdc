# SPDX-License-Identifier: MIT
# The Pi's GPCLK0, 200 MHz; every slot input is synchronised to it.
create_clock -name PI_CLK -period 5.000 [get_ports PI_CLK]
# C8M (7.8336 MHz) clocks the small Interrupt-switch stock-mode logic, which must work with no Pi
create_clock -name C8M -period 127.655 [get_ports M68K_CLK]
set_clock_groups -asynchronous -group {PI_CLK} -group {C8M}
set_false_path -from [get_ports {M68K_* SLOT_PWR FAN_TACH PI_RD PI_WR PI_A*}]
