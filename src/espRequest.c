/*
    espRequest.c -- ESP Request handler

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/

#include    "esp.h"

/************************************* Local **********************************/
/*
    Singleton ESP control structure
 */
static Esp *esp;

/*
    UNUSED. espRenderView flags are reserved
 */
#define ESP_DONT_RENDER 0x1

/************************************ Forward *********************************/

static int cloneDatabase(HttpConn *conn);
static void closeEsp(HttpQueue *q);
static cchar *getCacheName(HttpRoute *route, cchar *kind, cchar *source);
static void ifConfigModified(HttpRoute *route, cchar *path, bool *modified);
static void manageEsp(Esp *esp, int flags);
static void manageReq(EspReq *req, int flags);
static int openEsp(HttpQueue *q);
static int runAction(HttpConn *conn);
static void startEsp(HttpQueue *q);
static int unloadEsp(MprModule *mp);

#if !ME_STATIC
static int espLoadModule(HttpRoute *route, MprDispatcher *dispatcher, cchar *kind, cchar *source, cchar **errMsg,
    bool *loaded);
static cchar *getModuleName(HttpRoute *route, cchar *kind, cchar *target);
static char *getModuleEntry(EspRoute *eroute, cchar *kind, cchar *source, cchar *cacheName);
static bool layoutIsStale(EspRoute *eroute, cchar *source, cchar *module);
#endif

/************************************* Code ***********************************/
/*
    Load and initialize ESP module. Manually loaded when used inside esp.c.
 */
