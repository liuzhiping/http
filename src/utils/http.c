/*
    httpCmd.c -- Http client program

    The http program is a client to issue HTTP requests. It is also a test platform for loading and testing web servers. 

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */
 
/******************************** Includes ***********************************/

#include    "http.h"

/*********************************** Locals ***********************************/

typedef struct ThreadData {
    HttpConn        *conn;
    MprDispatcher   *dispatcher;
    char            *url;
    MprList         *files;
} ThreadData;

typedef struct App {
    int      activeLoadThreads;  /* Still running test threads */
    int      benchmark;          /* Output benchmarks */
    int      chunkSize;          /* Ask for response data to be chunked in this quanta */
    int      continueOnErrors;   /* Continue testing even if an error occurs. Default is to stop */
    int      success;            /* Total success flag */
    int      fetchCount;         /* Total count of fetches */
    MprList  *files;             /* Upload files */
    MprList  *formData;          /* Form body data */
    MprBuf   *bodyData;          /* Block body data */
    Mpr      *mpr;               /* Portable runtime */
    MprList  *headers;           /* Request headers */
    Http     *http;              /* Http service object */
    int      iterations;         /* URLs to fetch */
    int      isBinary;           /* Looks like a binary output file */
    char     *host;              /* Host to connect to */
    int      loadThreads;        /* Number of threads to use for URL requests */
    char     *method;            /* HTTP method when URL on cmd line */
    int      nextArg;            /* Next arg to parse */
    int      noout;              /* Don't output files */
    int      nofollow;           /* Don't automatically follow redirects */
    char     *password;          /* Password for authentication */
    int      printable;          /* Make binary output printable */
    char     *protocol;          /* HTTP/1.0, HTTP/1.1 */
    char     *ranges;            /* Request ranges */
    int      retries;            /* Times to retry a failed request */
    int      sequence;           /* Sequence requests with a custom header */
    int      showStatus;         /* Output the Http response status */
    int      showHeaders;        /* Output the response headers */
    int      singleStep;         /* Pause between requests */
    char     *target;            /* Destination url */
    int      timeout;            /* Timeout in secs for a non-responsive server */
    int      upload;             /* Upload using multipart mime */
    char     *username;          /* User name for authentication of requests */
    int      verbose;            /* Trace progress */
    int      workers;            /* Worker threads. >0 if multi-threaded */
    MprList  *threadData;        /* Per thread data */
    MprMutex *mutex;
} App;

static App *app;

/***************************** Forward Declarations ***************************/

static void     addFormVars(cchar *buf);
static void     processing();
static int      doRequest(HttpConn *conn, cchar *url, MprList *files);
static void     finishThread(MprThread *tp);
static char     *getPassword();
static void     initSettings();
static bool     isPort(cchar *name);
static bool     iterationsComplete();
static void     manageApp(App *app, int flags);
static void     manageThreadData(ThreadData *data, int flags);
static bool     parseArgs(int argc, char **argv);
static int      processThread(HttpConn *conn, MprEvent *event);
static void     threadMain(void *data, MprThread *tp);
static char     *resolveUrl(HttpConn *conn, cchar *url);
static int      setContentLength(HttpConn *conn, MprList *files);
static void     showOutput(HttpConn *conn, cchar *content, ssize contentLen);
static void     showUsage();
static int      startLogging(char *logSpec);
static void     trace(HttpConn *conn, cchar *url, int fetchCount, cchar *method, int status, ssize contentLen);
static void     waitForUser();
static int      writeBody(HttpConn *conn, MprList *files);

/*********************************** Code *************************************/

MAIN(httpMain, int argc, char *argv[])
{
    MprTime     start;
    double      elapsed;

    if (mprCreate(argc, argv, MPR_USER_EVENTS_THREAD) == 0) {
        return MPR_ERR_MEMORY;
    }
    if ((app = mprAllocObj(App, manageApp)) == 0) {
        return MPR_ERR_MEMORY;
    }
    mprAddRoot(app);

    initSettings();
    if (!parseArgs(argc, argv)) {
        showUsage();
        return MPR_ERR_BAD_ARGS;
    }
    mprSetMaxWorkers(app->workers);

#if BLD_FEATURE_SSL
    if (!mprLoadSsl(1)) {
        mprError("Can't load SSL");
        exit(1);
    }
#endif

    if (mprStart() < 0) {
        mprError("Can't start MPR for %s", mprGetAppTitle());
        exit(2);
    }
    start = mprGetTime();
    app->http = httpCreate();

    processing();
    mprServiceEvents(-1, 0);

    if (app->benchmark) {
        elapsed = (double) (mprGetTime() - start);
        if (app->fetchCount == 0) {
            elapsed = 0;
            app->fetchCount = 1;
        }
        mprPrintf("\nRequest Count:       %13d\n", app->fetchCount);
        mprPrintf("Time elapsed:        %13.4f sec\n", elapsed / 1000.0);
        mprPrintf("Time per request:    %13.4f sec\n", elapsed / 1000.0 / app->fetchCount);
        mprPrintf("Requests per second: %13.4f\n", app->fetchCount * 1.0 / (elapsed / 1000.0));
        mprPrintf("Load threads:        %13d\n", app->loadThreads);
        mprPrintf("Worker threads:      %13d\n", app->workers);
    }
    if (!app->success && app->verbose) {
        mprError("Request failed");
    }
    return (app->success) ? 0 : 255;
}


