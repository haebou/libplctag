/***************************************************************************
 *   Copyright (C) 2017 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License as        *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/


#include <platform.h>
#include <util/attr.h>
#include <util/bytebuf.h>
#include <util/debug.h>
#include <util/hashtable.h>
#include <util/pt.h>
#include <util/refcount.h>
#include <util/resource.h>
#include <ab/ab.h>
#include <ab/logix.h>



#define PLC_RESOURCE_PREFIX "AB Logix PLC"


typedef struct logix_plc *logix_plc_p;

typedef struct {
    struct impl_tag_t base_tag;

    mutex_p mutex;

    int status;

    int read_requested;
    int write_requested;
    int abort_requested;

    const char *path;
    const char *name;
    int element_count;

    bytebuf_p data;

    protothread_p worker;
} logix_tag_t;

typedef logix_tag_t *logix_tag_p;

/*
We need to implement the following functions:

struct impl_vtable {
    int (*abort)(impl_tag_p tag);
    int (*get_size)(impl_tag_p tag);
    int (*get_status)(impl_tag_p tag);
    int (*start_read)(impl_tag_p tag);
    int (*start_write)(impl_tag_p tag);

    int (*get_int)(impl_tag_p tag, int offset, int size, int64_t *val);
    int (*set_int)(impl_tag_p tag, int offset, int size, int64_t val);
    int (*get_double)(impl_tag_p tag, int offset, int size, double *val);
    int (*set_double)(impl_tag_p tag, int offset, int size, double val);
};

*/



static PT_FUNC(setup_tag);
static void setup_tag_cleanup(void *tag_arg, int arg_count, void **args);
static void logix_tag_destroy(void *tag_arg, int arg_count, void **args);

//~ static rc_plc find_plc(attr attribs);
//~ static rc_plc create_plc(attr attribs);


static int abort_operation(impl_tag_p impl_tag);
static int get_size(impl_tag_p impl_tag);
static int get_status(impl_tag_p impl_tag);
static int start_read(impl_tag_p impl_tag);
static int start_write(impl_tag_p impl_tag);
static int get_int(impl_tag_p impl_tag, int offset, int size, int64_t *val);
static int set_int(impl_tag_p impl_tag, int offset, int size, int64_t val);
static int get_double(impl_tag_p impl_tag, int offset, int size, double *val);
static int set_double(impl_tag_p impl_tag, int offset, int size, double val);


static struct impl_vtable logix_vtable = { abort_operation, get_size, get_status, start_read, start_write,
                                           get_int, set_int, get_double, set_double };

impl_tag_p logix_tag_create(attr attribs)
{
    logix_tag_p tag = NULL;
    int rc = PLCTAG_STATUS_OK;
    const char *path;
    const char *name;
    const char *elems;

    /* FIXME */
    (void)attribs;

    pdebug(DEBUG_INFO,"Starting.");

    /* build the new tag. */
    tag = rc_alloc(sizeof(logix_tag_t), logix_tag_destroy, 0);
    if(!tag) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for new tag!");
        return NULL;
    }

    /* set up the vtable for this kind of tag. */
    tag->base_tag.vtable = &logix_vtable;
    tag->status = PLCTAG_STATUS_PENDING;

    rc = mutex_create(&tag->mutex);
    if(!rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to create mutex!");
        logix_tag_destroy(tag, 0, NULL);
        return NULL;
    }

    /* create a worker protothread to set up the rest of the tag. */
    path = str_dup(attr_get_str(attribs,"path","NONE"));
    name = str_dup(attr_get_str(attribs,"name",NULL));
    elems = str_dup(attr_get_str(attribs,"elem_count","1"));

    tag->worker = pt_create(setup_tag, 4, tag, path, name, elems);
    if(!tag->worker) {
        pdebug(DEBUG_ERROR,"Unable to start tag set up protothread!");
        logix_tag_destroy(tag, 0, NULL);
        return NULL;
    }

    /* add a clean up function to get rid of the extra arguments. */
    rc = rc_register_cleanup(tag->worker, setup_tag_cleanup, 3, path, name, elems);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to register cleanup function!");
        mem_free(path);
        mem_free(name);
        mem_free(elems);
        logix_tag_destroy(tag, 0, NULL);
        return NULL;
    }

    pdebug(DEBUG_INFO,"Done.");

    return (impl_tag_p)tag;
}





/***********************************************************************
 *********************** Implementation Functions **********************
 **********************************************************************/


PT_FUNC(setup_tag) {
    logix_tag_p tag = args[0];
    char *path = args[1];
    char *name = args[2];
    char *elem_count_str = args[3];

    (void)arg_count;

    PT_BODY
        pdebug(DEBUG_INFO,"Running");

        pdebug(DEBUG_INFO,"Got tag path %s", path);
        pdebug(DEBUG_INFO,"Got tag name %s", name);
        pdebug(DEBUG_INFO,"Got element count %s", elem_count_str);


        tag->status = PLCTAG_STATUS_OK;

    PT_END
}


