/*
 * erlxsl.c
 *
 * -----------------------------------------------------------------------------
 * Copyright (c) 2008-2010 Tim Watson (watson.timothy@gmail.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * -----------------------------------------------------------------------------
 * Notes:
 *
 * This driver is intended for dynamic rather than static linking. The erlang
 * runtime should be started with the +A thread pool size option to ensure that
 * the driver can take advantage of the async driver apis.
 *
 * The runtime should also be started with a maximum stack space argument,
 * to ensure we don't run out of stack space prematurely!
 *
 */

// The erlxsl_driver header brings in the main erlxsl header and major pervasive includes
#include "erlxsl_driver.h"
#include "erlxsl_ei.h"

/* INTERNAL DATA & DATA STRUCTURES */

// often used constants
static const char* const unknown_command = "Unknown Command!";
static const char* const heap_space_exhausted = "Out of Memory!";
static const char* const transform_command = "transform";
static const char* const unsupported_response_type = "Unsupported Response Type.";

#define NUM_TYPE_HEADERS 3
#define NUM_SIZE_HEADERS 2
#define FIRST_BINV_ENTRY 1

/* DRIVER CALLBACK FUNCTIONS */

// Called by the emulator when the driver is starting.
static ErlDrvData
start_driver(ErlDrvPort port, char *buff) {
    atom_result = driver_mk_atom("result");
    atom_error    = driver_mk_atom("error");
    atom_log        = driver_mk_atom("log");

    // avoid total madness! nice tip that one...
    if (port == NULL) {
            return ERL_DRV_ERROR_GENERAL;
    }

    DriverHandle *d = ALLOC(sizeof(DriverHandle));
    if (d == NULL) {
        return ERL_DRV_ERROR_GENERAL; // TODO: use ERL_DRV_ERROR_ERRNO and provide out-of-memory info
    }
    d->port = (void*)port;
    d->logging_port = NULL;
    return (ErlDrvData)d;
};

// Called by the emulator when the driver is stopping.
static void
stop_driver(ErlDrvData drv_data) {
    // give the provider a chance to clean up
    DriverHandle *d = (DriverHandle*)drv_data;
    ErlDrvPort port = (ErlDrvPort)d->port;
    XslEngine *engine = d->engine;
    void *state = &port;
    INFO("provider handoff: shutdown\n");
    engine->shutdown(state);

    // unload engine/so_library
    INFO("unloading library %s\n", d->loader->name);
    dlclose((void*)d->loader->library);

    // driver cleanup
    driver_free(engine);
    driver_free(d->loader);
    driver_free(drv_data);
};

