Nginx Custom Counters module
============================

This Nginx module features customizable counters shared by all worker processes
and, optionally, by configured sets of virtual servers.

Table of contents
-----------------

- [Directives](#directives)
- [Sharing between virtual servers](#sharing-between-virtual-servers)
- [Collecting all counters in a JSON object](#collecting-all-counters-in-a-json-object)
- [Reloading Nginx configuration](#reloading-nginx-configuration)
- [Persistent counters](#persistent-counters)
- [An example](#an-example)
- [Remarks on using location ifs and complex conditions](#remarks-on-using-location-ifs-and-complex-conditions)
- [See also](#see-also)

Directives
----------

*Normal counters* are updated on the latest request's *log phase*. They can be
declared on *server*, *location*, and *location-if* configuration levels. Their
cumulative values (except for references to run-time variables) are merged
through all the levels from the top to the bottom when Nginx reads
configuration.

#### Normal counters synopsis

```nginx
counter $cnt_name1 set 1;
counter $cnt_name2 inc $inc_cnt_name2;
```

Variables `$cnt_name1` and `$cnt_name2` can be accessed elsewhere in the
configuration: they return values held in a shared memory and thus are equal
across all workers at the same moment. The second argument of the directive is
an operation &mdash; *set* or *inc* (i.e. increment). The third argument &mdash;
an integer (possibly negative) or a variable (possibly negated), is optional,
its default value is *1*.

Starting from version *1.3* of the module, directive `counter` may declare
*no-op* counters such as

```nginx
counter $cnt_name3;
```

This is the exact equivalent of directive `counter` with option `inc 0` which
can be used to declare variable `$cnt_name3` as a counter with the appropriate
variable handler while avoiding access to the shared memory in the run-time.

*Early counters* are updated before *rewrite* directives and can be used to mark
entry points before any *rewrites* and *ifs*. They are allowed only on
*location* configuration level.

#### Early counters synopsis

```nginx
early_counter $cnt_name1 set 1;
early_counter $cnt_name2 inc $inc_cnt_name2;
```

Meaning of the arguments corresponds to that of the normal counters.

Early counters are capable to update on every *rewrite* jump to another
location. With the following configuration,

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
        server_name     main;

        location / {
            early_counter $cnt_1 inc;
            rewrite ^ /2;
        }

        location /2 {
            early_counter $cnt_1 inc;
            early_counter $cnt_2 inc;
            rewrite ^ /3;
        }

        location /3 {
            echo "cnt_1 = $cnt_1 | cnt_2 = $cnt_2";
        }
    }
}
```

all early counters will be printed expectedly.

```ShellSession
$ curl 'http://127.0.0.1:8010/'
cnt_1 = 2 | cnt_2 = 1
$ curl 'http://127.0.0.1:8010/'
cnt_1 = 4 | cnt_2 = 2
```

A single counter can be declared both as normal and early if none of the merged
location configuration hierarchies contains both types simultaneously.

Sharing between virtual servers
-------------------------------

Counters are shared between virtual servers if the latter have equal last
*server names* that form an identifier for the counter set. The counter set
identifier may also be declared explicitly using directive `counter_set_id`
which must precede all server's counters declarations.

When a counter is not mentioned within a virtual server being a member of some
other counter set, it gets *unreachable* in this virtual server. Unreachable
counters are displayed as empty strings, but this is configurable on *main* or
*server* configuration levels via directive `display_unreachable_counter_as`,
e.g.

```nginx
        display_unreachable_counter_as -;
```

Collecting all counters in a JSON object
----------------------------------------

Starting from version *1.5* of the module, a new predefined variable
`$cnt_collection` can be used to collect values of all counters from all counter
sets and display them as a JSON object.

Reloading Nginx configuration
-----------------------------

Counters *may* survive after Nginx configuration reload, provided directive
`counters_survive_reload` was set on *main* or *server* configuration levels.
Counters from a specific counter set *will not* survive if their number in the
set has changed in the new configuration. Also avoid changing the order of
counters declarations, otherwise survived counters will pick values of their
mates that were standing on these positions before reloading.

Persistent counters
-------------------

Counters that survive reload may also be saved and loaded back when Nginx exits
and starts respectively. To enjoy this feature, you need to add on *http*
configuration level lines

```nginx
    counters_survive_reload on;
    counters_persistent_storage /var/lib/nginx/counters.json;
```

The first directive can be moved inside *server* levels of the configuration
where counters persistency is really wanted. Path */var/lib/nginx/counters.json*
denotes location where counters will be saved.

Persistent counters require [*JSMN*](https://github.com/zserge/jsmn) library,
which is header-only. It means that for building persistent counters, you need
to put file *jsmn.h* in the source directory of this module or in a standard
system include path such as */usr/include*. If you want to disable building
persistent counters completely, remove line

```
CFLAGS="$CFLAGS -DNGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY"
```

in file *config*.

An example
----------

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
        server_name     main;

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

        counter $cnt_bytes_sent inc $bytes_sent;
    }

    server {
        listen          8020;
        server_name     monitor.main;
        counter_set_id  main;

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

        location /bytes_sent {
            echo "bytes_sent = $cnt_bytes_sent";
        }

        location /all {
            echo $cnt_collection;
        }
    }

    server {
        listen          8030;
        server_name     other;

        counter $cnt_test1_requests inc;

        display_unreachable_counter_as -;

        location / {
            echo "all = $cnt_all_requests";
        }
    }
}
```

Let's run some curl tests.

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

Now let's see how many bytes were sent by Nginx so far.

```ShellSession
$ curl 'http://127.0.0.1:8020/bytes_sent'
bytes_sent = 949
```

And finally, let's see all counters at once.

```ShellSession
$ curl -s 'http://127.0.0.1:8020/all' | jq
{
  "main": {
    "cnt_all_requests": 5,
    "cnt_a_requests": 9,
    "cnt_test1_requests": 5,
    "cnt_test2_requests": 5,
    "cnt_test3_requests": 5,
    "cnt_test_requests": 4,
    "cnt_test_a_requests": 9,
    "cnt_test_b_requests": 1,
    "ecnt_test_requests": 1,
    "cnt_test0_requests": 1,
    "cnt_bytes_sent": 949
  },
  "other": {
    "cnt_test1_requests": 0
  }
}
```

Remarks on using location ifs and complex conditions
----------------------------------------------------

Originally in Nginx, *location ifs* were designed for a very special task:
*replacing location configuration* when a given condition matches, not for
*doing anything*. That's why using them for only checking a counter like when
testing against `$arg_a` in location */test* is a bad idea in general. In
contrast, *server ifs* do not change location configurations and can be used for
checking increment or set values like `$inc_a_requests`. In our example we can
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

However *if* condition testing in Nginx is not as powerful as it may be
required. If you need a complex condition testing then consider binding
increment or set variables to a full-featured programming language's handler.
For example, let's increment a counter when a *base64*-encoded value contains a
version tag like *v=1*. To make all required computations, let's use Haskell and
[*Nginx Haskell module*](http://github.com/lyokha/nginx-haskell-module).

Put directive `haskell compile` with Haskell function *hasVTag* on *http*
configuration level.

```nginx
    haskell compile standalone /tmp/ngx_haskell.hs '

import Data.ByteString.Base64
import Data.Maybe
import Text.Regex.PCRE

hasVTag = either (const False) (matchTest r) . decode
    where r = makeRegex "\\\\bv=\\\\d+\\\\b" :: Regex

NGX_EXPORT_B_Y (hasVTag)

    ';
```

Then put the next 2 lines into location */test*.

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

into location */* in the second virtual server, the counter gets monitored along
with other custom counters.

To test this, run

```ShellSession
$ curl -b 'Misc=bW9kZT10ZXN0LHY9Mg==' 'http://localhost:8010/test'
```

(value *bW9kZT10ZXN0LHY9Mg==* is base64-encoded string *mode=test,v=2*, try
other variants with or without a version tag too), and watch the value of the
counter *cnt_test_cookie_misc_vtag*.

See also
--------

[*Универсальные счетчики в nginx: замечания к реализации
модуля*](http://lin-techdet.blogspot.com/2016/08/nginx.html) (in Russian).
Remarks on implementation of the module.