PUBLIC int espOpen(MprModule *module)
{
    HttpStage   *handler;

    if ((handler = httpCreateHandler("espHandler", module)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->espHandler = handler;
    handler->open = openEsp;
    handler->close = closeEsp;
    handler->start = startEsp;

    /*
        Using the standard 'incoming' callback that simply transfers input to the queue head
        Applications should read by defining a notifier for READABLE events and then calling httpGetPacket
        on the read queue.
     */
    if ((esp = mprAllocObj(Esp, manageEsp)) == 0) {
        return MPR_ERR_MEMORY;
    }
    MPR->espService = esp;
    handler->stageData = esp;
    esp->mutex = mprCreateLock();
    esp->local = mprCreateThreadLocal();
    if (espInitParser() < 0) {
        return 0;
    }
    if ((esp->ediService = ediCreateService()) == 0) {
        return 0;
    }
#if ME_COM_MDB
    mdbInit();
#endif
#if ME_COM_SQLITE
    sdbInit();
#endif
    if (module) {
        mprSetModuleFinalizer(module, unloadEsp);
    }
    return 0;
}


static int unloadEsp(MprModule *mp)
{
    HttpStage   *stage;

    if (esp->inUse) {
       return MPR_ERR_BUSY;
    }
    if (mprIsStopping()) {
        return 0;
    }
    if ((stage = httpLookupStage(mp->name)) != 0) {
        stage->flags |= HTTP_STAGE_UNLOADED;
    }
    return 0;
}


/*
    Open an instance of the ESP for a new request
 */
static int openEsp(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpRoute   *rp, *route;
    EspRoute    *eroute;
    EspReq      *req;
    char        *cookie;
    int         next;

    conn = q->conn;
    rx = conn->rx;
    route = rx->route;

    if ((req = mprAllocObj(EspReq, manageReq)) == 0) {
        httpMemoryError(conn);
        return MPR_ERR_MEMORY;
    }
    conn->reqData = req;

    /*
        If unloading a module, this lock will cause a wait here while ESP applications are reloaded.
     */
    lock(esp);
    esp->inUse++;
    unlock(esp);

    /*
        Find the ESP route configuration. Search up the route parent chain.
     */
    for (eroute = 0, rp = route; rp; rp = rp->parent) {
        if (rp->eroute) {
            eroute = rp->eroute;
            break;
        }
    }
    if (!eroute) {
        /* WARNING: may yield */
        if (espInit(route, 0, "esp.json") < 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
        eroute = route->eroute;
    } else {
        route->eroute = eroute;
    }
    req->esp = esp;
    req->route = route;
    req->autoFinalize = route->autoFinalize;

    /*
        If a cookie is not explicitly set, use the application name for the session cookie so that
        cookies are unique per esp application.
     */
    cookie = sfmt("esp-%s", eroute->appName);
    for (ITERATE_ITEMS(route->host->routes, rp, next)) {
        if (!rp->cookie) {
            httpSetRouteCookie(rp, cookie);
        }
    }
    return 0;
}


static void closeEsp(HttpQueue *q)
{
    lock(esp);
    esp->inUse--;
    assert(esp->inUse >= 0);
    unlock(esp);
}


#if !ME_STATIC
/*
    This is called when unloading a view or controller module
    All of ESP must be quiesced.
 */
static bool espUnloadModule(cchar *module, MprTicks timeout)
{
    MprModule   *mp;
    MprTicks    mark;

    if ((mp = mprLookupModule(module)) != 0) {
        mark = mprGetTicks();
        esp->reloading = 1;
        do {
            lock(esp);
            /* Own request will count as 1 */
            if (esp->inUse <= 1) {
                if (mprUnloadModule(mp) < 0) {
                    mprLog("error esp", 0, "Cannot unload module %s", mp->name);
                    unlock(esp);
                }
                esp->reloading = 0;
                unlock(esp);
                return 1;
            }
            unlock(esp);
            mprSleep(10);

        } while (mprGetRemainingTicks(mark, timeout) > 0);
        esp->reloading = 0;
    }
    return 0;
}
#endif


/*
    Not used
 */
PUBLIC void espClearFeedback(HttpConn *conn)
{
    EspReq      *req;

    req = conn->reqData;
    req->feedback = 0;
}


static void setupFeedback(HttpConn *conn)
{
    EspReq      *req;

    req = conn->reqData;
    req->lastFeedback = 0;
    if (req->route->json) {
        req->feedback = mprCreateHash(0, MPR_HASH_STABLE);
    } else {
        if (httpGetSession(conn, 0)) {
            req->feedback = httpGetSessionObj(conn, ESP_FEEDBACK_VAR);
            if (req->feedback) {
                httpRemoveSessionVar(conn, ESP_FEEDBACK_VAR);
                req->lastFeedback = mprCloneHash(req->feedback);
            }
        }
    }
}


static void finalizeFeedback(HttpConn *conn)
{
    EspReq  *req;
    MprKey  *kp, *lp;

    req = conn->reqData;
    if (req->feedback) {
        if (req->route->json) {
            if (req->lastFeedback) {
                for (ITERATE_KEYS(req->feedback, kp)) {
                    if ((lp = mprLookupKeyEntry(req->lastFeedback, kp->key)) != 0 && kp->data == lp->data) {
                        mprRemoveKey(req->feedback, kp->key);
                    }
                }
            }
            if (mprGetHashLength(req->feedback) > 0) {
                /*
                    If the session does not exist, this will create one. However, must not have
                    emitted the headers, otherwise cannot inform the client of the session cookie.
                */
                httpSetSessionObj(conn, ESP_FEEDBACK_VAR, req->feedback);
            }
        }
    }
}


/*
    Start the request. At this stage, body data may not have been fully received unless
    the request is a form (POST method and content type is application/x-www-form-urlencoded).
    Forms are a special case and delay invoking the start callback until all body data is received.
    WARNING: GC yield
 */
static void startEsp(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    EspReq      *req;

    conn = q->conn;
    rx = conn->rx;
    req = conn->reqData;

#if ME_WIN_LIKE
    rx->target = mprGetPortablePath(rx->target);
#endif

    if (req) {
        mprSetThreadData(req->esp->local, conn);
        /* WARNING: GC yield */
        if (runAction(conn)) {
            if (!conn->error && req->autoFinalize) {
                if (!conn->tx->responded) {
                    /* WARNING: GC yield */
                    espRenderDocument(conn, rx->target);
                }
                if (req->autoFinalize) {
                    espFinalize(conn);
                }
            }
        }
        finalizeFeedback(conn);
        mprSetThreadData(req->esp->local, NULL);
    }
}


static bool loadController(HttpConn *conn)
{
#if !ME_STATIC
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    cchar       *errMsg, *controllers, *controller;
    bool        loaded;

    rx = conn->rx;
    route = rx->route;
    eroute = route->eroute;
    if (!eroute->combine && (eroute->update || !mprLookupKey(eroute->actions, rx->target))) {
        if ((controllers = httpGetDir(route, "CONTROLLERS")) == 0) {
            controllers = ".";
        }
        controllers = mprJoinPath(route->home, controllers);
        controller = schr(route->sourceName, '$') ? stemplateJson(route->sourceName, rx->params) : route->sourceName;
        controller = controllers ? mprJoinPath(controllers, controller) : mprJoinPath(route->home, controller);

        if (espLoadModule(route, conn->dispatcher, "controller", controller, &errMsg, &loaded) < 0) {
            if (mprPathExists(controller, R_OK)) {
                httpError(conn, HTTP_CODE_NOT_FOUND, "%s", errMsg);
                return 0;
            }
        } else if (loaded) {
            httpTrace(conn, "esp.handler", "context", "msg: 'Load module %s'", controller);
        }
    }
#endif /* !ME_STATIC */
    return 1;
}


static bool setToken(HttpConn *conn)
{
    HttpRx      *rx;
    HttpRoute   *route;

    rx = conn->rx;
    route = rx->route;

    if (route->flags & HTTP_ROUTE_XSRF && !(rx->flags & HTTP_GET)) {
        if (!httpCheckSecurityToken(conn)) {
            httpSetStatus(conn, HTTP_CODE_UNAUTHORIZED);
            if (route->json) {
                httpTrace(conn, "esp.xsrf.error", "error", 0);
                espRenderString(conn,
                    "{\"retry\": true, \"success\": 0, \"feedback\": {\"error\": \"Security token is stale. Please retry.\"}}");
                espFinalize(conn);
            } else {
                httpError(conn, HTTP_CODE_UNAUTHORIZED, "Security token is stale. Please reload page.");
            }
            return 0;
        }
    }
    return 1;
}


/*
    Run an action (may yield)
 */
static int runAction(HttpConn *conn)
{
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    EspReq      *req;
    EspAction   action;

    rx = conn->rx;
    req = conn->reqData;
    route = rx->route;
    eroute = route->eroute;
    assert(eroute);

    if (eroute->edi && eroute->edi->flags & EDI_PRIVATE) {
        cloneDatabase(conn);
    } else {
        req->edi = eroute->edi;
    }
    if (route->sourceName == 0 || *route->sourceName == '\0') {
        if (eroute->commonController) {
            (eroute->commonController)(conn);
        }
        return 1;
    }
    if (!loadController(conn)) {
        return 0;
    }
    if (!setToken(conn)) {
        return 0;
    }
    httpAuthenticate(conn);
    if (eroute->commonController) {
        (eroute->commonController)(conn);
    }
    assert(eroute->top);
    action = mprLookupKey(eroute->top->actions, rx->target);
    if (action) {
        httpTrace(conn, "esp.handler", "context", "msg: 'Invoke controller action %s'", rx->target);
        setupFeedback(conn);
        if (!httpIsFinalized(conn)) {
            (action)(conn);
        }
    }
    return 1;
}


static cchar *loadView(HttpConn *conn, cchar *target)
{
#if !ME_STATIC
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    bool        loaded;
    cchar       *errMsg, *path;

    rx = conn->rx;
    route = rx->route;
    eroute = route->eroute;
    assert(eroute);

    if (!eroute->combine && (eroute->update || !mprLookupKey(eroute->top->views, target))) {
        /* WARNING: GC yield - need target below */
        target = sclone(target);
        mprHold(target);
        path = mprJoinPath(route->documents, target);
        if (espLoadModule(route, conn->dispatcher, "view", path, &errMsg, &loaded) < 0) {
            httpError(conn, HTTP_CODE_NOT_FOUND, "%s", errMsg);
            mprRelease(target);
            return 0;
        }
        if (loaded) {
            httpTrace(conn, "esp.handler", "context", "msg: 'Load module %s'", path);
        }
        mprRelease(target);
    }
#endif
    return target;
}

PUBLIC bool espRenderView(HttpConn *conn, cchar *target, int flags)
{
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    EspViewProc viewProc;

    rx = conn->rx;
    route = rx->route;
    eroute = route->eroute;

    if ((target = loadView(conn, target)) == 0) {
        return 0;
    }
    if ((viewProc = mprLookupKey(eroute->top->views, target)) == 0) {
        httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot find function %s for %s",
            getCacheName(route, "view", mprJoinPath(route->documents, target)), target);
        return 0;
    }
    if (!(flags & ESP_DONT_RENDER)) {
        if (route->flags & HTTP_ROUTE_XSRF) {
            /* Add a new unique security token */
            httpAddSecurityToken(conn, 1);
        }
        httpSetContentType(conn, "text/html");
        httpSetFilename(conn, mprJoinPath(route->documents, target), 0);
        /* WARNING: may GC yield */
        (viewProc)(conn);
    }
    return 1;
}


/*
    Check if the target/filename.ext is registered as an esp view
 */
static cchar *checkView(HttpConn *conn, cchar *target, cchar *filename, cchar *ext)
{
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;

    assert(target);

    rx = conn->rx;
    route = rx->route;
    eroute = route->eroute;

    if (filename) {
        target = mprJoinPath(target, filename);
    }
    assert(target && *target);

    if (ext && *ext) {
        if (!smatch(mprGetPathExt(target), ext)) {
            target = sjoin(target, ".", ext, NULL);
        }
    }
    /*
        See if module already loaded for this view
     */
    if (mprLookupKey(eroute->top->views, target)) {
        return target;
    }

#if !ME_STATIC
{
    MprPath info;
    cchar   *module, *path;

    path = mprJoinPath(route->documents, target);

    if (!eroute->combine) {
        /*
            If target exists as a cached module that has not yet been loaded
            Note: source may not be present.
         */
        module = getModuleName(route, "view", path);
        if (mprGetPathInfo(module, &info) == 0 && !info.isDir) {
            return target;
        }
    }

    /*
        If target exists as a view (extension appended above)
     */
    if (mprGetPathInfo(path, &info) == 0 && !info.isDir) {
        return target;
    }

#if UNUSED
    /*
        If target exists as a mapped / compressed view
     */
    if (route->map && !(conn->tx->flags & HTTP_TX_NO_MAP)) {
        path = httpMapContent(conn, path);
        if (mprGetPathInfo(path, &info) == 0 && !info.isDir) {
            return target;
        }
    }
#endif

#if DEPRECATED || 1
    /*
        See if views are under client/app. Remove in version 6.
     */
    path = mprJoinPaths(route->documents, "app", target, NULL);
    if (mprGetPathInfo(path, &info) == 0 && !info.isDir) {
        return mprJoinPath("app", target);
    }
#endif
}
#endif
    return 0;
}


/*
    Render a document by mapping a URL target to a document. The target is interpreted relative to route->documents.
    If target + defined extension exists, serve that.
    If target is a directory with an index.esp, return the index.esp without a redirect.
    If target is a directory without a trailing "/" but with an index.esp, do an external redirect to "URI/".
    Otherwise relay to the fileHandler.
 */
PUBLIC void espRenderDocument(HttpConn *conn, cchar *target)
{
    HttpUri     *up;
    MprKey      *kp;
    cchar       *dest;

    if (!target) {
        httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot find document");
        return;
    }
    if (*target) {
        for (ITERATE_KEYS(conn->rx->route->extensions, kp)) {
            if (kp->data == HTTP->espHandler && kp->key && kp->key[0]) {
                if ((dest = checkView(conn, target, 0, kp->key)) != 0) {
                    httpTrace(conn, "esp.handler", "context", "msg: 'Render view %s'", dest);
                    espRenderView(conn, dest, 0);
                    return;
                }
            }
        }
    }
    /*
        Check for index
     */
    if ((dest = checkView(conn, target, "index", "esp")) != 0) {
        /*
            Must do external redirect first if URL does not end with "/"
         */
        if (!sends(conn->rx->parsedUri->path, "/")) {
            up = conn->rx->parsedUri;
            httpRedirect(conn, HTTP_CODE_MOVED_PERMANENTLY, httpFormatUri(up->scheme, up->host,
                up->port, sjoin(up->path, "/", NULL), up->reference, up->query, 0));
            return;
        }
        httpTrace(conn, "esp.handler", "context", "msg: 'Render index %s'", dest);
        espRenderView(conn, dest, 0);
        return;
    }

#if UNUSED
    /*
        Last chance, forward to the file handler ... not an ESP request. This enables file requests within ESP routes.
     */
    path = mprJoinPath(route->documents, target);
    if (mprGetPathInfo(path, &info) == 0 && !info.isDir) {
        target = path;
    }
    /*
        If target exists as a mapped / compressed document
     */
    if (route->map && !(conn->tx->flags & HTTP_TX_NO_MAP)) {
        path = httpMapContent(conn, path);
        if (mprGetPathInfo(path, &info) == 0 && !info.isDir) {
            target = path;
        }
    }
#endif

    conn->rx->target = sclone(&conn->rx->pathInfo[1]);
    httpMapFile(conn);
    if (conn->tx->fileInfo.isDir) {
        httpHandleDirectory(conn);
    }
    if (!conn->tx->finalized) {
        httpSetFileHandler(conn, 0);
    }
}


/************************************ Support *********************************/
/*
    Create a per user session database clone.
    Used for demos so one users updates to not change anothers view of the database.
 */
static void pruneDatabases(Esp *esp)
{
    MprKey      *kp;

    lock(esp);
    for (ITERATE_KEYS(esp->databases, kp)) {
        if (!httpLookupSessionID(kp->key)) {
            mprRemoveKey(esp->databases, kp->key);
            /* Restart scan */
            kp = 0;
        }
    }
    unlock(esp);
}


/*
    This clones a database to give a private view per user.
 */
static int cloneDatabase(HttpConn *conn)
{
    Esp         *esp;
    EspRoute    *eroute;
    EspReq      *req;
    cchar       *id;

    req = conn->reqData;
    eroute = conn->rx->route->eroute;
    assert(eroute->edi);
    assert(eroute->edi->flags & EDI_PRIVATE);

    esp = req->esp;
    if (!esp->databases) {
        lock(esp);
        if (!esp->databases) {
            esp->databases = mprCreateHash(0, 0);
            esp->databasesTimer = mprCreateTimerEvent(NULL, "esp-databases", 60 * 1000, pruneDatabases, esp, 0);
        }
        unlock(esp);
    }
    /*
        If the user is logging in or out, this will create a redundant session here.
     */
    httpGetSession(conn, 1);
    id = httpGetSessionID(conn);
    if (id && (req->edi = mprLookupKey(esp->databases, id)) == 0) {
        if ((req->edi = ediClone(eroute->edi)) == 0) {
            mprLog("error esp", 0, "Cannot clone database: %s", eroute->edi->path);
            return MPR_ERR_CANT_OPEN;
        }
        mprAddKey(esp->databases, id, req->edi);
    }
    return 0;
}


static cchar *getCacheName(HttpRoute *route, cchar *kind, cchar *target)
{
    EspRoute    *eroute;
    cchar       *appName, *canonical;

    eroute = route->eroute;
#if VXWORKS
    /*
        Trim the drive for VxWorks where simulated host drives only exist on the target
     */
    target = mprTrimPathDrive(target);
#endif
    canonical = mprGetPortablePath(mprGetRelPath(target, route->home));

    appName = eroute->appName;
    return eroute->combine ? appName : mprGetMD5WithPrefix(sfmt("%s:%s", appName, canonical), -1, sjoin(kind, "_", NULL));
}


#if !ME_STATIC
static char *getModuleEntry(EspRoute *eroute, cchar *kind, cchar *source, cchar *cacheName)
{
    char    *cp, *entry;

    if (smatch(kind, "view")) {
        entry = sfmt("esp_%s", cacheName);

    } else if (smatch(kind, "app")) {
        if (eroute->combine) {
            entry = sfmt("esp_%s_%s_combine", kind, eroute->appName);
        } else {
            entry = sfmt("esp_%s_%s", kind, eroute->appName);
        }
    } else {
        /* Controller */
        if (eroute->appName) {
            entry = sfmt("esp_%s_%s_%s", kind, eroute->appName, mprTrimPathExt(mprGetPathBase(source)));
        } else {
            entry = sfmt("esp_%s_%s", kind, mprTrimPathExt(mprGetPathBase(source)));
        }
    }
    for (cp = entry; *cp; cp++) {
        if (!isalnum((uchar) *cp) && *cp != '_') {
            *cp = '_';
        }
    }
    return entry;
}


static cchar *getModuleName(HttpRoute *route, cchar *kind, cchar *target)
{
    cchar   *cache, *cacheName;

    cacheName = getCacheName(route, "view", target);
    if ((cache = httpGetDir(route, "CACHE")) == 0) {
        /* May not be set for non esp apps */
        cache = "cache";
    }
    return mprJoinPathExt(mprJoinPaths(route->home, cache, cacheName, NULL), ME_SHOBJ);
}


/*
    WARNING: GC yield
 */
static int espLoadModule(HttpRoute *route, MprDispatcher *dispatcher, cchar *kind, cchar *source, cchar **errMsg,
    bool *loaded)
{
    EspRoute    *eroute;
    MprModule   *mp;
    cchar       *cache, *cacheName, *entry, *module;
    int         isView, recompile;

    eroute = route->eroute;
    *errMsg = "";

    if (loaded) {
        *loaded = 0;
    }
    cacheName = getCacheName(route, kind, source);
    if ((cache = httpGetDir(route, "CACHE")) == 0) {
        cache = "cache";
    }
    module = mprJoinPathExt(mprJoinPaths(route->home, cache, cacheName, NULL), ME_SHOBJ);

    lock(esp);
    if (mprLookupModule(source) == 0 || eroute->update) {
        espModuleIsStale(route, source, module, &recompile);
        if (eroute->compile && mprPathExists(source, R_OK)) {
            isView = smatch(kind, "view");
            if (recompile || (isView && layoutIsStale(eroute, source, module))) {
                if (recompile) {
                    mprHoldBlocks(source, module, cacheName, NULL);
                    if (!espCompile(route, dispatcher, source, module, cacheName, isView, (char**) errMsg)) {
                        mprReleaseBlocks(source, module, cacheName, NULL);
                        unlock(esp);
                        return MPR_ERR_CANT_WRITE;
                    }
                    mprReleaseBlocks(source, module, cacheName, NULL);
                }
            }
        }
    }
    if (mprLookupModule(source) == 0) {
        if (!mprPathExists(module, R_OK)) {
            *errMsg = sfmt("Module does not exist: %s", module);
            unlock(esp);
            return MPR_ERR_CANT_FIND;
        }
        entry = getModuleEntry(eroute, kind, source, cacheName);
        if ((mp = mprCreateModule(source, module, entry, route)) == 0) {
            *errMsg = "Memory allocation error loading module";
            unlock(esp);
            return MPR_ERR_MEMORY;
        }
        if (mprLoadModule(mp) < 0) {
            *errMsg = sfmt("Cannot load compiled esp module: %s", module);
            unlock(esp);
            return MPR_ERR_CANT_READ;
        }
        if (loaded) {
            *loaded = 1;
        }
    }
    unlock(esp);
    return 0;
}


/*
    Test if a module has been updated (is stale).
    This will unload the module if it loaded but stale.
    Set recompile to true if the source is absent or more recent.
    Will return false if the source does not exist (important for testing layouts).
 */
PUBLIC bool espModuleIsStale(HttpRoute *route, cchar *source, cchar *module, int *recompile)
{
    EspRoute    *eroute;
    MprModule   *mp;
    MprPath     sinfo, minfo;

    *recompile = 0;
    eroute = route->eroute;

    mprGetPathInfo(module, &minfo);
    if (!minfo.valid) {
        if ((mp = mprLookupModule(source)) != 0) {
            if (!espUnloadModule(source, ME_ESP_RELOAD_TIMEOUT)) {
                mprLog("error esp", 0, "Cannot unload module %s. Connections still open. Continue using old version.",
                    source);
                return 0;
            }
        }
        if (eroute->compile) {
            *recompile = 1;
            /* Module not loaded */
        }
        return 1;
    }
    if (eroute->compile) {
        mprGetPathInfo(source, &sinfo);
        if (sinfo.valid && sinfo.mtime > minfo.mtime) {
            if ((mp = mprLookupModule(source)) != 0) {
                if (!espUnloadModule(source, ME_ESP_RELOAD_TIMEOUT)) {
                    mprLog("warn esp", 4, "Cannot unload module %s. Connections still open. Continue using old version.",
                        source);
                    return 0;
                }
            }
            *recompile = 1;
            mprLog("info esp", 4, "Source %s is newer than module %s, recompiling ...", source, module);
            return 1;
        }
    }
    if ((mp = mprLookupModule(source)) != 0) {
        if (minfo.mtime > mp->modified) {
            /* Module file has been updated */
            if (!espUnloadModule(source, ME_ESP_RELOAD_TIMEOUT)) {
                mprLog("warn esp", 4, "Cannot unload module %s. Connections still open. Continue using old version.",
                    source);
                return 0;
            }
            mprLog("info esp", 4, "Module %s has been externally updated, reloading ...", module);
            return 1;
        }
    }
    /* Loaded module is current */
    return 0;
}


/*
    Check if the layout has changed. Returns false if the layout does not exist.
 */
static bool layoutIsStale(EspRoute *eroute, cchar *source, cchar *module)
{
    char    *data, *lpath, *quote;
    cchar   *layout, *layoutsDir;
    ssize   len;
    bool    stale;
    int     recompile;

    stale = 0;
    layoutsDir = httpGetDir(eroute->route, "LAYOUTS");
    if ((data = mprReadPathContents(source, &len)) != 0) {
        if ((lpath = scontains(data, "@ layout \"")) != 0) {
            lpath = strim(&lpath[10], " ", MPR_TRIM_BOTH);
            if ((quote = schr(lpath, '"')) != 0) {
                *quote = '\0';
            }
            layout = (layoutsDir && *lpath) ? mprJoinPath(layoutsDir, lpath) : 0;
        } else {
            layout = (layoutsDir) ? mprJoinPath(layoutsDir, "default.esp") : 0;
        }
        if (layout) {
            stale = espModuleIsStale(eroute->route, layout, module, &recompile);
            if (stale) {
                mprLog("info esp", 4, "esp layout %s is newer than module %s", layout, module);
            }
        }
    }
    return stale;
}
#else

PUBLIC bool espModuleIsStale(HttpRoute *route, cchar *source, cchar *module, int *recompile)
{
    return 0;
}
#endif /* ME_STATIC */


/************************************ Esp Route *******************************/
/*
    Public so that esp.c can also call
 */
PUBLIC void espManageEspRoute(EspRoute *eroute, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(eroute->actions);
        mprMark(eroute->appName);
        mprMark(eroute->compileCmd);
        mprMark(eroute->configFile);
        mprMark(eroute->currentSession);
        mprMark(eroute->edi);
        mprMark(eroute->env);
        mprMark(eroute->linkCmd);
        mprMark(eroute->searchPath);
        mprMark(eroute->top);
        mprMark(eroute->views);
        mprMark(eroute->winsdk);
#if DEPRECATED || 1
        mprMark(eroute->combineScript);
        mprMark(eroute->combineSheet);
#endif
    }
}


PUBLIC EspRoute *espCreateRoute(HttpRoute *route)
{
    EspRoute    *eroute;

    if ((eroute = mprAllocObj(EspRoute, espManageEspRoute)) == 0) {
        return 0;
    }
    route->eroute = eroute;
    eroute->route = route;
    eroute->compile = 1;
    eroute->keep = 0;
    eroute->update = 1;
    eroute->compileMode = ESP_COMPILE_SYMBOLS;

    if (route->parent && route->parent->eroute) {
        eroute->top = ((EspRoute*) route->parent->eroute)->top;
    } else {
        eroute->top = eroute;
    }
    eroute->appName = sclone("app");
    return eroute;
}


static EspRoute *cloneEspRoute(HttpRoute *route, EspRoute *parent)
{
    EspRoute      *eroute;

    assert(parent);
    assert(route);

    if ((eroute = mprAllocObj(EspRoute, espManageEspRoute)) == 0) {
        return 0;
    }
    eroute->route = route;
    eroute->top = parent->top;
    eroute->searchPath = parent->searchPath;
    eroute->configFile = parent->configFile;
    eroute->edi = parent->edi;
    eroute->commonController = parent->commonController;
    if (parent->compileCmd) {
        eroute->compileCmd = sclone(parent->compileCmd);
    }
    if (parent->linkCmd) {
        eroute->linkCmd = sclone(parent->linkCmd);
    }
    if (parent->env) {
        eroute->env = mprCloneHash(parent->env);
    }
    eroute->appName = parent->appName;
    eroute->combine = parent->combine;
    eroute->compile = parent->compile;
    eroute->keep = parent->keep;
    eroute->update = parent->update;
#if DEPRECATED || 1
    eroute->combineScript = parent->combineScript;
    eroute->combineSheet = parent->combineSheet;
#endif
    route->eroute = eroute;
    return eroute;
}


/*
    Get an EspRoute. Allocate if required.
    It is expected that the caller will modify the EspRoute.
 */
PUBLIC EspRoute *espRoute(HttpRoute *route, bool create)
{
    HttpRoute   *rp;

    if (route->eroute && (!route->parent || route->parent->eroute != route->eroute)) {
        return route->eroute;
    }
    /*
        Lookup up the route chain for any configured EspRoutes to clone
     */
    for (rp = route; rp; rp = rp->parent) {
        if (rp->eroute) {
            return cloneEspRoute(route, rp->eroute);
        }
        if (rp->parent == 0 && create) {
            /*
                Create an ESP configuration on the top level parent so others can inherit
                Load the compiler rules once for all
             */
            espCreateRoute(rp);
            if (rp != route) {
                espInit(rp, 0, "esp.json");
            }
            break;
        }
    }
    if (rp) {
        return cloneEspRoute(route, rp->eroute);
    }
    return 0;
}


/*
    Manage all links for EspReq for the garbage collector
 */
static void manageReq(EspReq *req, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(req->commandLine);
        mprMark(req->data);
        mprMark(req->edi);
        mprMark(req->feedback);
        mprMark(req->lastFeedback);
        mprMark(req->route);
    }
}


