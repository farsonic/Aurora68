/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_files.h - the remote control's files and programs (psmac_files.c, docs/13 "Files and
 * applications"): the HTTP endpoints under /files/ and /apps, served through the Aurora68 Agent on
 * the Mac (mac/piagent, the card's agent block in psmb.h). psmac_remote.c owns the connections and
 * hands a request over here; everything runs on core 3 and nothing waits.
 */
#ifndef PSMAC_FILES_H
#define PSMAC_FILES_H
#include <stdint.h>

void psmac_files_init(uint8_t *window);          /* the agent's block made known to the Mac (always, remote on or off) */
void psmac_files_mac_reset(void);                /* the 68k was reset: any core; the job ends on core 3's next turn */
void psmac_files_poll(void);                     /* every turn of the remote's service */
int  psmac_files_busy(void);                     /* a job is under way: poll every turn */
/* A request for /files/... or /apps: 1 taken (the answer comes from psmac_files_produce), 0 refused with
   *code and msg (the caller answers with a JSON error). body/have: the bytes of the body already received
   with the headers; clen: Content-Length, -1 when there was none. */
int  psmac_files_request(int conn, const char *method, const char *path, const char *query,
                         const uint8_t *body, uint32_t have, int32_t clen, int *code, char *msg, unsigned msg_max);
uint32_t psmac_files_produce(int conn, uint8_t *buf, uint32_t room, int *finished);
uint32_t psmac_files_room(int conn);             /* body bytes the job can take now (an upload's flow control) */
void psmac_files_body(int conn, const uint8_t *data, uint32_t len);
void psmac_files_gone(int conn);                 /* the connection closed or was reset */
/* /screenshot?front=1: the application (Mac Roman name) to the front by the agent: 1 asked, 0 not possible;
   then the result: 0 waiting, 1 done, 2 it was in front already, -1 failed; prev the one that was in front */
int  psmac_files_front(const uint8_t *name, unsigned len);
int  psmac_files_front_result(uint8_t *prev, unsigned max);
void psmac_files_state_json(char *out, unsigned max);   /* the "agent" member of /state (1600 bytes are enough) */

#endif