static void manageApp(App *app, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(app->files);
        mprMark(app->formData);
        mprMark(app->headers);
        mprMark(app->bodyData);
        mprMark(app->http);
        mprMark(app->password);
        mprMark(app->ranges);
        mprMark(app->mutex);
        mprMark(app->threadData);
    }
}


static void initSettings()
{
    app->method = 0;
    app->verbose = 0;
    app->continueOnErrors = 0;
    app->showHeaders = 0;

    app->host = "localhost";
    app->iterations = 1;
    app->loadThreads = 1;
    app->protocol = "HTTP/1.1";
    app->retries = HTTP_RETRIES;
    app->success = 1;
    app->timeout = 60;
    app->workers = 1;            
    app->headers = mprCreateList(0, 0);
    app->mutex = mprCreateLock();
#if WIN
    _setmode(fileno(stdout), O_BINARY);
#endif
}


static bool parseArgs(int argc, char **argv)
{
    char    *argp, *key, *value;
    int     i, setWorkers, httpVersion, nextArg;

    setWorkers = 0;

    for (nextArg = 1; nextArg < argc; nextArg++) {
        argp = argv[nextArg];
        if (*argp != '-') {
            break;
        }

        if (strcmp(argp, "--benchmark") == 0 || strcmp(argp, "-b") == 0) {
            app->benchmark++;

        } else if (strcmp(argp, "--chunk") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                value = argv[++nextArg];
                app->chunkSize = atoi(value);
                if (app->chunkSize < 0) {
                    mprError("Bad chunksize %d", app->chunkSize);
                    return 0;
                }
            }

        } else if (strcmp(argp, "--continue") == 0) {
            app->continueOnErrors++;

        } else if (strcmp(argp, "--cookie") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                mprAddItem(app->headers, mprCreateKeyPair("Cookie", argv[++nextArg]));
            }

        } else if (strcmp(argp, "--data") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                if (app->bodyData == 0) {
                    app->bodyData = mprCreateBuf(-1, -1);
                }
                mprPutStringToBuf(app->bodyData, argv[++nextArg]);
            }

        } else if (strcmp(argp, "--debugger") == 0 || strcmp(argp, "-D") == 0) {
            mprSetDebugMode(1);
            app->retries = 0;
            app->timeout = INT_MAX / MPR_TICKS_PER_SEC;

        } else if (strcmp(argp, "--delete") == 0) {
            app->method = "DELETE";

        } else if (strcmp(argp, "--form") == 0 || strcmp(argp, "-f") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                if (app->formData == 0) {
                    app->formData = mprCreateList(-1, 0);
                }
                addFormVars(argv[++nextArg]);
            }

        } else if (strcmp(argp, "--header") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                key = argv[++nextArg];
                if ((value = strchr(key, ':')) == 0) {
                    mprError("Bad header format. Must be \"key: value\"");
                    return 0;
                }
                *value++ = '\0';
                while (isspace((int) *value)) {
                    value++;
                }
                mprAddItem(app->headers, mprCreateKeyPair(key, value));
            }

        } else if (strcmp(argp, "--host") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->host = argv[++nextArg];
            }

        } else if (strcmp(argp, "--http") == 0) {
            //  DEPRECATED
            if (nextArg >= argc) {
                return 0;
            } else {
                httpVersion = atoi(argv[++nextArg]);
                app->protocol = (httpVersion == 0) ? "HTTP/1.0" : "HTTP/1.1";
            }

        } else if (strcmp(argp, "--iterations") == 0 || strcmp(argp, "-i") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->iterations = atoi(argv[++nextArg]);
            }

        } else if (strcmp(argp, "--log") == 0 || strcmp(argp, "-l") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                startLogging(argv[++nextArg]);
            }

        } else if (strcmp(argp, "--method") == 0 || strcmp(argp, "-m") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->method = argv[++nextArg];
            }

        } else if (strcmp(argp, "--noout") == 0 || strcmp(argp, "-n") == 0  ||
                   strcmp(argp, "--quiet") == 0 || strcmp(argp, "-q") == 0) {
            app->noout++;

        } else if (strcmp(argp, "--nofollow") == 0) {
            app->nofollow++;

        } else if (strcmp(argp, "--password") == 0 || strcmp(argp, "-p") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->password = sclone(argv[++nextArg]);
            }

        } else if (strcmp(argp, "--post") == 0) {
            app->method = "POST";

        } else if (strcmp(argp, "--printable") == 0) {
            app->printable++;

        } else if (strcmp(argp, "--protocol") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->protocol = supper(argv[++nextArg]);
            }

        } else if (strcmp(argp, "--put") == 0) {
            app->method = "PUT";

        } else if (strcmp(argp, "--range") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                //  TODO - should allow multiple ranges
                if (app->ranges == 0) {
                    app->ranges = mprAsprintf("bytes=%s", argv[++nextArg]);
                } else {
                    app->ranges = srejoin(app->ranges, ",", argv[++nextArg], NULL);
                }
            }
            
        } else if (strcmp(argp, "--retries") == 0 || strcmp(argp, "-r") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->retries = atoi(argv[++nextArg]);
            }
            
        } else if (strcmp(argp, "--sequence") == 0) {
            app->sequence++;

        } else if (strcmp(argp, "--showHeaders") == 0 || strcmp(argp, "--show") == 0) {
            app->showHeaders++;

        } else if (strcmp(argp, "--showStatus") == 0 || strcmp(argp, "--showCode") == 0) {
            app->showStatus++;

        } else if (strcmp(argp, "--single") == 0 || strcmp(argp, "-s") == 0) {
            app->singleStep++;

        } else if (strcmp(argp, "--threads") == 0 || strcmp(argp, "-t") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->loadThreads = atoi(argv[++nextArg]);
            }

        } else if (strcmp(argp, "--timeout") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->timeout = atoi(argv[++nextArg]);
            }

        } else if (strcmp(argp, "--upload") == 0 || strcmp(argp, "-u") == 0) {
            app->upload++;

        } else if (strcmp(argp, "--user") == 0 || strcmp(argp, "--username") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->username = argv[++nextArg];
            }

        } else if (strcmp(argp, "--verbose") == 0 || strcmp(argp, "-v") == 0) {
            app->verbose++;

        } else if (strcmp(argp, "--version") == 0 || strcmp(argp, "-V") == 0) {
            mprPrintfError("%s %s\n"
                "Copyright (C) Embedthis Software 2003-2011\n"
                "Copyright (C) Michael O'Brien 2003-2011\n",
               BLD_NAME, BLD_VERSION);
            exit(0);

        } else if (strcmp(argp, "--workerTheads") == 0 || strcmp(argp, "-w") == 0) {
            if (nextArg >= argc) {
                return 0;
            } else {
                app->workers = atoi(argv[++nextArg]);
            }
            setWorkers++;

        } else if (strcmp(argp, "--") == 0) {
            nextArg++;
            break;

        } else if (strcmp(argp, "-") == 0) {
            break;

        } else {
            return 0;
            break;
        }
    }
    if (argc == nextArg) {
        return 0;
    }
    app->nextArg = nextArg;
    argc = argc - nextArg;
    argv = &argv[nextArg];
    app->target = argv[argc - 1];
    if (--argc > 0) {
        /*
            Files present on command line
         */
        app->files = mprCreateList(argc, MPR_LIST_STATIC_VALUES);
        for (i = 0; i < argc; i++) {
            mprAddItem(app->files, argv[i]);
        }
    }
    if (!setWorkers) {
        app->workers = app->loadThreads + 2;
    }
    if (app->method == 0) {
        if (app->bodyData || app->formData || app->files) {
            app->method = "POST";
        } else if (app->files) {
            app->method = "PUT";
        } else {
            app->method = "GET";
        }
    }
    return 1;
}


