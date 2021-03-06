/*
    httpService.c -- Http service. Includes timer for expired requests.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Locals ************************************/
/**
    Standard HTTP error code table
 */
typedef struct HttpStatusCode {
    int     code;                           /**< Http error code */
    char    *codeString;                    /**< Code as a string (for hashing) */
    char    *msg;                           /**< Error message */
} HttpStatusCode;


PUBLIC HttpStatusCode HttpStatusCodes[] = {
    { 100, "100", "Continue" },
    { 101, "101", "Switching Protocols" },
    { 200, "200", "OK" },
    { 201, "201", "Created" },
    { 202, "202", "Accepted" },
    { 204, "204", "No Content" },
    { 205, "205", "Reset Content" },
    { 206, "206", "Partial Content" },
    { 301, "301", "Moved Permanently" },
    { 302, "302", "Moved Temporarily" },
    { 304, "304", "Not Modified" },
    { 305, "305", "Use Proxy" },
    { 307, "307", "Temporary Redirect" },
    { 400, "400", "Bad Request" },
    { 401, "401", "Unauthorized" },
    { 402, "402", "Payment Required" },
    { 403, "403", "Forbidden" },
    { 404, "404", "Not Found" },
    { 405, "405", "Method Not Allowed" },
    { 406, "406", "Not Acceptable" },
    { 408, "408", "Request Timeout" },
    { 409, "409", "Conflict" },
    { 410, "410", "Gone" },
    { 411, "411", "Length Required" },
    { 412, "412", "Precondition Failed" },
    { 413, "413", "Request Entity Too Large" },
    { 414, "414", "Request-URI Too Large" },
    { 415, "415", "Unsupported Media Type" },
    { 416, "416", "Requested Range Not Satisfiable" },
    { 417, "417", "Expectation Failed" },
    { 500, "500", "Internal Server Error" },
    { 501, "501", "Not Implemented" },
    { 502, "502", "Bad Gateway" },
    { 503, "503", "Service Unavailable" },
    { 504, "504", "Gateway Timeout" },
    { 505, "505", "Http Version Not Supported" },
    { 507, "507", "Insufficient Storage" },

    /*
        Proprietary codes (used internally) when connection to client is severed
     */
    { 550, "550", "Comms Error" },
    { 551, "551", "General Client Error" },
    { 0,   0 }
};

/****************************** Forward Declarations **************************/

static void httpTimer(Http *http, MprEvent *event);
static bool isIdle();
static void manageHttp(Http *http, int flags);
static void terminateHttp(int how, int status);
static void updateCurrentDate(Http *http);

/*********************************** Code *************************************/

PUBLIC Http *httpCreate(int flags)
{
    Http            *http;
    HttpStatusCode  *code;

    mprGlobalLock();
    if (MPR->httpService) {
        mprGlobalUnlock();
        return MPR->httpService;
    }
    if ((http = mprAllocObj(Http, manageHttp)) == 0) {
        mprGlobalUnlock();
        return 0;
    }
    MPR->httpService = http;
    http->software = sclone(BIT_HTTP_SOFTWARE);
    http->protocol = sclone("HTTP/1.1");
    http->mutex = mprCreateLock();
    http->stages = mprCreateHash(-1, 0);
    http->hosts = mprCreateList(-1, MPR_LIST_STATIC_VALUES);
    http->connections = mprCreateList(-1, MPR_LIST_STATIC_VALUES);
    http->authTypes = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_UNIQUE);
    http->authStores = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_UNIQUE);
    http->booted = mprGetTime();
    http->flags = flags;
    http->monitorMaxPeriod = 0;
    http->monitorMinPeriod = MAXINT;
    http->secret = mprGetRandomString(HTTP_MAX_SECRET);

    updateCurrentDate(http);
    http->statusCodes = mprCreateHash(41, MPR_HASH_STATIC_VALUES | MPR_HASH_STATIC_KEYS);
    for (code = HttpStatusCodes; code->code; code++) {
        mprAddKey(http->statusCodes, code->codeString, code);
    }
    httpInitAuth(http);
    httpOpenNetConnector(http);
    httpOpenSendConnector(http);
    httpOpenRangeFilter(http);
    httpOpenChunkFilter(http);
