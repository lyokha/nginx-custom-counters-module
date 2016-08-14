Nginx Custom Counters module
============================

This Nginx module features customizable counters shared by all worker processes
and, optionally, by configured sets of virtual servers.

Directives
----------

Normal counters are updated on a later request's phase while filtering response
headers. They can be declared on *server*, *location*, and *location-if* levels.
Their cumulative values (except references to run-time variables) are merged
through all the levels from the top to the bottom when nginx reads
configuration.

#### Normal counters synopsis

```nginx
counter $cnt_name1 set 1;
counter $cnt_name2 inc $inc_cnt_name2;
```

Variables ``$cnt_name1`` and ``$cnt_name2`` can be accessed elsewhere in the
configuration: they return values held in a shared memory and must be equal
across all workers at the same moment of time. The second argument of the
directive is an operation &mdash; *set* or *inc* (i.e. *increment*). The third
argument &mdash; an integer (possibly negative) or a variable (possibly
negated), is optional, its default value is *1*.

Early counters are updated before *rewrite* directives and can be used to mark
entry points before any *rewrites* and *ifs*. They are allowed only on the
*location* level. No counters are able to operate in the middle of sequential
rewrites.

#### Early counters synopsis

```nginx
early_counter $ecnt_name1 set 1;
early_counter $ecnt_name2 inc $inc_cnt_name2;
```

Meaning of the arguments correspond to that of the normal counters. A single
counter can be declared both as normal and early if none of the merged location
configuration hierarchies contains both the types simultaneously.

Sharing between virtual servers
-------------------------------

Counters are shared between virtual servers if the latter have equal last
*server names*. Also be aware that counters always require a server name to
identify themselves: nginx just won't start if a counter has been declared
inside a virtual server without a server name.

Example
-------

```nginx
user                    nobody;
worker_processes        4;

events {
    worker_connections  1024;
}

http {
    default_type        application/octet-stream;
    sendfile            on;

    server {
        listen       8010;
        server_name  main monitored;
        error_log    /tmp/nginx-test-custom-counters-error.log warn;
        access_log   /tmp/nginx-test-custom-counters-access.log;

        counter $cnt_all_requests inc;

        if ($arg_a) {
            set $inc_a_requests 1;
        }

        location / {
            return 200;
        }

        counter $cnt_a_requests inc $inc_a_requests;

        counter $cnt_test1_requests inc;
        counter $cnt_test2_requests inc;
        counter $cnt_test3_requests inc;

        location /test {
            counter $cnt_test_requests inc;
            if ($arg_a) {
                counter $cnt_test_a_requests inc;
                break;
            }
            if ($arg_b) {
                counter $cnt_test_b_requests inc;
                return 200;
            }
            echo "All requests before this: $cnt_all_requests";
        }

        location /test/rewrite {
            early_counter $ecnt_test_requests inc;
            rewrite ^ /test last;
        }
    }

    server {
        listen       8020;
        server_name  server1 monitored;
        allow 127.0.0.1;
        deny all;

        location / {
            echo -n "all = $cnt_all_requests";
            echo -n " | all?a = $cnt_a_requests";
            echo -n " | /test = $cnt_test_requests";
            echo -n " | /test?a = $cnt_test_a_requests";
            echo -n " | /test?b = $cnt_test_b_requests";
            echo " | /test/rewrite = $ecnt_test_requests";
        }

        location ~* ^/reset/a/(\d+)$ {
            set $set_a_requests $1;
            counter $cnt_a_requests set $set_a_requests;
            counter $cnt_test_a_requests set $set_a_requests;
            return 200;
        }
    }
}
```

A session example (based on the configuration above)
----------------------------------------------------

```ShellSession
$ curl 'http://127.0.0.1:8020/'
all = 0 | all?a = 0 | /test = 0 | /test?a = 0 | /test?b = 0 | /test/rewrite = 0
$ curl 'http://127.0.0.1:8010/test'
All requests before this: 0
$ curl 'http://127.0.0.1:8020/'
all = 1 | all?a = 0 | /test = 1 | /test?a = 0 | /test?b = 0 | /test/rewrite = 0
$ curl 'http://127.0.0.1:8010/?a=1'
$ curl 'http://127.0.0.1:8020/'
all = 2 | all?a = 1 | /test = 1 | /test?a = 0 | /test?b = 0 | /test/rewrite = 0
$ curl 'http://127.0.0.1:8010/test?b=1'
$ curl 'http://127.0.0.1:8020/'
all = 3 | all?a = 1 | /test = 2 | /test?a = 0 | /test?b = 1 | /test/rewrite = 0
$ curl 'http://127.0.0.1:8010/test?b=1&a=2'
All requests before this: 3
$ curl 'http://127.0.0.1:8020/'
all = 4 | all?a = 2 | /test = 3 | /test?a = 1 | /test?b = 1 | /test/rewrite = 0
$ curl 'http://127.0.0.1:8010/test/rewrite?b=1&a=2'
All requests before this: 4
$ curl 'http://127.0.0.1:8020/'
all = 5 | all?a = 3 | /test = 4 | /test?a = 2 | /test?b = 1 | /test/rewrite = 1
$ curl 'http://127.0.0.1:8020/reset/a/0'
$ curl 'http://127.0.0.1:8020/'
all = 5 | all?a = 0 | /test = 4 | /test?a = 0 | /test?b = 1 | /test/rewrite = 1
$ curl 'http://127.0.0.1:8020/reset/a/9'
$ curl 'http://127.0.0.1:8020/'
all = 5 | all?a = 9 | /test = 4 | /test?a = 9 | /test?b = 1 | /test/rewrite = 1
```

