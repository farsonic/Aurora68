/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/include/names.h - a song's number and title from its file's name, as both Plays tidy it.
 *
 * The server names each track from its tags once its album has been listed, and from the file's name before
 * that; the Live channel's queue and a command's tracks may name only files. Libraries name them
 * "01. Artist - Title.m4a", "01 Title.mp3", "1-01 Title.flac" (disc and track) or "Title.mp3". The rule, the
 * macOS Play's since 22 November 2026 (its regular expression ^\s*(?:\d{1,2}[-.])?(\d{1,3})(?:\s*[-.)_]\s*|\s+)(.+)$,
 * written out here so a 68000 needs no expression engine): an optional disc number of one or two digits and
 * '-' or '.', a track number of up to three digits, then a separator ('-', '.', ')' or '_' with spaces around
 * it, or spaces alone), then the title; and "Artist - " taken off the title's front when it is the album's
 * artist. A name that only starts with a number ("2001 A Space Odyssey") keeps it. Written on 22 November
 * 2026 (docs/08 item 107).
 */
#ifndef A68_NAMES_H
#define A68_NAMES_H

/* the stem of a path: its last part without the extension, into out (max bytes); its length */
long a68_stem(const char *path, char *out, long max);
/* the number (0 if none) and the title of a file's stem, the artist's name taken off the front when it is
   there; the title into out (max bytes) */
unsigned long a68_track_name(const char *stem, const char *artist, char *out, long max);
#endif