#if BIT_HTTP_WEB_SOCKETS
    httpOpenWebSockFilter(http);
#endif

    mprSetIdleCallback(isIdle);
    mprAddTerminator(terminateHttp);

    if (flags & HTTP_SERVER_SIDE) {
        http->endpoints = mprCreateList(-1, MPR_LIST_STATIC_VALUES);
        http->routeTargets = mprCreateHash(-1, MPR_HASH_STATIC_VALUES);
        http->routeConditions = mprCreateHash(-1, MPR_HASH_STATIC_VALUES);
        http->routeUpdates = mprCreateHash(-1, MPR_HASH_STATIC_VALUES);
        http->sessionCache = mprCreateCache(MPR_CACHE_SHARED);
        http->counters = mprCreateList(-1, 0);
        http->monitors = mprCreateList(-1, 0);
        http->addresses = mprCreateHash(-1, 0);
        http->defenses = mprCreateHash(-1, 0);
        http->remedies = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_STATIC_VALUES);
        httpOpenUploadFilter(http);
        httpOpenCacheHandler(http);
        httpOpenPassHandler(http);
        httpOpenActionHandler(http);
        http->serverLimits = httpCreateLimits(1);
        httpDefineRouteBuiltins();
        httpAddCounters();
        httpAddRemedies();
    }
    if (flags & HTTP_CLIENT_SIDE) {
        http->defaultClientHost = sclone("127.0.0.1");
        http->defaultClientPort = 80;
        http->clientLimits = httpCreateLimits(0);
        http->clientRoute = httpCreateConfiguredRoute(0, 0);
        http->clientHandler = httpCreateHandler(http, "client", 0);
    }
    mprGlobalUnlock();
    return http;
}


static void manageHttp(Http *http, int flags)
{
    HttpConn    *conn;
    int         next;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(http->endpoints);
        mprMark(http->hosts);
        mprMark(http->connections);
        mprMark(http->stages);
        mprMark(http->statusCodes);
        mprMark(http->routeTargets);
        mprMark(http->routeConditions);
        mprMark(http->routeUpdates);
        mprMark(http->sessionCache);
        /* Don't mark convenience stage references as they will be in http->stages */

        mprMark(http->clientLimits);
        mprMark(http->serverLimits);
        mprMark(http->clientRoute);
        mprMark(http->clientHandler);
        mprMark(http->timer);
        mprMark(http->timestamp);
        mprMark(http->mutex);
        mprMark(http->software);
        mprMark(http->forkData);
        mprMark(http->context);
        mprMark(http->currentDate);
        mprMark(http->secret);
        mprMark(http->defaultClientHost);
        mprMark(http->protocol);
        mprMark(http->proxyHost);
        mprMark(http->authTypes);
        mprMark(http->authStores);
        mprMark(http->addresses);
        mprMark(http->defenses);
        mprMark(http->remedies);
        mprMark(http->monitors);
        mprMark(http->counters);
        mprMark(http->dateCache);

        /*
            Endpoints keep connections alive until a timeout. Keep marking even if no other references.
         */
        lock(http->connections);
        for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
            if (conn->endpoint) {
                mprMark(conn);
            }
        }
        unlock(http->connections);
    }
}


PUBLIC void httpDestroy(Http *http)
{
    if (http->timer) {
        mprRemoveEvent(http->timer);
        http->timer = 0;
    }
    if (http->timestamp) {
        mprRemoveEvent(http->timestamp);
        http->timestamp = 0;
    }
    MPR->httpService = NULL;
}


PUBLIC void httpAddEndpoint(Http *http, HttpEndpoint *endpoint)
{
    mprAddItem(http->endpoints, endpoint);
}


PUBLIC void httpRemoveEndpoint(Http *http, HttpEndpoint *endpoint)
{
    mprRemoveItem(http->endpoints, endpoint);
}


/*
    Lookup a host address. If ipAddr is null or port is -1, then those elements are wild.
 */
