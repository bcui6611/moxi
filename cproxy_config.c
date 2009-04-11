/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sysexits.h>
#include <pthread.h>
#include <assert.h>
#include <libmemcached/memcached.h>
#include "memcached.h"
#include "memagent.h"
#include "cproxy.h"
#include "work.h"

void cproxy_on_new_serverlist(void *data0, void *data1) {
    proxy_main *m = data0;
    assert(m);
    memcached_server_list_t *list = data1;
    assert(list);
    assert(list->servers);
    assert(is_listen_thread());

    if (!is_listen_thread())
        return;

    // Create a config string that libmemcached likes,
    // first by counting up buffer size needed.
    //
    int j;
    int n = 0;
    for (j = 0; list->servers[j]; j++) {
        memcached_server_t *server = list->servers[j];
        n = n + strlen(server->host) + 50;
    }

    char *cfg = calloc(n, 1);

    for (int j = 0; list->servers[j]; j++) {
        memcached_server_t *server = list->servers[j];
        char *cur = cfg + strlen(cfg);
        if (j == 0)
            sprintf(cur, "%s:%u", server->host, server->port);
        else
            sprintf(cur, ",%s:%u", server->host, server->port);
    }

    if (settings.verbose > 1)
        fprintf(stderr, "cproxy main has new cfg: %s (bound to %d)\n",
                cfg, list->binding);

    // See if we've already got a proxy running on the port,
    // and create one if needed.
    //
    // TODO: Need to shutdown old proxies.
    //
    proxy *p = m->proxy_head;
    while (p != NULL &&
           p->port != list->binding)
        p = p->next;

    if (p == NULL) {
        if (settings.verbose > 1)
            fprintf(stderr, "cproxy main creating new proxy for %s on %d\n",
                    cfg, list->binding);

        p = cproxy_create(list->name, list->binding, cfg,
                          m->nthreads, m->default_downstream_max);
        if (p != NULL) {
            p->next = m->proxy_head;
            m->proxy_head = p;

            int n = cproxy_listen(p);
            if (n > 0) {
                if (settings.verbose > 1)
                    fprintf(stderr, "cproxy listening on %d conns\n", n);
            } else {
                if (settings.verbose > 1)
                    fprintf(stderr, "cproxy_listen failed on %u\n", p->port);
            }
        }
    } else {
        if (settings.verbose > 1)
            fprintf(stderr, "cproxy main handling config change %u\n",
                    p->port);

        pthread_mutex_lock(&p->proxy_lock);

        if (p->name != NULL && list->name != NULL &&
            strcmp(p->name, list->name) != 0) {
            if (p->name != NULL) {
                free(p->name);
                p->name = NULL;
            }
        }
        if (p->name == NULL &&
            list->name != NULL)
            p->name = strdup(list->name);

        if (strcmp(p->config, cfg) != 0) {
            if (settings.verbose > 1)
                fprintf(stderr,
                        "cproxy main config changed from %s to %s\n",
                        p->config, cfg);

            free(p->config);
            p->config = cfg;
            p->config_ver++;
            cfg = NULL;
        }

        pthread_mutex_unlock(&p->proxy_lock);
    }

    if (cfg != NULL)
        free(cfg);

    free_server_list(list);
}

void on_memagent_new_serverlist(void *userdata,
                                memcached_server_list_t **lists) {
    assert(lists != NULL);

    proxy_main *m = userdata;
    assert(m != NULL);

    LIBEVENT_THREAD *mthread = thread_by_index(0);
    assert(mthread != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "on_memagent_new_serverlist\n");

    bool err = false;

    for (int i = 0; lists[i] && !err; i++) {
        memcached_server_list_t *list = lists[i];
        memcached_server_list_t *list_copy;

        list_copy = copy_server_list(list);
        if (list_copy != NULL) {
            err = !work_send(mthread->work_queue,
                             cproxy_on_new_serverlist,
                             m, list_copy);
        } else
            err = true;

        if (err) {
            if (list_copy != NULL)
                free_server_list(list_copy);
        }
    }
}

