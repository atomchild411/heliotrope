/* soupget [-c N] URL...: fetch URLs with libsoup 3 (N at a time, default all at once) and
 * report status, HTTP version, bytes and time for each; for testing the network stack. */
#include <libsoup/soup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pending, next_url, nurls, concurrency;
static char **urls;
static SoupSession *session;
static GMainLoop *loop;

typedef struct { char *url; gint64 start; gsize bytes; SoupMessage *msg; GInputStream *in; char buf[16384]; } Req;

static void start_next(void);

static void finish(Req *r, const char *err)
{
    SoupHTTPVersion v = soup_message_get_http_version(r->msg);
    printf("%3u %s %8lu B %6.2f s %s%s%s\n", soup_message_get_status(r->msg),
        v == SOUP_HTTP_2_0 ? "h2 " : v == SOUP_HTTP_1_1 ? "1.1" : "1.0", (unsigned long)r->bytes,
        (g_get_monotonic_time() - r->start) / 1e6, r->url, err ? " ERROR " : "", err ? err : "");
    fflush(stdout);
    if (r->in)
        g_object_unref(r->in);
    g_object_unref(r->msg);
    g_free(r);
    pending--;
    start_next();
    if (!pending && next_url >= nurls)
        g_main_loop_quit(loop);
}

static void read_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Req *r = data;
    GError *e = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &e);
    if (n < 0) {
        finish(r, e->message);
        g_error_free(e);
        return;
    }
    if (n == 0) {
        finish(r, NULL);
        return;
    }
    r->bytes += (gsize)n;
    g_input_stream_read_async(r->in, r->buf, sizeof r->buf, G_PRIORITY_DEFAULT, NULL, read_done, r);
}

static void sent(GObject *src, GAsyncResult *res, gpointer data)
{
    Req *r = data;
    GError *e = NULL;
    r->in = soup_session_send_finish(SOUP_SESSION(src), res, &e);
    if (!r->in) {
        finish(r, e->message);
        g_error_free(e);
        return;
    }
    g_input_stream_read_async(r->in, r->buf, sizeof r->buf, G_PRIORITY_DEFAULT, NULL, read_done, r);
}

static void start_next(void)
{
    while (next_url < nurls && pending < concurrency) {
        Req *r = g_new0(Req, 1);
        r->url = urls[next_url++];
        r->start = g_get_monotonic_time();
        r->msg = soup_message_new("GET", r->url);
        if (!r->msg) {
            printf("bad URL %s\n", r->url);
            g_free(r);
            continue;
        }
        pending++;
        soup_session_send_async(session, r->msg, G_PRIORITY_DEFAULT, NULL, sent, r);
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    concurrency = 1000;
    if (argc > 2 && !strcmp(argv[1], "-c")) {
        concurrency = atoi(argv[2]);
        i = 3;
    }
    urls = argv + i;
    nurls = argc - i;
    session = soup_session_new_with_options("timeout", 60, "user-agent",
        "Mozilla/5.0 (X11; IRIX64 IP28) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15", NULL);
    loop = g_main_loop_new(NULL, FALSE);
    start_next();
    if (pending)
        g_main_loop_run(loop);
    return 0;
}