PUBLIC HttpEndpoint *httpLookupEndpoint(Http *http, cchar *ip, int port)
{
    HttpEndpoint    *endpoint;
    int             next;

    if (ip == 0) {
        ip = "";
    }
    for (next = 0; (endpoint = mprGetNextItem(http->endpoints, &next)) != 0; ) {
        if (endpoint->port <= 0 || port <= 0 || endpoint->port == port) {
            assert(endpoint->ip);
            if (*endpoint->ip == '\0' || *ip == '\0' || scmp(endpoint->ip, ip) == 0) {
                return endpoint;
            }
        }
    }
    return 0;
}


PUBLIC HttpEndpoint *httpGetFirstEndpoint(Http *http)
{
    return mprGetFirstItem(http->endpoints);
}


/*
    WARNING: this should not be called by users as httpCreateHost will automatically call this.
 */
PUBLIC void httpAddHost(Http *http, HttpHost *host)
{
    mprAddItem(http->hosts, host);
}


PUBLIC void httpRemoveHost(Http *http, HttpHost *host)
{
    mprRemoveItem(http->hosts, host);
}


PUBLIC HttpHost *httpLookupHost(Http *http, cchar *name)
{
    HttpHost    *host;
    int         next;

    for (next = 0; (host = mprGetNextItem(http->hosts, &next)) != 0; ) {
        if (smatch(name, host->name)) {
            return host;
        }
    }
    return 0;
}


PUBLIC void httpInitLimits(HttpLimits *limits, bool serverSide)
{
    memset(limits, 0, sizeof(HttpLimits));
    limits->bufferSize = BIT_MAX_QBUFFER;
    limits->cacheItemSize = BIT_MAX_CACHE_ITEM;
    limits->chunkSize = BIT_MAX_CHUNK;
    limits->clientMax = BIT_MAX_CLIENTS;
    limits->connectionsMax = BIT_MAX_CONNECTIONS;
    limits->headerMax = BIT_MAX_NUM_HEADERS;
    limits->headerSize = BIT_MAX_HEADERS;
    limits->keepAliveMax = BIT_MAX_KEEP_ALIVE;
    limits->receiveFormSize = BIT_MAX_RECEIVE_FORM;
    limits->receiveBodySize = BIT_MAX_RECEIVE_BODY;
    limits->processMax = BIT_MAX_PROCESSES;
    limits->requestsPerClientMax = BIT_MAX_REQUESTS_PER_CLIENT;
    limits->sessionMax = BIT_MAX_SESSIONS;
    limits->transmissionBodySize = BIT_MAX_TX_BODY;
    limits->uploadSize = BIT_MAX_UPLOAD;
    limits->uriSize = BIT_MAX_URI;

    limits->inactivityTimeout = BIT_MAX_INACTIVITY_DURATION;
    limits->requestTimeout = BIT_MAX_REQUEST_DURATION;
    limits->requestParseTimeout = BIT_MAX_PARSE_DURATION;
    limits->sessionTimeout = BIT_MAX_SESSION_DURATION;

    limits->webSocketsMax = BIT_MAX_WSS_SOCKETS;
    limits->webSocketsMessageSize = HTTP_MAX_WSS_MESSAGE;
    limits->webSocketsFrameSize = BIT_MAX_WSS_FRAME;
    limits->webSocketsPacketSize = BIT_MAX_WSS_PACKET;
    limits->webSocketsPing = BIT_MAX_PING_DURATION;

#if FUTURE
    mprSetMaxSocketClients(endpoint, atoi(value));

    if (scaselesscmp(key, "LimitClients") == 0) {
        mprSetMaxSocketClients(endpoint, atoi(value));
        return 1;
    }
    if (scaselesscmp(key, "LimitMemoryMax") == 0) {
        mprSetAllocLimits(endpoint, -1, atoi(value));
        return 1;
    }
    if (scaselesscmp(key, "LimitMemoryRedline") == 0) {
        mprSetAllocLimits(endpoint, atoi(value), -1);
        return 1;
    }
#endif
}


PUBLIC HttpLimits *httpCreateLimits(int serverSide)
{
    HttpLimits  *limits;

    if ((limits = mprAllocStruct(HttpLimits)) != 0) {
        httpInitLimits(limits, serverSide);
    }
    return limits;
}


PUBLIC void httpEaseLimits(HttpLimits *limits)
{
    limits->receiveFormSize = MAXOFF;
    limits->receiveBodySize = MAXOFF;
    limits->transmissionBodySize = MAXOFF;
    limits->uploadSize = MAXOFF;
}