void setup_tag_cleanup(void *tag_arg, int arg_count, void **args)
{
    pdebug(DEBUG_INFO,"Starting");

    (void)tag_arg;

    if(arg_count != 3) {
        pdebug(DEBUG_WARN, "Expected arg_count=3 but got %d", arg_count);
        return;
    }

    if(args[0]) {
        mem_free(args[0]);
    }

    if(args[1]) {
        mem_free(args[1]);
    }

    if(args[2]) {
        mem_free(args[2]);
    }

}


/***********************************************************************
 ************************ Helper Functions *****************************
 **********************************************************************/


void logix_tag_destroy(void *tag_arg, int arg_count, void **args)
{
    logix_tag_p tag = NULL;

    pdebug(DEBUG_INFO,"Starting.");

    (void)args;

    if(arg_count != 0) {
        pdebug(DEBUG_WARN,"Destructor called with no arguments or null arg pointer!");
        return;
    }

    tag = tag_arg;

    if(!tag) {
        pdebug(DEBUG_WARN,"Destructor called with null pointer!");
        return;
    }

    if(tag->mutex) {
        mutex_destroy(&tag->mutex);
    }

    if(tag->data) {
        bytebuf_destroy(tag->data);
    }

    if(tag->worker) {
        rc_dec(tag->worker);
    }

    pdebug(DEBUG_WARN,"Done.");
}




/***********************************************************************
 ******************** Tag Implementation Functions *********************
 **********************************************************************/

int abort_operation(impl_tag_p impl_tag)
{
    logix_tag_p tag = (logix_tag_p)impl_tag;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    tag->abort_requested = 1;

    return PLCTAG_STATUS_OK;
}

int get_size(impl_tag_p impl_tag)
{
    logix_tag_p tag = (logix_tag_p)impl_tag;
    int size = 0;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->data) {
        size = bytebuf_size(tag->data);
    }


    return size;
}


int get_status(impl_tag_p impl_tag) {
    logix_tag_p tag = (logix_tag_p)impl_tag;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    return tag->status;
}



int start_read(impl_tag_p impl_tag) {
    logix_tag_p tag = (logix_tag_p)impl_tag;
    int rc = PLCTAG_STATUS_OK;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = get_status(impl_tag);

    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->mutex) {
            tag->read_requested = 1;
            tag->status = PLCTAG_STATUS_PENDING;
        }

        return PLCTAG_STATUS_PENDING;
    } else if(rc == PLCTAG_STATUS_PENDING) {
        return PLCTAG_ERR_BUSY;
    }

    tag->read_requested = 1;

    return PLCTAG_STATUS_OK;
}



int start_write(impl_tag_p impl_tag) {
    logix_tag_p tag = (logix_tag_p)impl_tag;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    tag->write_requested = 1;

    return PLCTAG_STATUS_OK;
}




int get_int(impl_tag_p impl_tag, int offset, int size, int64_t *val) {
    logix_tag_p tag = (logix_tag_p)impl_tag;
    int rc = PLCTAG_STATUS_OK;

    (void)offset;
    (void)size;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *val = 1;

    return rc;
}

int set_int(impl_tag_p impl_tag, int offset, int size, int64_t val) {
    logix_tag_p tag = (logix_tag_p)impl_tag;
    int rc = PLCTAG_STATUS_OK;

    (void)offset;
    (void)size;
    (void)val;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    return rc;
}

int get_double(impl_tag_p impl_tag, int offset, int size, double *val) {
    logix_tag_p tag = (logix_tag_p)impl_tag;
    int rc = PLCTAG_STATUS_OK;

    (void)offset;
    (void)size;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *val = 1.0;

    return rc;
}

int set_double(impl_tag_p impl_tag, int offset, int size, double val) {
    logix_tag_p tag = (logix_tag_p)impl_tag;
    int rc = PLCTAG_STATUS_OK;

    (void)offset;
    (void)size;
    (void)val;

    if(!tag) {
        pdebug(DEBUG_WARN,"Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    return rc;
}



//~ rc_plc find_plc(attr attribs)
//~ {
    //~ const char *path = attr_get_str(attribs,"path","NONE");
    //~ rc_plc plc = resource_get(PLC_RESOURCE_PREFIX, path);

    //~ pdebug(DEBUG_INFO,"Starting with path %s", path);

    //~ if(!plc) {
        //~ pdebug(DEBUG_DETAIL,"Might need to create new PLC.");

        //~ /* better try to make a PLC. */
        //~ plc = create_plc(attribs);

        //~ /* FIXME - check the return code! */
        //~ if(resource_put(PLC_RESOURCE_PREFIX, path, plc) == PLCTAG_ERR_DUPLICATE) {
            //~ /* oops hit race condition, need to dump this, there is one already. */
            //~ pdebug(DEBUG_DETAIL,"Oops! Hit race condition and someone else created PLC already!");

            //~ rc_release(plc);
            //~ plc = resource_get(PLC_RESOURCE_PREFIX, path);
        //~ }
    //~ }

    //~ return plc;
//~ }


//~ rc_plc create_plc(attr attribs)
//~ {
    //~ rc_plc plc = NULL;

    //~ (void) attribs;

    //~ /* FIXME */

    //~ return plc;
//~ }