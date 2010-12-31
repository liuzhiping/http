/**
    testHttpGen.c - tests for HTTP
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "http.h"

/************************************ Code ************************************/

static int initHttp(MprTestGroup *gp)
{
    MprSocket   *sp;
    
    if (getenv("NO_INTERNET")) {
        gp->skip = 1;
        return 0;
    }
    sp = mprCreateSocket(NULL);
    
    /*
        Test if we have network connectivity. If not, then skip these tests.
     */
    if (mprConnectSocket(sp, "www.google.com", 80, 0) < 0) {
        static int once = 0;
        if (once++ == 0) {
            mprPrintf("%12s Disabling tests %s.*: no internet connection. %d\n", "[Notice]", gp->fullName, once);
        }
        gp->skip = 1;
    }
    mprCloseSocket(sp, 0);
    return 0;
}


static void testCreateHttp(MprTestGroup *gp)
{
    Http     *http;

    http = httpCreate(gp);
    assert(http != 0);
}


static void testBasicHttpGet(MprTestGroup *gp)
{
    Http        *http;
    HttpConn    *conn;
    ssize       length;
    int         rc, status;

    http = httpCreate(gp);
    assert(http != 0);

    conn = httpCreateConn(http, NULL);
    rc = httpConnect(conn, "GET", "http://embedthis.com/index.html");
    assert(rc >= 0);
    if (rc >= 0) {
        httpFinalize(conn);
        httpWait(conn, conn->dispatcher, HTTP_STATE_COMPLETE, MPR_TIMEOUT_SOCKETS);
        status = httpGetStatus(conn);
        assert(status == 200 || status == 302);
        if (status != 200 && status != 302) {
            mprLog(0, "HTTP response status %d", status);
        }
        assert(httpGetError(conn) != 0);
        length = httpGetContentLength(conn);
        assert(length != 0);
    }
    httpDestroy(http);
}


#if BLD_FEATURE_SSL && (BLD_FEATURE_MATRIXSSL || BLD_FEATURE_OPENSSL)
static void testSecureHttpGet(MprTestGroup *gp)
{
    Http        *http;
    HttpConn    *conn;
    int         rc, status;

    http = httpCreate(gp);
    assert(http != 0);
    conn = httpCreateConn(http, NULL);

    rc = httpConnect(conn, "GET", "https://www.amazon.com/index.html");
    assert(rc >= 0);
    if (rc >= 0) {
        httpFinalize(conn);
        httpWait(conn, conn->dispatcher, HTTP_STATE_COMPLETE, MPR_TIMEOUT_SOCKETS);
        status = httpGetStatus(conn);
        assert(status == 200 || status == 301 || status == 302);
        if (status != 200 && status != 301 && status != 302) {
            mprLog(0, "HTTP response status %d", status);
        }
    }
    httpDestroy(http);
}
#endif


#if FUTURE && TODO
    mprSetHttpTimeout(http, timeout);
    mprSetHttpRetries(http, retries);
    mprSetHttpKeepAlive(http, on);
    mprSetHttpAuth(http, authType, realm, username, password);
    mprSetHttpHeader(http, "MyHeader: value");
    mprSetHttpDefaultHost(http, "localhost");
    mprSetHttpDefaultPort(http, 80);
    mprSetHttpBuffer(http, initial, max);
    mprSetHttpCallback(http, fn, arg);
    mprGetHttpHeader(http);
    url = mprGetHttpParsedUrl(http);
#endif


MprTestDef testHttpGen = {
    "http", 0, initHttp, 0,
    {
        MPR_TEST(0, testCreateHttp),
        MPR_TEST(0, testBasicHttpGet),
#if BLD_FEATURE_SSL && (BLD_FEATURE_MATRIXSSL || BLD_FEATURE_OPENSSL)
        MPR_TEST(0, testSecureHttpGet),
#endif
        MPR_TEST(0, 0),
    },
};

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