/*
This function is called from erlang:port_call/3. It works a lot like the control call-back,
but uses the external term format for input and output.
- command is an integer, obtained from the call from erlang (the second argument to erlang:port_call/3).
- buf and len provide the arguments to the call (the third argument to erlang:port_call/3). They're decoded using ei.
- rbuf points to a return buffer, rlen bytes long.

The return data (written to *rbuf) should be a valid erlang term in the external term format. This is converted to an
erlang term and returned by erlang:port_call/3 to the caller. If more space than rlen bytes is needed to return data,
*rbuf can be set to memory allocated with driver_alloc. This memory will be freed automatically after call has returned.
The return value (of this callback function) is the number of bytes returned in *rbuf. If ERL_DRV_ERROR_GENERAL is returned
(or in fact, anything < 0), erlang:port_call/3 will throw a BAD_ARG.

THIS IMPLEMENTATION of the callback handles two kinds of commands, INIT_COMMAND and ENGINE_COMMAND. An INIT_COMMAND should
only be issued once during the lifecycle of the driver, *before* any data is sent to the port using port_command/port_control.
The INIT_COMMAND causes the driver to load the specified shared library and call a predefined entry point (see the
erlxsl_driver header file for details) to initialize an XslEngine structure.

TODO: document ENGINE_COMMAND.
TODO: locking during ENGINE_COMMAND calls
TODO: support the transform command here as well - small binaries (which we can't/won't share/refcount) can be passed and copied...
*/
static int
call(ErlDrvData drv_data, unsigned int command, char *buf,
    int len, char **rbuf, int rlen, unsigned int *flags) {

    int i;
    int type;
    int size;
    int index = 0;
    int rindex = 0;
    /*int arity;
     *
    char cmd[MAXATOMLEN];*/
    char *data;
    DriverState state;
    DriverHandle *d = (DriverHandle*)drv_data;

    ei_decode_version(buf, &index, &i);
    if (command == INIT_COMMAND) {
        ei_get_type(buf, &index, &type, &size);
        INFO("ei_get_type %s of size = %i\n", ((char*)&type), size);
        // TODO: pull options tuple instead
        data = ALLOC(size + 1);
        ei_decode_string(buf, &index, data);
        INFO("Driver received data %s\n", data);
        state = init_provider(d, data);
    } else if (command == ENGINE_COMMAND) {
        DriverContext *ctx = ALLOC(sizeof(DriverContext));
        // ErlDrvPort port = (ErlDrvPort)d->port;
        // XslEngine *engine = (XslEngine*)d->engine;
        // ErlDrvTermData callee_pid = driver_caller(port);
        Command *cmd = init_command(NULL, ctx, NULL, init_iov(Text, 0, NULL));

        state = decode_ei_cmd(cmd, buf, &index);
        if (state == Success) {
            XslEngine *engine = (XslEngine*)d->engine;
            if (engine->command != NULL) {
                EngineState enstate = engine->command(cmd);
                if (enstate == Ok) {
                    state = Success;
                }
            }
        }
        /*ei_get_type(buf, &index, &type, &size);
        INFO("ei_get_type %s of size = %i\n", ((char*)&type), size);
        data = ALLOC(size + 1);
        ei_decode_string(buf, &index, data);*/
    } else {
        state = UnknownCommand;
    }

    ei_encode_version(*rbuf, &rindex);
    if (state == InitOk) {
        INFO("Provider configured with library %s\n", d->loader->name);
#ifdef _DRV_SASL_LOGGING
        // TODO: pull the logging_port and install it....
#endif
        ei_encode_atom(*rbuf, &rindex, "configured");
    } else if (state == Success) {
        ei_encode_tuple_header(*rbuf, &rindex, 2);
        ei_encode_atom(*rbuf, &rindex, "ok");
        if (state == OutOfMemory) {
            ei_encode_string(*rbuf, &rindex, heap_space_exhausted);
        } else if (state == UnknownCommand) {
            ei_encode_string(*rbuf, &rindex, unknown_command);
        } else {
            const char *err = (d->loader)->error_message;
            ei_encode_string_len(*rbuf, &rindex, err, strlen(err));
        }
    } else {
        ei_encode_tuple_header(*rbuf, &rindex, 2);
        ei_encode_atom(*rbuf, &rindex, "error");
        if (state == OutOfMemory) {
            ei_encode_string(*rbuf, &rindex, heap_space_exhausted);
        } else if (state == UnknownCommand) {
            ei_encode_string(*rbuf, &rindex, unknown_command);
        } else {
            const char *err = (d->loader)->error_message;
            ei_encode_string_len(*rbuf, &rindex, err, strlen(err));
        }
    }
    DRV_FREE(data);
    return(rindex);
};

