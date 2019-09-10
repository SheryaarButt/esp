/*
    caching.c - Test caching

    Assumes configuration of: LimitCache 64K, CacheItem 16K
 */
#include "esp.h"

//  This is configured for caching by API below
static void api() {
    render("{ when: %lld, uri: '%s', query: '%s' }\r\n", mprGetTicks(), getUri(), getQuery());
}

static void sml() {
    int     i;
    for (i = 0; i < 1; i++) {
        render("Line: %05d %s", i, "aaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccccddddddd<br/>\r\n");
        mprYield(0);
    }
    render("{ when: %lld, uri: '%s', query: '%s' }\r\n", mprGetTicks(), getUri(), getQuery());
}

static void medium() {
    int     i;
    //  This will emit ~8K (under the item limit)
    for (i = 0; i < 100; i++) {
        render("Line: %05d %s", i, "aaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccccddddddd<br/>\r\n");
        mprYield(0);
    }
    render("{ when: %lld, uri: '%s', query: '%s' }\r\n", mprGetTicks(), getUri(), getQuery());
}

static void big() {
    int     i;
    //  This will emit ~80K (under the item limit)
    for (i = 0; i < 1000; i++) {
        render("Line: %05d %s", i, "aaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccccddddddd<br/>\r\n");
        mprYield(0);
    }
}

static void huge() {
    int     i;
    //  This will emit ~800K (over the item limit)
    for (i = 0; i < 10000; i++) {
        render("Line: %05d %s", i, "aaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccccddddddd<br/>\r\n");
        mprYield(0);
    }
    render("{ when: %lld, uri: '%s', query: '%s' }\r\n", mprGetTicks(), getUri(), getQuery());
}

static void clear() {
    espUpdateCache(getStream(), "/caching/manual", 0, 0);
    espUpdateCache(getStream(), "/caching/big", 0, 0);
    espUpdateCache(getStream(), "/caching/medium", 0, 0);
    espUpdateCache(getStream(), "/caching/small", 0, 0);
    espUpdateCache(getStream(), "/caching/api", 0, 0);
    espUpdateCache(getStream(), "/caching/api", 0, 0);
    render("cleared");
}

static void client() {
    render("{ when: %lld, uri: '%s', query: '%s' }\r\n", mprGetTicks(), getUri(), getQuery());
}

static void manual() {
    if (smatch(getQuery(), "send")) {
        setHeader("X-SendCache", "true");
        finalize();
    } else if (!espRenderCached(getStream())) {
        render("{ when: %lld, uri: '%s', query: '%s' }\r\n", mprGetTicks(), getUri(), getQuery());
    }
}

static void update() {
    cchar   *data = sfmt("{ when: %lld, uri: '%s', query: '%s' }\r\n", mprGetTicks(), getUri(), getQuery());
    espUpdateCache(getStream(), "/caching/manual", data, 86400);
    render("done");
}

ESP_EXPORT int esp_controller_esptest_caching(HttpRoute *route, MprModule *module) {
    HttpRoute   *rp;

    espAction(route, "caching/api", NULL, api);
    espAction(route, "caching/big", NULL, big);
    espAction(route, "caching/small", NULL, sml);
    espAction(route, "caching/medium", NULL, medium);
    espAction(route, "caching/clear", NULL, clear);
    espAction(route, "caching/client", NULL, client);
    espAction(route, "caching/huge", NULL, huge);
    espAction(route, "caching/manual", NULL, manual);
    espAction(route, "caching/update", NULL, update);

    //  This is not required for unit tests
    if ((rp = httpLookupRoute(route->host, "/caching/")) != 0) {
        espCache(rp, "/caching/{action}", 0, 0);
    }
    return 0;
}
