/*
 * xsldrv.c
 * 
 * Implements an erts linked-in driver, exposing sablotron to erlang.
 *
 * Notes: 
 * 
 * This driver is intended for dynamic rather than static linking. The erlang
 * runtime should be started with a thread pool size option to ensure that 
 * the driver can take advantage of the async driver apis.
 *
 * The runtime should also be started with a maximum stack space argument,
 * to ensure we don't run out of stack space prematurely!
 *
 */

#include "protocol.h"

/* internal data structures */

typedef struct {
    ErlDrvPort port;
} driver_spec;

/* erts driver callbacks */

/*
static int driver_initialize() {
    //allow providers to initialize
    DriverState initialState = init_provider();
    if ( initialState == ProviderError ) {
        DebugOut("Driver init failed!");
        return -1;
    }
    return 0;
}*/

static ErlDrvData start_driver(ErlDrvPort port, char *buff) {
    
    // avoid total madness! nice tip that one...
    if (port == NULL) {
        return ERL_DRV_ERROR_GENERAL;
    }

    DriverState initialState = init_provider();
    if (initialState == ProviderError) {
        DebugOut("Provider failed to initialize");
        return ERL_DRV_ERROR_GENERAL;
    }
    
    // configure erts
    driver_spec* d = (driver_spec*)driver_alloc(sizeof(driver_spec));
    d->port = port;    
    
    // respond with driver_spec
    return (ErlDrvData)d;
}

static void stop_driver(ErlDrvData handle) {

    // give the provider a chance to clean up
    destroy_provider();
    // cleanup for erts
    driver_free((char*)handle);
}

/* 
 *  This output handler function is called from erlang with 
 *  erlang:port_command(Port, Payload), where the payload looks like: 
 *  
 */
static void output(ErlDrvData handle, char *buff, int bufflen) {
    driver_spec* globalDriverSpecData;
    ErlDrvPort port;
    
    globalDriverSpecData = (driver_spec*)handle;
    port = globalDriverSpecData->port;
    
#ifdef DEBUG
    fprintf(stderr, "Driver received buffer of size %i\n", ((char *)&bufflen));
#endif
    
    ErlDrvTermData callerPid = driver_caller(port);
    
    RequestContext* context;
    context = driver_alloc(sizeof(RequestContext));
    if (NULL == context) {
        E_OUT_OF_MEMORY;
        return;
    }
    
    context->port = port;
    context->callerPid = callerPid;
    TransformRequestPtr request = unpackRequest((const char *)buff, bufflen);
    if (NULL == request) { //NB: this only happens when the heap is exhausted!
        releaseContext(context);
        E_OUT_OF_MEMORY;
        return;
    }
    
    context->request = request;    
    TransformResponse* response = malloc(sizeof(TransformResponse));
    if (NULL == response) {
        releaseContext(context);
        E_OUT_OF_MEMORY;
        return;
    }
    
    response->context = context;
    long taskHandle = driver_async(port, NULL, handle_request, response, NULL);
}

static void ready_async(ErlDrvData drv_data, ErlDrvThreadData async_data) {
    driver_spec* driverBundle = (driver_spec*)drv_data;
    ErlDrvPort port = driverBundle->port;
    
    TransformResponse* response = (TransformResponse*)async_data;
    sendResponse(response, NULL);    

    post_handle_request(response);
    releaseResponse(response);
}

static ErlDrvEntry driver_entry = {
    NULL,                       /* init */
    start_driver,               /* start, called when port is opened */
    stop_driver,                /* stop, called when port is closed */
    output,                     /* output, called when port receives messages */
    NULL,                       /* ready_input, called when input descriptor ready to read*/
    NULL,                       /* ready_output, called when output descriptor ready to write */
    "xsldrv",                   /* the name of the driver */
    NULL,                       /* finish, called when unloaded */ //TODO: should we do resource disposal here!?
    NULL,                       /* handle,  */
    NULL,                       /* control */
    NULL,                       /* timeout */
    NULL,                       /* outputv */
    ready_async,                /* ready_async, called (from the emulator thread) after an asynchronous call has completed. */
    NULL,                       /* flush */
    NULL,                       /* call */
    NULL                        /* event */
};

//ErlDrvEntry driver_entry = {
//    NULL,                           /* F_PTR init, N/A */
//    start_driver,                   /* L_PTR start, called when port is opened */
//    stop_driver,                    /* F_PTR stop, called when port is closed */
//    output,                         /* F_PTR output, called when port receives messages */
//    NULL,                           /* F_PTR ready_input, called when input descriptor ready to read*/
//    NULL,                           /* F_PTR ready_output, called when output descriptor ready to write */
//    "xsldrv",                       /* char *driver_name, the argument to open_port */
//    NULL,                           /* F_PTR finish, called when unloaded */
//    NULL,                           /* F_PTR control, port_command callback */
//    NULL,                           /* F_PTR timeout, reserved */
//    NULL                            /* F_PTR outputv, reserved */
//};

DRIVER_INIT(xsldrv) {
    return &driver_entry;
}