/*
    Manage all links for Esp for the garbage collector
 */
static void manageEsp(Esp *esp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(esp->databases);
        mprMark(esp->databasesTimer);
        mprMark(esp->ediService);
        mprMark(esp->internalOptions);
        mprMark(esp->local);
        mprMark(esp->mutex);
        mprMark(esp->vstudioEnv);
        mprMark(esp->hostedDocuments);
    }
}

/*********************************** Directives *******************************/
/*
    Load the ESP configuration file esp.json (eroute->configFile) and an optional pak.json file
    WARNING: may yield
 */
PUBLIC int espLoadConfig(HttpRoute *route)
{
    EspRoute    *eroute;
    HttpRoute   *rp;
    cchar       *cookie, *home, *name, *package;
    int         next;
    bool        modified;

    eroute = route->eroute;
    if (!eroute) {
        mprLog("esp error", 0, "Cannot find esp configuration when loading config");
        return MPR_ERR_CANT_LOAD;
    }
    if (eroute->loaded && !eroute->update) {
        return 0;
    }
    home = eroute->configFile ? mprGetPathDir(eroute->configFile) : route->home;
    package = mprJoinPath(home, "pak.json");
    modified = 0;
    ifConfigModified(route, eroute->configFile, &modified);
    ifConfigModified(route, package, &modified);

    if (modified) {
        lock(esp);
        httpInitConfig(route);
        if (mprPathExists(package, R_OK) && !mprSamePath(package, eroute->configFile)) {
            if (httpLoadConfig(route, package) < 0) {
                unlock(esp);
                return MPR_ERR_CANT_LOAD;
            }
        }
        if (httpLoadConfig(route, eroute->configFile) < 0) {
            unlock(esp);
            return MPR_ERR_CANT_LOAD;
        }
        if ((name = espGetConfig(route, "name", 0)) != 0) {
            eroute->appName = name;
        }
        cookie = sfmt("esp-%s", eroute->appName);
        for (ITERATE_ITEMS(route->host->routes, rp, next)) {
            if (!rp->cookie) {
                httpSetRouteCookie(rp, cookie);
            }
        }
        unlock(esp);
    }
    if (!httpGetDir(route, "CACHE")) {
        espSetDefaultDirs(route, 0);
    }
    return 0;
}


