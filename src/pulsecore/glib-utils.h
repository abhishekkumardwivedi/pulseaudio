#ifndef _foopulsecoreglibutilsh_
#define _foopulsecoreglibutilsh_

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

/* pa_gsource_for_thread_mq() returns a GSource object that can
 * be attached to a GMainLoop in order to dispatch a thread message queue.
 * Such a pa_thread_mq doesn't need to have to be attached to a pa_rtpoll
 * object. It also only dispatches messages in the 'in' side of the mq.
 */
GSource *pa_gsource_for_thread_mq(pa_thread_mq *mq, GMainLoop *mainloop);

#endif
