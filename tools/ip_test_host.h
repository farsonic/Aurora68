/* SPDX-License-Identifier: MPL-2.0
 *
 * ip_test_host.h - what emu68/src/psmac/psmac_ip.c takes from the kernel (support.h, psmac.h, the
 * generic timer) when it is compiled on the host for tools/ip_test.c (-DPSMAC_IP_HOST). The clock
 * is the test's: a count of microseconds it moves on by itself.
 */
#ifndef IP_TEST_HOST_H
#define IP_TEST_HOST_H
#include <stdint.h>
extern uint64_t ip_test_ticks;
void ip_test_kprintf(const char *format, ...);
#define kprintf ip_test_kprintf
#define sprintf ip_test_sprintf
#endif