static void showUsage()
{
    mprPrintfError("usage: %s [options] [files] url\n"
        "  Options:\n"
        "  --benchmark           # Compute benchmark results.\n"
        "  --chunk size          # Request response data to use this chunk size.\n"
        "  --continue            # Continue on errors.\n"
        "  --cookie CookieString # Define a cookie header. Multiple uses okay.\n"
        "  --data                # Body data to send with PUT or POST.\n"
        "  --debugger            # Disable timeouts to make running in a debugger easier.\n"
        "  --delete              # Use the DELETE method. Shortcut for --method DELETE..\n"
        "  --form string         # Form data. Must already be form-www-urlencoded.\n"
        "  --header 'key: value' # Add a custom request header.\n"
        "  --host hostName       # Host name or IP address for unqualified URLs.\n"
        "  --iterations count    # Number of times to fetch the urls (default 1).\n"
        "  --log logFile:level   # Log to the file at the verbosity level.\n"
        "  --method KIND         # HTTP request method GET|OPTIONS|POST|PUT|TRACE (default GET).\n"
        "  --nofollow            # Don't automatically follow redirects.\n"
        "  --noout               # Don't output files to stdout.\n"
        "  --password pass       # Password for authentication.\n"
        "  --post                # Use POST method. Shortcut for --method POST.\n"
        "  --printable           # Make binary output printable.\n"
        "  --protocol PROTO      # Set HTTP protocol to HTTP/1.0 or HTTP/1.1 .\n"
        "  --put                 # Use PUT method. Shortcut for --method PUT.\n"
        "  --range byteRanges    # Request a subset range of the document.\n"
        "  --retries count       # Number of times to retry failing requests.\n"
        "  --sequence            # Sequence requests with a custom header.\n"
        "  --showHeaders         # Output response headers.\n"
        "  --showStatus          # Output the Http response status code.\n"
        "  --single              # Single step. Pause for input between requests.\n"
        "  --timeout secs        # Request timeout period in seconds.\n"
        "  --threads count       # Number of thread instances to spawn.\n"
        "  --upload              # Use multipart mime upload.\n"
        "  --user name           # User name for authentication.\n"
        "  --verbose             # Verbose operation. Trace progress.\n"
        "  --workers count       # Set maximum worker threads.\n",
        mprGetAppName());
}