/*
This function is called whenever the port is written to. The port should be in binary mode, see open_port/2.
The ErlIOVec contains both a SysIOVec, suitable for writev, and one or more binaries. If these binaries should be retained,
when the driver returns from outputv, they can be queued (using driver_enq_bin for instance), or if they are kept in a
static or global variable, the reference counter can be incremented.

THIS IMPLEMENTATION of the callback unpacks a set of headers from the input binary and constructs a Command object
which is then submitted to the XslEngine. When the emulator is running in SMP mode, the actual processing is done on
an async thread (using the driver_async submission mechanism) and the apply_transform function is used to wrap the
XslEngine callback functions. The results of processing are handled on a main emulator thread in the ready_async driver callback.
*/
static void
outputv(ErlDrvData drv_data, ErlIOVec *ev) {

    // char *error_msg;
    char *xml;
    char *xsl;
    char *data;

    DriverHandle *d = (DriverHandle*)drv_data;
    ErlDrvPort port = (ErlDrvPort)d->port;
    InputSpec *hspec;
    PayloadSize *hsize;

    XslTask *job;
    DriverContext *ctx;
    AsyncState *asd;
    DriverState state;
    ErlDrvTermData callee_pid = driver_caller(port);
    UInt8 *type1;
    UInt64 *size;

    if ((hspec = ALLOC(sizeof(InputSpec))) == NULL) {
        FAIL(port, "system_limit");
        return;
    }
    if ((hsize = (PayloadSize*)try_driver_alloc(port,
        sizeof(PayloadSize), hspec)) == NULL) return;

    DBG("sizeof(uint64_t): %lu \n", sizeof(UInt64));
    DBG("ev->vsize: %i \n", ev->vsize);

    // the first 8bit chunk holds the number of parameters
    type1 = ((UInt8*)ev->binv[1]->orig_bytes);
    hspec->param_grp_arity = *type1;

    // next two 8bit chunks hold the type specs
    type1++;
    hspec->input_kind = *type1;

    type1++;
    hspec->xsl_kind = *type1;

    // next two 64bit chunks hold the type specs
    type1++;
    size = ((UInt64*)type1);
    hsize->input_size = *size;

    size++;
    hsize->xsl_size = *size;

    // next comes the xml and xslt binaries, which may be in one of three places:
    // 1. if the XML binary is heap allocated, it'll be in the binv entry
    // 2. if the XML binary is not heap allocated, it'll be in the next binv entry
    // 3. if the XSL binary is not heap allocated, it'll be in the next binv entry
    //        otherwise it'll bin in the (following) SysIOVec

    // if there is enough space remaining in a binv entry for the input document,
    // we pull it from there. Otherwise, it'll be collapsed into the first binary.
    size_t pos = ((sizeof(UInt8) * NUM_TYPE_HEADERS) +
                                (sizeof(UInt64) * NUM_SIZE_HEADERS));
    // INFO("pos = %lu \n", pos);
    UInt8 bin_idx = FIRST_BINV_ENTRY;    // first entry is reserved

    if ((pos + hsize->input_size) <= ev->binv[bin_idx]->orig_size) {
        data = (char*)(&ev->binv[bin_idx]->orig_bytes[pos]);
    } else {
        data = ev->binv[++bin_idx]->orig_bytes;
    }
    // FIXME: find a way around NULL terminated strings and we can share the binary!
    xml = ALLOC(hsize->input_size + 1);
    xml[hsize->input_size] = '\0';
    strncpy(xml, data, hsize->input_size);

    if (hsize->xsl_size < 64) {
        data = &ev->iov[++bin_idx].iov_base[0];
    } else {
        data = ev->binv[++bin_idx]->orig_bytes;
    }
    xsl = ALLOC(hsize->xsl_size + 1);
    xsl[hsize->xsl_size] = '\0';
    strncpy(xsl, data, hsize->xsl_size);

    if ((job = (XslTask*)try_driver_alloc(port,
        sizeof(XslTask), xml, xsl, hsize, hspec)) == NULL) return;
    if ((ctx = (DriverContext*)try_driver_alloc(port,
        sizeof(DriverContext), xml, xsl, hsize, hspec, job)) == NULL) return;
    if ((asd = (AsyncState*)try_driver_alloc(port,
        sizeof(AsyncState), xml, xsl, hsize, hspec, job, ctx)) == NULL) return;

    ctx->port = port;
    ctx->caller_pid = callee_pid;
    asd->driver = d;
    if ((asd->command = init_command(transform_command, ctx, job, NULL)) == NULL) {
        free_async_state(asd);
        FAIL(port, "system_limit");
        return;
    }

    fprintf(stderr, "xml[spec: %lu, len:%lu]\n", (long unsigned int)hsize->input_size, strlen(xml));
    fprintf(stderr, "xsl[spec: %lu, len:%lu]\n", (long unsigned int)hsize->xsl_size, strlen(xsl));

    state = init_task(job, hsize, hspec, xml, xsl);
    switch (state) {
    case OutOfMemoryError:
        free_async_state(asd);
        FAIL(port, "system_limit");
        return;
    case Success:
        /*
        driver_async will call engine->transform passing command, then
        call ready_async followed by cleanup_task. The synchronous code
        works something like this:

        (*a->async_invoke)(a->async_data);
        if (async_ready(prt, a->async_data)) {
            if (a->async_free != NULL)
                (*a->async_free)(a->async_data);
        }
        */
        INFO("provider handoff: transform\n");
        driver_async(port, NULL, apply_transform, asd, NULL); //cleanup_task);
        break;
    default:    // TODO: it would be better if we didn't do "everthing else is an error" here
        // TODO: error!?
        break;
    }
};

