#ifndef fooraopclientfoo
#define fooraopclientfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/mainloop-api.h>

typedef struct pa_raop_client pa_raop_client;

pa_raop_client* pa_raop_client_new(void);
void pa_raop_client_free(pa_raop_client* c);

int pa_raop_client_connect(pa_raop_client* c, pa_mainloop_api *mainloop, const char* host);

void pa_raop_client_disconnect(pa_raop_client* c);

void pa_raop_client_send_sample(pa_raop_client* c, const uint8_t* buffer, unsigned int count);

#endif