PUBLIC void httpAddStage(Http *http, HttpStage *stage)
{
    mprAddKey(http->stages, stage->name, stage);
}


PUBLIC HttpStage *httpLookupStage(Http *http, cchar *name)
{
    return mprLookupKey(http->stages, name);
}


PUBLIC void *httpLookupStageData(Http *http, cchar *name)
{
    HttpStage   *stage;
    if ((stage = mprLookupKey(http->stages, name)) != 0) {
        return stage->stageData;
    }
    return 0;
}


PUBLIC cchar *httpLookupStatus(Http *http, int status)
{
    HttpStatusCode  *ep;
    char            *key;

    key = itos(status);
    ep = (HttpStatusCode*) mprLookupKey(http->statusCodes, key);
    if (ep == 0) {
        return "Custom error";
    }
    return ep->msg;
}


PUBLIC void httpSetForkCallback(Http *http, MprForkCallback callback, void *data)
{
    http->forkCallback = callback;
    http->forkData = data;
}


PUBLIC void httpSetListenCallback(Http *http, HttpListenCallback fn)
{
    http->listenCallback = fn;
}


/*
    The http timer does maintenance activities and will fire per second while there are active requests.
    This is run in both servers and clients.
    NOTE: Because we lock the http here, connections cannot be deleted while we are modifying the list.
 */
static void httpTimer(Http *http, MprEvent *event)
{
    HttpConn    *conn;
    HttpStage   *stage;
    HttpLimits  *limits;
    MprModule   *module;
    int         next, active, abort;

    assert(event);

    updateCurrentDate(http);

    /* 
       Check for any inactive connections or expired requests (inactivityTimeout and requestTimeout)
       OPT - could check for expired connections every 10 seconds.
     */
    lock(http->connections);
    mprTrace(7, "httpTimer: %d active connections", mprGetListLength(http->connections));
    for (active = 0, next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; active++) {
        limits = conn->limits;
        if (!conn->timeoutEvent) {
            abort = 0;
            if (conn->endpoint && HTTP_STATE_BEGIN < conn->state && conn->state < HTTP_STATE_PARSED && 
                    (conn->started + limits->requestParseTimeout) < http->now) {
                abort = 1;
            } else if ((conn->lastActivity + limits->inactivityTimeout) < http->now || 
                    (conn->started + limits->requestTimeout) < http->now) {
                abort = 1;
            }
            if (abort && !mprGetDebugMode()) {
                conn->timeoutEvent = mprCreateEvent(conn->dispatcher, "connTimeout", 0, httpConnTimeout, conn, 0);
            }
        }
    }

    /*
        Check for unloadable modules
        OPT - could check for modules every minute
     */
    if (mprGetListLength(http->connections) == 0) {
        for (next = 0; (module = mprGetNextItem(MPR->moduleService->modules, &next)) != 0; ) {
            if (module->timeout) {
                if (module->lastActivity + module->timeout < http->now) {
                    mprLog(2, "Unloading inactive module %s", module->name);
                    if ((stage = httpLookupStage(http, module->name)) != 0) {
                        if (mprUnloadModule(module) < 0)  {
                            active++;
                        } else {
                            stage->flags |= HTTP_STAGE_UNLOADED;
                        }
                    } else {
                        mprUnloadModule(module);
                    }
                } else {
                    active++;
                }
            }
        }
    }
    if (active == 0) {
        mprRemoveEvent(event);
        http->timer = 0;
        /*
            Going to sleep now, so schedule a GC to free as much as possible.
         */
        mprRequestGC(MPR_GC_FORCE | MPR_GC_NO_BLOCK);
    } else {
        mprRequestGC(MPR_GC_NO_BLOCK);
    }
    unlock(http->connections);

}


static void timestamp()
{
    mprLog(0, "Time: %s", mprGetDate(NULL));
}


PUBLIC void httpSetTimestamp(MprTicks period)
{
    Http    *http;

    http = MPR->httpService;
    if (period < (10 * MPR_TICKS_PER_SEC)) {
        period = (10 * MPR_TICKS_PER_SEC);
    }
    if (http->timestamp) {
        mprRemoveEvent(http->timestamp);
    }
    if (period > 0) {
        http->timestamp = mprCreateTimerEvent(NULL, "httpTimestamp", period, timestamp, NULL, 
            MPR_EVENT_CONTINUOUS | MPR_EVENT_QUICK);
    }
}