/*
    Preload application module
    WARNING: may yield when compiling modules
 */
static bool preload(HttpRoute *route)
{
#if !ME_STATIC
    EspRoute    *eroute;
    MprJson     *preload, *item;
    cchar       *errMsg, *source;
    char        *kind;
    int         i;

    eroute = route->eroute;
    if (eroute->app && !(route->flags & HTTP_ROUTE_NO_LISTEN)) {
        if (eroute->combine) {
            source = mprJoinPaths(route->home, httpGetDir(route, "CACHE"), sfmt("%s.c", eroute->appName), NULL);
        } else {
            source = mprJoinPaths(route->home, httpGetDir(route, "SRC"), "app.c", NULL);
        }
        if (espLoadModule(route, NULL, "app", source, &errMsg, NULL) < 0) {
            if (eroute->combine) {
                mprLog("error esp", 0, "%s", errMsg);
                return 0;
            }
        }
        if (!eroute->combine && (preload = mprGetJsonObj(route->config, "esp.preload")) != 0) {
            for (ITERATE_JSON(preload, item, i)) {
                source = ssplit(sclone(item->value), ":", &kind);
                if (*kind == '\0') {
                    kind = "controller";
                }
                source = mprJoinPaths(route->home, httpGetDir(route, "CONTROLLERS"), source, NULL);
                if (espLoadModule(route, NULL, kind, source, &errMsg, NULL) < 0) {
                    mprLog("error esp", 0, "Cannot preload esp module %s. %s", source, errMsg);
                    return 0;
                }
            }
        }
    }
#endif
    return 1;
}


