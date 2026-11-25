# SPDX-License-Identifier: MIT
# The Pi's GPCLK0, 200 MHz; every slot input is synchronised to it.
create_clock -name PI_CLK -period 5.000 [get_ports PI_CLK]
set_false_path -from [get_ports {M68K_* SLOT_PWR FAN_TACH STOCK_SW_n PI_RD PI_WR PI_A*}]