static void processing()
{
    MprThread   *tp;
    ThreadData  *data;
    int         j;

    if (app->chunkSize > 0) {
        mprAddItem(app->headers, mprCreateKeyPair("X-Appweb-Chunk-Size", mprAsprintf("%d", app->chunkSize)));
    }
    app->activeLoadThreads = app->loadThreads;
    app->threadData = mprCreateList(app->loadThreads, 0);

    for (j = 0; j < app->loadThreads; j++) {
        char name[64];
        if ((data = mprAllocObj(ThreadData, manageThreadData)) == 0) {
            return;
        }
        mprAddItem(app->threadData, data);

        mprSprintf(name, sizeof(name), "http.%d", j);
        tp = mprCreateThread(name, threadMain, NULL, 0); 
        tp->data = data;
        mprStartThread(tp);
    }
}


static void manageThreadData(ThreadData *data, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(data->url);
        mprMark(data->files);
        mprMark(data->conn);
    }
}


/*  
    Per-thread execution. Called for main thread and helper threads.
 */ 
static void threadMain(void *data, MprThread *tp)
{
    ThreadData      *td;
    HttpConn        *conn;
    MprEvent        e;

    td = tp->data;
    td->dispatcher = mprCreateDispatcher(tp->name, 1);
    td->conn = conn = httpCreateClient(app->http, td->dispatcher);

    /*  
        Relay to processThread via the dispatcher. This serializes all activity on the conn->dispatcher
     */
    e.mask = MPR_READABLE;
    e.data = tp;
    mprRelayEvent(conn->dispatcher, (MprEventProc) processThread, conn, &e);
}


static int processThread(HttpConn *conn, MprEvent *event)
{
    ThreadData  *td;
    MprList     *files;
    cchar       *path;
    char        *url;
    int         next;

    td = mprGetCurrentThread()->data;
    httpFollowRedirects(conn, !app->nofollow);
    httpSetTimeout(conn, app->timeout, app->timeout);

    if (strcmp(app->protocol, "HTTP/1.0") == 0) {
        httpSetKeepAliveCount(conn, 0);
        httpSetProtocol(conn, "HTTP/1.0");
    }
    if (app->username) {
        if (app->password == 0 && !strchr(app->username, ':')) {
            app->password = getPassword();
        }
        httpSetCredentials(conn, app->username, app->password);
    }
    while (!mprIsStopping(conn) && (app->success || app->continueOnErrors)) {
        if (app->singleStep) waitForUser();
        if (app->files && !app->upload) {
            for (next = 0; (path = mprGetNextItem(app->files, &next)) != 0; ) {
                /*
                    If URL ends with "/", assume it is a directory on the target and append each file name 
                 */
                if (app->target[strlen(app->target) - 1] == '/') {
                    url = mprJoinPath(app->target, mprGetPathBase(path));
                } else {
                    url = app->target;
                }
                files = mprCreateList(-1, -0);
                mprAddItem(files, path);
                td->url = url = resolveUrl(conn, url);
                if (app->verbose) {
                    mprPrintf("putting: %s to %s\n", path, url);
                }
                if (doRequest(conn, url, files) < 0) {
                    app->success = 0;
                    break;
                }
            }
        } else {
            td->url = url = resolveUrl(conn, app->target);
            if (doRequest(conn, url, NULL) < 0) {
                app->success = 0;
                break;
            }
        }
        if (iterationsComplete()) {
            break;
        }
    }
    httpDestroyConn(conn);
    finishThread((MprThread*) event->data);
    return -1;
}