static void terminateHttp(int how, int status)
{
    Http            *http;
    HttpEndpoint    *endpoint;
    int             next;

    /*
        Stop listening for new requests
     */
    http = (Http*) mprGetMpr()->httpService;
    if (http) {
        for (ITERATE_ITEMS(http->endpoints, endpoint, next)) {
            httpStopEndpoint(endpoint);
        }
    }
}


static bool isIdle()
{
    HttpConn        *conn;
    Http            *http;
    MprTicks        now;
    int             next;
    static MprTicks lastTrace = 0;

    http = (Http*) mprGetMpr()->httpService;
    now = http->now;

    lock(http->connections);
    for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
        if (conn->state != HTTP_STATE_BEGIN) {
            if (lastTrace < now) {
                if (conn->rx) {
                    mprLog(1, "  Request %s is still active", conn->rx->uri ? conn->rx->uri : conn->rx->pathInfo);
                } else {
                    mprLog(1, "Waiting for connection to close");
                    conn->started = 0;
                    conn->timeoutEvent = mprCreateEvent(conn->dispatcher, "connTimeout", 0, httpConnTimeout, conn, 0);
                }
                lastTrace = now;
            }
            unlock(http->connections);
            return 0;
        }
    }
    unlock(http->connections);
    if (!mprServicesAreIdle()) {
        if (lastTrace < now) {
            mprLog(3, "Waiting for MPR services complete");
            lastTrace = now;
        }
        return 0;
    }
    return 1;
}


PUBLIC void httpAddConn(Http *http, HttpConn *conn)
{
    http->now = mprGetTicks();
    assert(http->now >= 0);
    conn->started = http->now;
    mprAddItem(http->connections, conn);
    updateCurrentDate(http);

    lock(http);
    conn->seqno = (int) http->totalConnections++;
    if ((!BIT_DEBUG || !mprGetDebugMode()) && !http->timer) {
        http->timer = mprCreateTimerEvent(NULL, "httpTimer", HTTP_TIMER_PERIOD, httpTimer, http, 
            MPR_EVENT_CONTINUOUS | MPR_EVENT_QUICK);
    }
    unlock(http);
}


PUBLIC void httpRemoveConn(Http *http, HttpConn *conn)
{
    mprRemoveItem(http->connections, conn);
}


PUBLIC char *httpGetDateString(MprPath *sbuf)
{
    MprTicks    when;

    if (sbuf == 0) {
        when = mprGetTime();
    } else {
        when = (MprTicks) sbuf->mtime * MPR_TICKS_PER_SEC;
    }
    return mprFormatUniversalTime(HTTP_DATE_FORMAT, when);
}


PUBLIC void *httpGetContext(Http *http)
{
    return http->context;
}


PUBLIC void httpSetContext(Http *http, void *context)
{
    http->context = context;
}


PUBLIC int httpGetDefaultClientPort(Http *http)
{
    return http->defaultClientPort;
}


PUBLIC cchar *httpGetDefaultClientHost(Http *http)
{
    return http->defaultClientHost;
}


PUBLIC void httpSetDefaultClientPort(Http *http, int port)
{
    http->defaultClientPort = port;
}


PUBLIC void httpSetDefaultClientHost(Http *http, cchar *host)
{
    http->defaultClientHost = sclone(host);
}


PUBLIC void httpSetSoftware(Http *http, cchar *software)
{
    http->software = sclone(software);
}


PUBLIC void httpSetProxy(Http *http, cchar *host, int port)
{
    http->proxyHost = sclone(host);
    http->proxyPort = port;
}


static void updateCurrentDate(Http *http)
{
    MprTicks    diff;

    http->now = mprGetTicks();
    diff = http->now - http->currentTime;
    if (diff <= MPR_TICKS_PER_SEC || diff >= MPR_TICKS_PER_SEC) {
        /*
            Optimize and only update the string date representation once per second
         */
        http->currentTime = http->now;
        http->currentDate = httpGetDateString(NULL);
    }
}