/*
    Initialize ESP.
    Prefix is the URI prefix for the application
    Path is the path to the esp.json
 */
PUBLIC int espInit(HttpRoute *route, cchar *prefix, cchar *path)
{
    EspRoute    *eroute;
    cchar       *hostname;
    bool        yielding;

    if (!route) {
        return MPR_ERR_BAD_ARGS;
    }
    lock(esp);
    if ((eroute = espRoute(route, 0)) == 0) {
        eroute = espCreateRoute(route);
    }
    if (prefix) {
        if (*prefix != '/') {
            prefix = sjoin("/", prefix, NULL);
        }
        prefix = stemplate(prefix, route->vars);
        httpSetRoutePrefix(route, prefix);
        httpSetRoutePattern(route, sfmt("^%s", prefix), 0);
        hostname = route->host->name ? route->host->name : "default";
        mprLog("info esp", 3, "Load ESP app: %s%s from %s", hostname, prefix, path);
    }
    eroute->top = eroute;
    if (path && mprPathExists(path, R_OK)) {
        httpSetRouteHome(route, mprGetPathDir(path));
        eroute->configFile = sclone(path);
    }
    if (!route->handler) {
        httpAddRouteHandler(route, "espHandler", "esp");
    }
    /*
        Loading config may run commands. To make it easier for parsing code, we disable GC by not consenting to
        yield for this section. This should only happen on application load.
     */
    yielding = mprSetThreadYield(NULL, 0);
    if (espLoadConfig(route) < 0) {
        mprSetThreadYield(NULL, yielding);
        unlock(esp);
        return MPR_ERR_CANT_LOAD;
    }
    if (eroute->compile && espLoadCompilerRules(route) < 0) {
        mprSetThreadYield(NULL, yielding);
        unlock(esp);
        return MPR_ERR_CANT_OPEN;
    }
    if (route->database && !eroute->edi && espOpenDatabase(route, route->database) < 0) {
        mprLog("error esp", 0, "Cannot open database %s", route->database);
        mprSetThreadYield(NULL, yielding);
        unlock(esp);
        return MPR_ERR_CANT_LOAD;
    }
    if (!preload(route)) {
        mprSetThreadYield(NULL, yielding);
        unlock(esp);
        return MPR_ERR_CANT_LOAD;
    }
    mprSetThreadYield(NULL, yielding);
    unlock(esp);
    return 0;
}


