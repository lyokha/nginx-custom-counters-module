Nginx Custom Counters module
============================

This Nginx module features customizable counters shared by all worker processes
and, optionally, by configured sets of virtual servers.

Directives
----------

Normal counters are updated on a later request's phase while filtering headers
of the response. They can be declared on *server*, *location*, and *location-if*
levels. Their cumulative values are merged through all the levels from the top
to the bottom when nginx reads configuration.

#### Normal counters synopsis

```nginx
counter $cnt_name1 set 1;
counter $cnt_name2 inc 1;
```

Variables ``$cnt_name1`` and ``$cnt_name2`` can be accessed elsewhere in the
configuration: they return values held in a shared memory and must be equal
across all workers at the same moment of time. The second argument of the
directive is an operation --- *set* or *inc* (i.e. *increment*). The third
argument --- an integer (possibly negative), is optional, its default value is
*1*.

Early counters are updated before *rewrite* directives and can be used to mark
entry points before any *rewrites* and *ifs*. They are allowed only on the
*location* level. No counters are able to operate in the middle of sequential
rewrites.

#### Early counters synopsis

```nginx
early_counter $ecnt_name1 set 1;
early_counter $ecnt_name2 inc 1;
```

The meaning of the arguments correspond to that of the normal counters. The same
counter can be declared both as normal and early if the location configuration
merged hierarchies do not contain the both simultaneously.

Sharing between virtual servers
-------------------------------

Counters are shared between virtual servers if the latter have equal last
*server names*. Also be aware that the counters always require a server name to
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
        error_log    /tmp/nginx-test-custom-counters-error.log;
        access_log   /tmp/nginx-test-custom-counters-access.log;

        counter $cnt_all_requests inc;

        location / {
            return 200;
        }

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

        location /test_rewrite {
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
            echo "All requests:  $cnt_all_requests";
            echo "Test requests: $cnt_test_requests";
            echo "Test 'a' requests: $cnt_test_a_requests";
            echo "Test 'b' requests: $cnt_test_b_requests";
            echo "Test rewrite requests: $ecnt_test_requests";
        }

        location /reset {
            counter $cnt_all_requests set 0;
            return 200;
        }
    }
}
```

A session example (based on the configuration above)
----------------------------------------------------

```ShellSession
# curl 'http://127.0.0.1:8020/'
All requests:  0
Test requests: 0
Test 'a' requests: 0
Test 'b' requests: 0
Test rewrite requests: 0
# curl 'http://127.0.0.1:8010/test'
All requests before this: 0
# curl 'http://127.0.0.1:8020/'
All requests:  1
Test requests: 1
Test 'a' requests: 0
Test 'b' requests: 0
Test rewrite requests: 0
# curl 'http://127.0.0.1:8010/test?b=1'
# curl 'http://127.0.0.1:8020/'
All requests:  2
Test requests: 2
Test 'a' requests: 0
Test 'b' requests: 1
Test rewrite requests: 0
# curl 'http://127.0.0.1:8010/test?b=1&a=2'
All requests before this: 2
# curl 'http://127.0.0.1:8020/'
All requests:  3
Test requests: 3
Test 'a' requests: 1
Test 'b' requests: 1
Test rewrite requests: 0
# curl 'http://127.0.0.1:8010/test_rewrite?b=1&a=2'
All requests before this: 3
# curl 'http://127.0.0.1:8020/'
All requests:  4
Test requests: 4
Test 'a' requests: 2
Test 'b' requests: 1
Test rewrite requests: 1
# curl 'http://127.0.0.1:8020/reset'
# curl 'http://127.0.0.1:8020/'
All requests:  0
Test requests: 4
Test 'a' requests: 2
Test 'b' requests: 1
Test rewrite requests: 1
```

