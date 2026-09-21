/*
 * ansi_view.h - the weather as a 24-bit ANSI screen, for callers who pick ANSI
 * (or have no TRACE). Same forecast, same keys, drawn in CP437 text.
 */
#ifndef ANSI_VIEW_H
#define ANSI_VIEW_H

#include "wx.h"

/* Runs the ANSI view until the caller quits. prefs is updated (and saved) as
 * they change place or units. Returns false if the caller hung up. */
bool ansi_view_run(WxUserPrefs *prefs);

#endif
