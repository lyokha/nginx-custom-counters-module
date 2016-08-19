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
directive is an operation &mdash; *set* or *inc* (i.e. increment). The third
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
*server names* that form an identifier for the counter set. The counter set
identifier may also be declared explicitly using directive ``counter_set_id``
which must precede all server's counters declarations.

Reloading nginx configuration
-----------------------------

Counters *may* survive after nginx configuration reload, provided directive
``counters_survive_reload`` was set on *main* or *server* configuration levels.
Counters from a specific counter set *will not* survive if their number in the
set has changed in the new configuration. Also avoid changing the order of
counters declarations, otherwise survived counters will pick values of their
mates that were standing on these positions before reloading.

Example
-------

```nginx
user                    nobody;
worker_processes        4;

events {
    worker_connections  1024;
}

error_log               /tmp/nginx-test-custom-counters-error.log warn;

http {
    default_type        application/octet-stream;
    sendfile            on;

    access_log          /tmp/nginx-test-custom-counters-access.log;

    counters_survive_reload on;

    server {
        listen          8010;
        server_name     main monitored;

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
        listen          8020;
        server_name     monitor;
        counter_set_id  monitored;

        allow 127.0.0.1;
        deny  all;

        location / {
            echo -n "all = $cnt_all_requests";
            echo -n " | all?a = $cnt_a_requests";
            echo -n " | /test = $cnt_test_requests";
            echo -n " | /test?a = $cnt_test_a_requests";
            echo -n " | /test?b = $cnt_test_b_requests";
            echo    " | /test/rewrite = $ecnt_test_requests";
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

Remarks on using location ifs and complex conditions
----------------------------------------------------

Originally in nginx *location ifs* were designed for a very special task:
*replacing location configuration* when a given condition matches, not for
*doing anything*. That's why using them for only checking a counter like when
testing against ``$arg_a`` in location */test* is a bad idea in general. In
contrast, *server ifs* do not change location configurations and can be used for
checking increment or set values like ``$inc_a_requests``. In our example we can
simply replace *location if* test

```nginx
            if ($arg_a) {
                counter $cnt_test_a_requests inc;
                break;
            }
```

with

```nginx
            counter $cnt_test_a_requests $inc_a_requests;
```

However nginx *if* condition testing is not as powerful as it may require. If
you need a full-fledged condition testing then consider binding increment or
set variables to a full-featured programming language's handler. For example,
let's increment a counter when a *base64*-encoded value contains a version tag.
To make all required computations, let's use Haskell and [*Nginx Haskell
module*](http://github.com/lyokha/nginx-haskell-module).

Put directive *haskell compile* with a haskell function *hasVTag* on *http
level*.

```nginx
    haskell compile standalone /tmp/ngx_haskell.hs '

import Data.ByteString.Base64
import Data.Maybe
import Text.Regex.PCRE

hasVTag = either (const False) (isJust . matchOnce r) . decode
    where r = makeRegex "\\\\bv=\\\\d+\\\\b" :: Regex

NGX_EXPORT_B_Y (hasVTag)

    ';
```

Then put next 2 lines into location */test*.

```nginx
            haskell_run hasVTag $hs_inc_cnt_vtag $cookie_misc;
            counter $cnt_test_cookie_misc_vtag inc $hs_inc_cnt_vtag;
```

Counter *cnt_test_cookie_misc_vtag* increments when value of *cookie Misc*
matches against a version tag compiled as a regular expression with *makeRegex*.

By adding another line with *echo*

```nginx
            echo -n " | /test?misc:vtag = $cnt_test_cookie_misc_vtag";
```

into location */* in the second virtual server, the counter gets monitored just
like other custom counters.