PUBLIC void httpGetStats(HttpStats *sp)
{
    Http                *http;
    HttpAddress         *address;
    MprKey              *kp;
    MprMemStats         *ap;
    MprWorkerStats      wstats;

    memset(sp, 0, sizeof(*sp));
    http = MPR->httpService;
    ap = mprGetMemStats();

    sp->cpus = ap->numCpu;
    sp->mem = ap->rss;
    sp->memRedline = ap->warnHeap;
    sp->memMax = ap->maxHeap;

    sp->heap = ap->bytesAllocated + ap->bytesFree;
    sp->heapUsed = ap->bytesAllocated;
    sp->heapFree = ap->bytesFree;

    mprGetWorkerStats(&wstats);
    sp->workersBusy = wstats.busy;
    sp->workersIdle = wstats.idle;
    sp->workersYielded = wstats.yielded;
    sp->workersMax = wstats.max;

    sp->activeVMs = http->activeVMs;
    sp->activeConnections = mprGetListLength(http->connections);
    sp->activeProcesses = http->activeProcesses;
    sp->activeSessions = http->activeSessions;

    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        sp->activeRequests += (int) address->counters[HTTP_COUNTER_ACTIVE_REQUESTS].value;
        sp->activeClients += (int) address->counters[HTTP_COUNTER_ACTIVE_CLIENTS].value;
    }
    unlock(http->addresses);
    sp->totalRequests = http->totalRequests;
    sp->totalConnections = http->totalConnections;
    sp->totalSweeps = MPR->heap->iteration;

}


PUBLIC char *httpStatsReport(int flags)
{
    MprTime             now;
    MprBuf              *buf;
    HttpStats           s;
    double              elapsed;
    static MprTime      lastTime;
    static HttpStats    last;
    double              mb;

    mb = 1024.0 * 1024;
    now = mprGetTime();
    elapsed = (now - lastTime) / 1000.0;
    httpGetStats(&s);
    buf = mprCreateBuf(0, 0);

    mprPutToBuf(buf, "\nHttp Report: at %s\n\n", mprGetDate("%D %T"));
    mprPutToBuf(buf, "Memory      %8.1f MB, %5.1f%% max\n", s.mem / mb, s.mem / (double) s.memMax * 100.0);
    mprPutToBuf(buf, "Heap        %8.1f MB, %5.1f%% mem\n", s.heap / mb, s.heap / (double) s.mem * 100.0);
    mprPutToBuf(buf, "Heap-used   %8.1f MB, %5.1f%% used\n", s.heapUsed / mb, s.heapUsed / (double) s.heap * 100.0);
    mprPutToBuf(buf, "Heap-free   %8.1f MB, %5.1f%% free\n", s.heapFree / mb, s.heapFree / (double) s.heap * 100.0);

    mprPutCharToBuf(buf, '\n');
    mprPutToBuf(buf, "CPUs        %8d\n", s.cpus);
    mprPutCharToBuf(buf, '\n');

    mprPutToBuf(buf, "Connections %8.1f per/sec\n", (s.totalConnections - last.totalConnections) / elapsed);
    mprPutToBuf(buf, "Requests    %8.1f per/sec\n", (s.totalRequests - last.totalRequests) / elapsed);
    mprPutToBuf(buf, "Sweeps      %8.1f per/sec\n", (s.totalSweeps - last.totalSweeps) / elapsed);
    mprPutCharToBuf(buf, '\n');

    mprPutToBuf(buf, "Clients     %8d active\n", s.activeClients);
    mprPutToBuf(buf, "Connections %8d active\n", s.activeConnections);
    mprPutToBuf(buf, "Processes   %8d active\n", s.activeProcesses);
    mprPutToBuf(buf, "Requests    %8d active\n", s.activeRequests);
    mprPutToBuf(buf, "Sessions    %8d active\n", s.activeSessions);
    mprPutToBuf(buf, "VMs         %8d active\n", s.activeVMs);
    mprPutCharToBuf(buf, '\n');

    mprPutToBuf(buf, "Workers     %8d busy - %d yielded, %d idle, %d max\n", 
        s.workersBusy, s.workersYielded, s.workersIdle, s.workersMax);
    mprPutCharToBuf(buf, '\n');

    last = s;
    lastTime = now;
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}



/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