PUBLIC int espOpenDatabase(HttpRoute *route, cchar *spec)
{
    EspRoute    *eroute;
    char        *provider, *path, *dir;
    int         flags;

    eroute = route->eroute;
    if (eroute->edi) {
        return 0;
    }
    flags = EDI_CREATE | EDI_AUTO_SAVE;
    if (smatch(spec, "default")) {
#if ME_COM_SQLITE
        spec = sfmt("sdb://%s.sdb", eroute->appName);
#elif ME_COM_MDB
        spec = sfmt("mdb://%s.mdb", eroute->appName);
#endif
    }
    provider = ssplit(sclone(spec), "://", &path);
    if (*provider == '\0' || *path == '\0') {
        return MPR_ERR_BAD_ARGS;
    }
    path = mprJoinPaths(route->home, httpGetDir(route, "DB"), path, NULL);
    dir = mprGetPathDir(path);
    if (!mprPathExists(dir, X_OK)) {
        mprMakeDir(dir, 0755, -1, -1, 1);
    }
    if ((eroute->edi = ediOpen(mprGetRelPath(path, NULL), provider, flags)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    route->database = sclone(spec);
    return 0;
}


static void setDir(HttpRoute *route, cchar *key, cchar *value, bool force)
{
    if (force) {
        httpSetDir(route, key, value);
    } else if (!httpGetDir(route, key)) {
        httpSetDir(route, key, value);
    }
}


PUBLIC void espSetDefaultDirs(HttpRoute *route, bool app)
{
    cchar   *controllers, *documents, *path, *migrations;

    documents = mprJoinPath(route->home, "dist");
    /*
        Consider keeping documents, web and public
     */
    if (!mprPathExists(documents, X_OK)) {
        documents = mprJoinPath(route->home, "documents");
        if (!mprPathExists(documents, X_OK)) {
            documents = mprJoinPath(route->home, "web");
            if (!mprPathExists(documents, X_OK)) {
                documents = mprJoinPath(route->home, "client");
                if (!mprPathExists(documents, X_OK)) {
                    documents = mprJoinPath(route->home, "public");
                    if (!mprPathExists(documents, X_OK)) {
#if ME_APPWEB_PRODUCT
                        if (!esp->hostedDocuments && mprPathExists("install.conf", R_OK)) {
                            /*
                                This returns the documents directory of the default route of the default host
                                When Appweb switches to appweb.json, then just it should be loaded with pak.json
                             */
                            char *output;
                            bool yielding = mprSetThreadYield(NULL, 0);
                            if (mprRun(NULL, "appweb --show-documents", NULL, (char**) &output, NULL, 5000) == 0) {
                                documents = esp->hostedDocuments = strim(output, "\n", MPR_TRIM_END);
                            } else {
                                documents = route->home;
                            }
                            mprSetThreadYield(NULL, yielding);
                        } else
#endif
                        {
                            documents = route->home;
                        }
                    }
                }
            }
        }
    }

    /*
        Detect if a controllers directory exists. Set controllers to "." if absent.
     */
    controllers = "controllers";
    path = mprJoinPath(route->home, controllers);
    if (!mprPathExists(path, X_OK)) {
        controllers = ".";
    }

    migrations = "db/migrations";
    path = mprJoinPath(route->home, migrations);
    if (!mprPathExists(path, X_OK)) {
        migrations = "migrations";
    }
    setDir(route, "CACHE", 0, app);
    setDir(route, "CONTROLLERS", controllers, app);
    setDir(route, "CONTENTS", 0, app);
    setDir(route, "DB", 0, app);
    setDir(route, "DOCUMENTS", documents, app);
    setDir(route, "HOME", route->home, app);
    setDir(route, "LAYOUTS", 0, app);
    setDir(route, "LIB", 0, app);
    setDir(route, "MIGRATIONS", migrations, app);
    setDir(route, "PAKS", 0, app);
    setDir(route, "PARTIALS", 0, app);
    setDir(route, "SRC", 0, app);
    setDir(route, "UPLOAD", "/tmp", app);
}


/*
    Initialize and load a statically linked ESP module
 */
PUBLIC int espStaticInitialize(EspModuleEntry entry, cchar *appName, cchar *routeName)
{
    HttpRoute   *route;

    if ((route = httpLookupRoute(NULL, routeName)) == 0) {
        mprLog("error esp", 0, "Cannot find route %s", routeName);
        return MPR_ERR_CANT_ACCESS;
    }
    return (entry)(route, NULL);
}


PUBLIC int espBindProc(HttpRoute *parent, cchar *pattern, void *proc)
{
    HttpRoute   *route;
    EspRoute    *eroute;

    if ((route = httpDefineRoute(parent, "ALL", pattern, "$&", "unused")) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    httpSetRouteHandler(route, "espHandler");
    espDefineAction(route, pattern, proc);
    eroute = route->eroute;
    eroute->update = 0;
    return 0;
}


static void ifConfigModified(HttpRoute *route, cchar *path, bool *modified)
{
    EspRoute    *eroute;
    MprPath     info;

    if (path) {
        eroute = route->eroute;
        mprGetPathInfo(path, &info);
        if (info.mtime > eroute->loaded) {
            *modified = 1;
            eroute->loaded = info.mtime;
        }
    }
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
