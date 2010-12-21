/* ``The contents of this file are copyright (C) ncorp llc,
 * Version 1.1, (the "License"); you may not use this file except in
 * compliance with the License. You should have received a copy of the
 * "License" along with this software. If not, it can be
 * retrieved via the world wide web at http://www.....
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 *
 *     $Id$
 */

#include <erlxsl.h>
#include <sablot.h>

/*
 * Contains pointers to the various chunks
 * of API data we need for post processing cleanup.
 */
typedef struct async_state {
    SablotSituation situation;
    SablotHandle sHandle;
    SDOM_Document xsl;
    SDOM_Document xml;
} AsyncState;

/* Walks the supplied linked list of params and adds them. */
static void addParametersToSituation(SablotSituation situation,
        SablotHandle sHandle, ParameterInfo* parameters) {
    
    while (NULL != parameters->next) {
        addParametersToSituation(situation, sHandle, parameters->next);
    }
    SablotAddParam(situation, sHandle, parameters->name, parameters->value);
}

DriverState init_provider() {
    return( Ok );
}

void handle_request(void* response) {
    DebugOut("Handling request...");
    //TODO: support for file based transforms as well!?
    TransformResponse* result = (TransformResponse*)response;
    DebugOut("Response ok...");
    RequestContext* context = result->context;
    DebugOut("Context ok...");
    TransformRequestPtr request = context->request;
    
    SablotSituation situation;
    SablotHandle sHandle;
    SDOM_Document xsl, xml;
    
    DebugOut("Parsing input data...");
    SablotCreateSituation(&situation);
    DebugOut("Situation built ok...");
    SablotParseBuffer(situation, request->inputData, &xml);
    DebugOut("Parsed xml data ok...");
    fprintf(stderr, "Stylesheet data: \n%s\n", request->stylesheetData);
    SablotParseStylesheetBuffer(situation, request->stylesheetData, &xsl);
    DebugOut("Parsed stylesheet ok...");
    
    DebugOut("Starting API calls...");
    SablotCreateProcessorForSituation(situation, &sHandle);
    
    SablotAddArgTree(situation, sHandle, "sheet", xsl);
    SablotAddArgTree(situation, sHandle, "data", xml);
    
    if (NULL != request->parameters) {
        DebugOut("Adding params...");
        addParametersToSituation(situation, sHandle, request->parameters);
    }
    
    DebugOut("Processing data...");
    SablotRunProcessorGen(situation, sHandle, "arg:/sheet", "arg:/data", "arg:/out");
    
    SablotGetResultArg(sHandle, "arg:/out", &(result->payload.buffer));
    
    result->responseFormat = Buffer;
    
    DebugOut("Building async state...");
    AsyncState* asyncState = malloc(sizeof(AsyncState));
    if (NULL == asyncState) {
        request->status = ProviderError;
        return;
    }
    
    asyncState->situation = situation;
    asyncState->sHandle = sHandle;
    asyncState->xml = xml;
    asyncState->xsl = xsl;
    
    result->externalData = asyncState;
    DebugOut("Done...");
}

DriverState post_handle_request(void* response) {
    TransformResponse* result = (TransformResponse*)response;
    DebugOut("Got result...");
    AsyncState* asyncState = result->externalData;
    if (NULL == asyncState) {
        //TODO: figure out what this means?
        DebugOut("WTF???");
        return( ProviderError );
    }
    
    DebugOut("Cleanup...");
    SablotSituation situation = asyncState->situation;
    SablotFree(result->payload.buffer);
    SablotDestroyDocument(situation, asyncState->xsl);
    SablotDestroyDocument(situation, asyncState->xml);
    SablotDestroyProcessor(asyncState->sHandle);
    SablotDestroySituation(situation);
    DebugOut("Finished cleaning...");
    return( Ok );
}

/*
 * Gives the implementation provider a change to cleanup.
 */
void destroy_provider() {}