/*
This function is called after an asynchronous call has completed. The asynchronous
call is started with driver_async. This function is called from the erlang emulator thread,
as opposed to the asynchronous function, which is called in some thread (if multithreading is enabled).

THIS IMPLEMENTATION of the callback, processes the supplied transform_result and sends it
to the appropriate erlang process (e.g. the specified receiver).
*/
static void
ready_async(ErlDrvData drv_data, ErlDrvThreadData data) {
    long response_len;
    ErlDrvTermData *term;

    DriverHandle *driver_handle = (DriverHandle*)drv_data;
    ErlDrvPort port = (ErlDrvPort)driver_handle->port;
    XslEngine *provider = (XslEngine*)driver_handle->engine;
    AsyncState *async_state = (AsyncState*)data;
    EngineState state = async_state->state;
    Command *command = async_state->command;
    ErlDrvTermData callee_pid = (ErlDrvTermData)(command->context)->caller_pid;
    ErlDrvTermData tag = (state == Ok) ? atom_result : atom_error;
    DriverIOVec* outv = command->result;

    if (state == OutOfMemoryError) {
        ERROR("Driver Out Of Memory!\n");
        free_async_state(async_state);
        FAIL(port, "system_limit");
        // this statement [above] will cause the driver to unload, so we may as well fail fast....
        return;
    }

    switch (outv->type) {
    case Text:
        term = make_driver_term(&port, outv->payload.buffer, &tag, &response_len);
        break;
    case Binary:
        term = make_driver_term_bin(&port, ((ErlDrvBinary*)outv->payload.data), &tag, &response_len);
        break;
    default:
        term = make_driver_term(&port, (char*)unsupported_response_type, &tag, &response_len);
        break;
    }

    if (term == NULL) {
        ERROR("Driver Out Of Memory!\n");
        // do a quick internal cleanup ?
        // free_async_state(asd);
        // give the XslEngine a chance to try and clean up!?
        // state = provider->after_transform(command);

        FAIL(port, "system_limit");
        // this statement [above] will cause the driver to unload, so we may as well fail fast....
        return;
    }

    INFO("Sending back response! \n");

    // TODO: use driver_output_term instead, passing the origin-PID in the term and use gen_server:reply to forward
    driver_send_term(port, callee_pid, term, response_len);

    // now the engine needs the opportunity to free up any intermediate structures
    INFO("provider handoff: after_transform\n");
    state = provider->after_transform(command);

    // internal cleanup time...
    free_async_state(async_state);
};

/* DRIVER API EXPORTS */

static ErlDrvEntry driver_entry = {
    NULL,               /* init */
    start_driver,       /* start, called when port is opened */
    stop_driver,        /* stop, called when port is closed */
    NULL,               /* output, called when port receives messages */
    NULL,               /* ready_input, called when input descriptor ready to read*/
    NULL,               /* ready_output, called when output descriptor ready to write */
    "erlxsl",           /* the name of the driver */
    NULL,               /* finish, called when unloaded */ //TODO: should we do resource disposal here!?
    NULL,               /* handle,    */
    NULL,               /* control */
    NULL,               /* timeout */
    outputv,            /* outputv */
    ready_async,        /* ready_async, called (from the emulator thread) after an asynchronous call has completed. */
    NULL,               /* flush */
    call,               /* call */
    NULL                /* event */
};

DRIVER_INIT(erlxsl_drv) {
    return &driver_entry;
}
