/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <assert.h>
#include <string.h>

#include "xmalloc.h"
#include "authkey-prop.h"
#include "props.h"
#include "log.h"

struct authkey_data {
    int ref;
    size_t length;
};

int pa_authkey_prop_get(struct pa_core *c, const char *name, void *data, size_t len) {
    struct authkey_data *a;
    assert(c && name && data && len > 0);
    
    if (!(a = pa_property_get(c, name)))
        return -1;

    assert(a->length == len);
    memcpy(data, a+1, len);
    return 0;
}

int pa_authkey_prop_put(struct pa_core *c, const char *name, const void *data, size_t len) {
    struct authkey_data *a;
    assert(c && name);

    if (pa_property_get(c, name))
        return -1;

    a = pa_xmalloc(sizeof(struct authkey_data) + len);
    a->ref = 1;
    a->length = len;
    memcpy(a+1, data, len);

    pa_property_set(c, name, a);
    
    return 0;
}

void pa_authkey_prop_ref(struct pa_core *c, const char *name) {
    struct authkey_data *a;
    assert(c && name);

    a = pa_property_get(c, name);
    assert(a && a->ref >= 1);

    a->ref++;
}

void pa_authkey_prop_unref(struct pa_core *c, const char *name) {
    struct authkey_data *a;
    assert(c && name);

    a = pa_property_get(c, name);
    assert(a && a->ref >= 1);

    if (!(--a->ref)) {
        pa_property_remove(c, name);
        pa_xfree(a);
    }
}