static int prepRequest(HttpConn *conn, MprList *files)
{
    MprKeyValue     *header;
    char            seqBuf[16];
    int             next;

    httpPrepClientConn(conn);

    for (next = 0; (header = mprGetNextItem(app->headers, &next)) != 0; ) {
        httpAppendHeader(conn, header->key, header->value);
    }
    if (app->sequence) {
        static int next = 0;
        itos(seqBuf, sizeof(seqBuf), next++, 10);
        httpSetHeader(conn, "X-Http-Seq", seqBuf);
    }
    if (app->ranges) {
        httpSetHeader(conn, "Range", app->ranges);
    }
    if (app->formData) {
        httpSetHeader(conn, "Content-Type", "application/x-www-form-urlencoded");
    }
    if (app->chunkSize > 0) {
        httpSetChunkSize(conn, app->chunkSize);
    }
    if (setContentLength(conn, files) < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    return 0;
}


static int sendRequest(HttpConn *conn, cchar *method, cchar *url, MprList *files)
{
    if (httpConnect(conn, method, url) < 0) {
        mprError("Can't process request for \"%s\". %s.", url, httpGetError(conn));
        return MPR_ERR_CANT_OPEN;
    }
    /*  
        This program does not do full-duplex writes with reads. ie. if you have a request that sends and receives
        data in parallel -- http will do the writes first then read the response.
     */
    if (app->bodyData || app->formData || files) {
        if (writeBody(conn, files) < 0) {
            mprError("Can't write body data to \"%s\". %s", url, httpGetError(conn));
            return MPR_ERR_CANT_WRITE;
        }
    }
    mprAssert(!mprGetCurrentThread()->yielded);
    httpFinalize(conn);
    return 0;
}


static int issueRequest(HttpConn *conn, cchar *url, MprList *files)
{
    HttpRx      *rx;
    HttpUri     *target, *location;
    char        *redirect;
    cchar       *msg, *sep;
    int         count, redirectCount;

    httpSetRetries(conn, app->retries);
    httpSetTimeout(conn, app->timeout, app->timeout);

    for (redirectCount = count = 0; count <= conn->retries && redirectCount < 16 && !mprIsStopping(conn); count++) {
        if (prepRequest(conn, files) < 0) {
            return MPR_ERR_CANT_OPEN;
        }
        if (sendRequest(conn, app->method, url, files) < 0) {
            return MPR_ERR_CANT_WRITE;
        }
        if (httpWait(conn, conn->dispatcher, HTTP_STATE_PARSED, conn->limits->requestTimeout) == 0) {
            if (httpNeedRetry(conn, &redirect)) {
                if (redirect) {
                    location = httpCreateUri(redirect, 0);
                    target = httpJoinUri(conn->tx->parsedUri, 1, &location);
                    url = httpUriToString(target, 1);
                }
                /* Count redirects and auth retries */
                redirectCount++;
                count--; 
            } else {
                break;
            }
        } else if (!conn->error) {
            httpConnError(conn, HTTP_CODE_REQUEST_TIMEOUT,
                "Inactive request timed out, exceeded request timeout %d", app->timeout);
        }
        if ((rx = conn->rx) != 0) {
            if (rx->status == HTTP_CODE_REQUEST_TOO_LARGE || rx->status == HTTP_CODE_REQUEST_URL_TOO_LARGE ||
                (rx->status == HTTP_CODE_UNAUTHORIZED && conn->authUser == 0)) {
                /* No point retrying */
                break;
            }
        }
        mprLog(MPR_DEBUG, "retry %d of %d for: %s %s", count, conn->retries, app->method, url);
    }
    if (conn->error || conn->errorMsg) {
        msg = (conn->errorMsg) ? conn->errorMsg : "";
        sep = (msg && *msg) ? "\n" : "";
        mprError("http: failed \"%s\" request for %s after %d attempt(s).%s%s", app->method, url, count, sep, msg);
        return MPR_ERR_CANT_CONNECT;
    }
    return 0;
}


static int reportResponse(HttpConn *conn, cchar *url, MprTime elapsed)
{
    HttpRx      *rx;
    char        *responseHeaders;
    ssize       contentLen;
    int         status;

    if (mprIsStopping(conn)) {
        return 0;
    }

    status = httpGetStatus(conn);
    contentLen = httpGetContentLength(conn);
    if (contentLen < 0) {
        contentLen = conn->rx->readContent;
    }

    mprLog(6, "Response status %d, elapsed %ld", status, elapsed);
    if (conn->error) {
        app->success = 0;
    }
    if (conn->rx && app->success) {
        if (app->showStatus) {
            mprPrintf("%d\n", status);
        }
        if (app->showHeaders) {
            responseHeaders = httpGetHeaders(conn);
            rx = conn->rx;
            mprPrintfError("\nHeaders\n-------\n%s %d %s\n", conn->protocol, rx->status, rx->statusMessage);
            if (responseHeaders) {
                mprPrintfError("%s\n", responseHeaders);
            }
        }
    }
    if (status < 0) {
        mprError("Can't process request for \"%s\" %s", url, httpGetError(conn));
        return MPR_ERR_CANT_READ;

    } else if (status == 0 && conn->protocol == 0) {
        /* Ignore */;

    } else if (!(200 <= status && status <= 206) && !(301 <= status && status <= 304)) {
        if (!app->showStatus) {
            mprError("Can't process request for \"%s\" (%d) %s", url, status, httpGetError(conn));
            return MPR_ERR_CANT_READ;
        }
    }
    mprLock(app->mutex);
    if (app->verbose && app->noout) {
        trace(conn, url, app->fetchCount, app->method, status, contentLen);
    }
    mprUnlock(app->mutex);
    return 0;
}


static void readBody(HttpConn *conn)
{
    char    buf[HTTP_BUFSIZE];
    ssize   bytes;

    while (!conn->error && conn->sock && (bytes = httpRead(conn, buf, sizeof(buf))) > 0) {
        showOutput(conn, buf, bytes);
    }
}


static int doRequest(HttpConn *conn, cchar *url, MprList *files)
{
    MprTime         mark;
    HttpLimits      *limits;

    mprAssert(url && *url);
    limits = conn->limits;

    mprLog(MPR_DEBUG, "fetch: %s %s", app->method, url);
    mark = mprGetTime();

    if (issueRequest(conn, url, files) < 0) {
        return MPR_ERR_CANT_CONNECT;
    }
    while (!conn->error && conn->state < HTTP_STATE_COMPLETE && mprGetElapsedTime(mark) <= limits->requestTimeout) {
        httpWait(conn, conn->dispatcher, HTTP_STATE_COMPLETE, 10);
        readBody(conn);
    }
    if (conn->state < HTTP_STATE_COMPLETE && !conn->error) {
        httpConnError(conn, HTTP_CODE_REQUEST_TIMEOUT,
            "Inactive request timed out, exceeded request timeout %d", app->timeout);
    } else {
        readBody(conn);
    }
    reportResponse(conn, url, mprGetTime() - mark);

    httpDestroyRx(conn->rx);
    httpDestroyTx(conn->tx);
    return 0;
}


static int setContentLength(HttpConn *conn, MprList *files)
{
    MprPath     info;
    char        *path, *pair;
    ssize       len;
    int         next;

    len = 0;
    if (app->upload) {
        httpEnableUpload(conn);
        return 0;
    }
    for (next = 0; (path = mprGetNextItem(files, &next)) != 0; ) {
        if (strcmp(path, "-") != 0) {
            if (mprGetPathInfo(path, &info) < 0) {
                mprError("Can't access file %s", path);
                return MPR_ERR_CANT_ACCESS;
            }
            len += (int) info.size;
        }
    }
    if (app->formData) {
        for (next = 0; (pair = mprGetNextItem(app->formData, &next)) != 0; ) {
            len += strlen(pair);
        }
        len += mprGetListLength(app->formData) - 1;
    }
    if (app->bodyData) {
        len += mprGetBufLength(app->bodyData);
    }
    if (len > 0) {
        httpSetContentLength(conn, len);
    }
    return 0;
}


static int writeBody(HttpConn *conn, MprList *files)
{
    MprFile     *file;
    char        buf[HTTP_BUFSIZE], *path, *pair;
    ssize       bytes, len;
    int         next, count, rc;

    rc = 0;
    if (app->upload) {
        if (httpWriteUploadData(conn, app->files, app->formData) < 0) {
            mprError("Can't write upload data %s", httpGetError(conn));
            return MPR_ERR_CANT_WRITE;
        }
    } else {
        if (app->formData) {
            count = mprGetListLength(app->formData);
            for (next = 0; !rc && (pair = mprGetNextItem(app->formData, &next)) != 0; ) {
                len = strlen(pair);
                if (next < count) {
                    len = strlen(pair);
                    if (httpWrite(conn->writeq, pair, len) != len || httpWrite(conn->writeq, "&", 1) != 1) {
                        return MPR_ERR_CANT_WRITE;
                    }
                } else {
                    if (httpWrite(conn->writeq, pair, len) != len) {
                        return MPR_ERR_CANT_WRITE;
                    }
                }
            }
        }
        if (files) {
            mprAssert(mprGetListLength(files) == 1);
            for (rc = next = 0; !rc && (path = mprGetNextItem(files, &next)) != 0; ) {
                if (strcmp(path, "-") == 0) {
                    file = mprAttachFileFd(0, "stdin", O_RDONLY | O_BINARY);
                } else {
                    file = mprOpenFile(path, O_RDONLY | O_BINARY, 0);
                }
                if (file == 0) {
                    mprError("Can't open \"%s\"", path);
                    return MPR_ERR_CANT_OPEN;
                }
                if (app->verbose) {
                    mprPrintf("uploading: %s\n", path);
                }
                while ((bytes = mprReadFile(file, buf, sizeof(buf))) > 0) {
                    if (httpWriteBlock(conn->writeq, buf, bytes) != bytes) {
                        mprCloseFile(file);
                        return MPR_ERR_CANT_WRITE;
                    }
                }
                mprCloseFile(file);
            }
        }
        if (app->bodyData) {
            mprAddNullToBuf(app->bodyData);
            len = strlen(app->bodyData->start);
            len = mprGetBufLength(app->bodyData);
            if (httpWriteBlock(conn->writeq, mprGetBufStart(app->bodyData), len) != len) {
                return MPR_ERR_CANT_WRITE;
            }
        }
    }
    return rc;
}


static bool iterationsComplete()
{
    mprLock(app->mutex);
    if (app->verbose > 1) mprPrintf(".");
    if (++app->fetchCount >= app->iterations) {
        mprUnlock(app->mutex);
        return 1;
    }
    mprUnlock(app->mutex);
    return 0;
}


static void finishThread(MprThread *tp)
{
    if (tp) {
        mprLock(app->mutex);
        app->activeLoadThreads--;
        if (--app->activeLoadThreads <= 0) {
            mprTerminate(MPR_GRACEFUL);
        }
        mprUnlock(app->mutex);
    }
}


static void waitForUser()
{
    int     c;

    mprLock(app->mutex);
    mprPrintf("Pause: ");
    (void) read(0, (char*) &c, 1);
    mprUnlock(app->mutex);
}


static void addFormVars(cchar *buf)
{
    char    *pair, *tok;

    pair = stok(sclone(buf), "&", &tok);
    while (pair != 0) {
        mprAddItem(app->formData, pair);
        pair = stok(0, "&", &tok);
    }
}


static bool isPort(cchar *name)
{
    cchar   *cp;

    for (cp = name; *cp && *cp != '/'; cp++) {
        if (!isdigit((int) *cp) || *cp == '.') {
            return 0;
        }
    }
    return 1;
}


static char *resolveUrl(HttpConn *conn, cchar *url)
{
    //  TODO replace with Url join
    if (*url == '/') {
        if (app->host) {
            if (sncasecmp(app->host, "http://", 7) != 0 && sncasecmp(app->host, "https://", 8) != 0) {
                return mprAsprintf("http://%s%s", app->host, url);
            } else {
                return mprAsprintf("%s%s", app->host, url);
            }
        } else {
            return mprAsprintf("http://127.0.0.1%s", url);
        }
    } 
    if (sncasecmp(url, "http://", 7) != 0 && sncasecmp(url, "https://", 8) != 0) {
        if (isPort(url)) {
            return mprAsprintf("http://127.0.0.1:%s", url);
        } else {
            return mprAsprintf("http://%s", url);
        }
    }
    return sclone(url);
}


static void showOutput(HttpConn *conn, cchar *buf, ssize count)
{
    HttpRx      *rx;
    int         i, c;
    
    rx = conn->rx;

    if (app->noout) {
        return;
    }
    if (rx->status == 401 || (conn->followRedirects && (301 <= rx->status && rx->status <= 302))) {
        return;
    }
    if (!app->printable) {
        (void) write(1, (char*) buf, count);
        return;
    }

    for (i = 0; i < count; i++) {
        if (!isprint((int) buf[i]) && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') {
            app->isBinary = 1;
            break;
        }
    }
    if (!app->isBinary) {
        (void) write(1, (char*) buf, count);
        return;
    }
    for (i = 0; i < count; i++) {
        c = (uchar) buf[i];
        if (app->printable && app->isBinary) {
            mprPrintf("%02x ", c & 0xff);
        } else {
            mprPrintf("%c", (int) buf[i]);
        }
    }
}


static void trace(HttpConn *conn, cchar *url, int fetchCount, cchar *method, int status, ssize contentLen)
{
    if (sncasecmp(url, "http://", 7) != 0) {
        url += 7;
    }
    if ((fetchCount % 200) == 1) {
        if (fetchCount == 1 || (fetchCount % 5000) == 1) {
            if (fetchCount > 1) {
                mprPrintf("\n");
            }
            mprPrintf("  Count  Thread   Op  Code   Bytes  Url\n");
        }
        mprPrintf("%7d %7s %4s %5d %7d  %s\n", fetchCount - 1,
            mprGetCurrentThreadName(conn), method, status, contentLen, url);
    }
}


static void logHandler(int flags, int level, const char *msg)
{
    Mpr         *mpr;
    MprFile     *file;
    char        *prefix;

    mpr = mprGetMpr();
    file = (MprFile*) mpr->logData;
    prefix = mpr->name;

    while (*msg == '\n') {
        mprFprintf(file, "\n");
        msg++;
    }
    if (flags & MPR_LOG_SRC) {
        mprFprintf(file, "%s: %d: %s\n", prefix, level, msg);
    } else if (flags & MPR_ERROR_SRC) {
        mprFprintf(file, "%s: Error: %s\n", prefix, msg);
    } else if (flags & MPR_FATAL_SRC) {
        mprFprintf(file, "%s: Fatal: %s\n", prefix, msg);
    } else if (flags & MPR_ASSERT_SRC) {
        mprFprintf(file, "%s: Assertion %s, failed\n", prefix, msg);
    } else if (flags & MPR_RAW) {
        mprFprintf(file, "%s", msg);
    }
    if (flags & (MPR_ERROR_SRC | MPR_FATAL_SRC | MPR_ASSERT_SRC)) {
        mprBreakpoint();
    }
}



static int startLogging(char *logSpec)
{
    MprFile     *file;
    char        *levelSpec;
    int         level;

    level = 0;
    logSpec = sclone(logSpec);

    if ((levelSpec = strchr(logSpec, ':')) != 0) {
        *levelSpec++ = '\0';
        level = atoi(levelSpec);
    }
    if (strcmp(logSpec, "stdout") == 0) {
        file = MPR->fileSystem->stdOutput;
    } else {
        if ((file = mprOpenFile(logSpec, O_CREAT | O_WRONLY | O_TRUNC | O_TEXT, 0664)) == 0) {
            mprPrintfError("Can't open log file %s\n", logSpec);
            return -1;
        }
    }
    mprSetLogLevel(level);
    mprSetLogHandler(logHandler, file);
    return 0;
}


#if (BLD_WIN_LIKE && !WINCE) || VXWORKS
static char *getpass(char *prompt)
{
    static char password[MPR_MAX_STRING];
    int     c, i;

    fputs(prompt, stderr);
    for (i = 0; i < (int) sizeof(password) - 1; i++) {
#if VXWORKS
        c = getchar();
#else
        c = _getch();
#endif
        if (c == '\r' || c == EOF) {
            break;
        }
        if ((c == '\b' || c == 127) && i > 0) {
            password[--i] = '\0';
            fputs("\b \b", stderr);
            i--;
        } else if (c == 26) {           /* Control Z */
            c = EOF;
            break;
        } else if (c == 3) {            /* Control C */
            fputs("^C\n", stderr);
            exit(255);
        } else if (!iscntrl(c) && (i < (int) sizeof(password) - 1)) {
            password[i] = c;
            fputc('*', stderr);
        } else {
            fputc('', stderr);
            i--;
        }
    }
    if (c == EOF) {
        return "";
    }
    fputc('\n', stderr);
    password[i] = '\0';
    return password;
}

#endif /* WIN */


static char *getPassword()
{
#if !WINCE
    char    *password;

    password = getpass("Password: ");
#else
    password = "no-user-interaction-support";
#endif
    return sclone(password);
}


#if VXWORKS
/*
    VxWorks link resolution
 */
int _cleanup() {
    return 0;
}
int _exit() {
    return 0;
}
#endif

/*
    @copy   default
    
    Copyright (c) Embedthis Software LLC, 2003-2011. All Rights Reserved.
    Copyright (c) Michael O'Brien, 1993-2011. All Rights Reserved.
    
    This software is distributed under commercial and open source licenses.
    You may use the GPL open source license described below or you may acquire 
    a commercial license from Embedthis Software. You agree to be fully bound 
    by the terms of either license. Consult the LICENSE.TXT distributed with 
    this software for full details.
    
    This software is open source; you can redistribute it and/or modify it 
    under the terms of the GNU General Public License as published by the 
    Free Software Foundation; either version 2 of the License, or (at your 
    option) any later version. See the GNU General Public License for more 
    details at: http://www.embedthis.com/downloads/gplLicense.html
    
    This program is distributed WITHOUT ANY WARRANTY; without even the 
    implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
    
    This GPL license does NOT permit incorporating this software into 
    proprietary programs. If you are unable to comply with the GPL, you must
    acquire a commercial license to use this software. Commercial licenses 
    for this software and support services are available from Embedthis 
    Software at http://www.embedthis.com 
    
    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */