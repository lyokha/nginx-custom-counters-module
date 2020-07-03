Nginx Custom Counters module
============================

This Nginx module features customizable counters shared by all worker processes
and, optionally, by configured sets of virtual servers.

Table of contents
-----------------

- [Directives](#directives)
- [Sharing between virtual servers](#sharing-between-virtual-servers)
- [Collecting all counters in a single JSON object](#collecting-all-counters-in-a-single-json-object)
- [Reloading Nginx configuration](#reloading-nginx-configuration)
- [Persistent counters](#persistent-counters)
- [Histograms](#histograms)
- [Predefined counters](#predefined-counters)
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
counter $cnt_name2 undo;
```

Variables `$cnt_name1` and `$cnt_name2` can be accessed elsewhere in the
configuration: they return values held in a shared memory and thus are equal
across all workers at the same moment. The second argument of the directive is
an operation &mdash; *set*, *inc* (i.e. increment), or *undo*. The third
argument is applicable to *set* and *inc* operations only. This is an optional
integer value (possibly negative) or a variable (possibly negated), the default
value is *1*. The *undo* operation discards all changes to the counter made on
the upper levels of the merged hierarchies.

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
early_counter $cnt_name2 undo;
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

Collecting all counters in a single JSON object
-----------------------------------------------

Starting from version *2.0* of the module, a new predefined variable
`$cnt_collection` can be used to collect values of all counters from all counter
sets and display them as a single JSON object. See [*an example*](#an-example).

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
    counters_persistent_storage /var/lib/nginx/counters.json 10s;
```

The first directive can be moved inside *server* levels of the configuration
where counters persistency is really wanted. Path */var/lib/nginx/counters.json*
denotes the directory where the counters will be saved. If the path is relative
(i.e. it does not start with */*), then the counters will be saved in the
*prefix* directory: run `nginx -h` to see where default prefix directory is
located.

Value *10s* defines time interval for saving persistent counters in a backup
storage. This argument is optional: if not set then the counters won't be
written into the backup storage. The name of the backup file corresponds to the
name of the main persistent storage with suffix *~* added. The file gets written
by a worker process when a user request comes to a virtual server associated
with an existing counter set and the specified time interval from the last write
has elapsed. 

Writing to the backup storage can be useful to restore persistent counters on
power outage or *kill -9* of the Nginx master process. In such cases the main
storage will be replaced by the backup storage automatically given that the
latter has more recent modification time and is not corrupted.

Persistent counters require library [*JSMN*](https://github.com/zserge/jsmn),
which is header-only. It means that for building them, you need to put file
*jsmn.h* in the source directory of this module or in a standard system include
path such as */usr/include*. If you want to enable building persistent counters,
set environment variable `$NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY` to *y* or *yes*
before or when running Nginx *configure* script, e.g.

```ShellSession
$ NGX_HTTP_CUSTOM_COUNTERS_PERSISTENCY=yes ./configure ...
```

Histograms
----------

Histograms provide a convenient way of dealing with a set of normal counters
associated with an arbitrary range.

#### Synopsis

```nginx
histogram $hst_name 12 $bound_var;
histogram $hst_name undo;
histogram $hst_name reset;
```

The upper line declares a histogram with *12* bins. The histogram must be bound
to a variable to read the number of the bin to increment from. In this example,
it's expected that variable `$bound_var` will return numbers in range *0 &ndash;
11* according to the number of the histogram bins. If it returns some unexpected
value then variable `$hst_name_err` (which was declared implicitly) will be
incremented instead of the range counters. The counters themselves and their
cumulative count value can be accessed directly via implicitly declared
variables `$hst_name_00 .. $hst_name_11` and `$hst_name_cnt`. Notice that
rarely, when shown in variable `$cnt_collection`, the error and the count values
can be very slightly inconsistent in relation to the range counters: this may
happen because all counters get updated independently, and the updates may occur
in the middle of building of the collection when there are more than one worker
processes.

To simplify detection of the bin to increment in the case of a contiguous value
distribution, directive `map_to_range_index` can be used. For example,

```nginx
    map_to_range_index $request_time $request_time_bin
        0.005
        0.01
        0.05;
```

shall return in variable `$request_time_bin` values from *0* to *3* depending on
the value of variable `$request_time`: if the request time was less than or
equal to *0.005* then its value will be *0*, otherwise, if the request time was
less than or equal to *0.01* then its value will be *1*, and so later, finally,
if the request time was more than *0.05* then its value will be *3*.

Histograms layout can be observed via predefined variable `$cnt_histograms`.

Predefined counters
-------------------

There is a number of predefined counter variables: 7 counters from the Nginx
stub status module (available only when Nginx was compiled with option
*--with-http_stub_status_module*): `$cnt_stub_status_accepted`,
`$cnt_stub_status_handled`, `$cnt_stub_status_requests`,
`$cnt_stub_status_active`, `$cnt_stub_status_reading`,
`$cnt_stub_status_writing`, `$cnt_stub_status_waiting`, and 2 counters related
to Nginx master process uptime: `$cnt_uptime` and `$cnt_uptime_reload`, they
contain the number of seconds elapsed since the start of Nginx. Reloading Nginx
with *SIGHUP* won't restart the former counter. All predefined counters are not
associated with any counter set identifier, nor are they collected in variable
`$cnt_collection`.

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

    map_to_range_index $request_time $request_time_bin
        0.005
        0.01
        0.05
        0.1
        0.5
        1.0
        5.0
        10.0
        30.0
        60.0;

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

        location /histograms {
            echo $cnt_histograms;
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

    server {
        listen          8040;
        server_name     test.histogram;

        histogram $hst_request_time 11 $request_time_bin;

        location / {
            echo_sleep 0.5;
            echo Ok;
        }

        location /1 {
            echo_sleep 1;
            echo Ok;
        }
    }

    server {
        listen          8050;
        server_name     monitor.test.histogram;
        counter_set_id  test.histogram;

        location / {
            echo "all bins: $hst_request_time";
            echo "bin 04:   $hst_request_time_04";
        }

        location /reset {
            histogram $hst_request_time reset;
            echo Ok;
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
  },
  "test.histogram": {
    "hst_request_time_00": 0,
    "hst_request_time_01": 0,
    "hst_request_time_02": 0,
    "hst_request_time_03": 0,
    "hst_request_time_04": 0,
    "hst_request_time_05": 0,
    "hst_request_time_06": 0,
    "hst_request_time_07": 0,
    "hst_request_time_08": 0,
    "hst_request_time_09": 0,
    "hst_request_time_10": 0,
    "hst_request_time_cnt": 0,
    "hst_request_time_err": 0
  }
}
```

It's time to test our histogram.

```ShellSession
$ for in in {1..20} ; do curl -D- 'http://localhost:8040/' & done
  ...
$ for in in {1..50} ; do curl -D- 'http://localhost:8040/1' & done
  ...
```

Locations */* and */1* in the virtual server *test.histogram* delay responses
for *0.5* and *1* seconds respectively. We can check this from the values of the
histogram counters.

```ShellSession
$ curl -s 'http://127.0.0.1:8020/all' | jq {\"test.histogram\"}
{
  "test.histogram": {
    "hst_request_time_00": 0,
    "hst_request_time_01": 0,
    "hst_request_time_02": 0,
    "hst_request_time_03": 0,
    "hst_request_time_04": 13,
    "hst_request_time_05": 45,
    "hst_request_time_06": 12,
    "hst_request_time_07": 0,
    "hst_request_time_08": 0,
    "hst_request_time_09": 0,
    "hst_request_time_10": 0,
    "hst_request_time_cnt": 70,
    "hst_request_time_err": 0
  }
}
```

From this output, we can see that there were *70* requests spread in bins
*04 &ndash; 06* which correspond approximately to a time range from *0.5* to
*1-and-more* seconds.

Let's see how to access all the bins at once and a specific bin.

```ShellSession
$ curl 'http://127.0.0.1:8050/'
all bins: 0,0,0,0,13,45,12,0,0,0,0
bin 04:   13
```

And we also have a way to reset the histogram.

```ShellSession
$ curl -s 'http://127.0.0.1:8050/reset'
Ok
$ curl -s 'http://127.0.0.1:8020/all' | jq {\"test.histogram\"}
{
  "test.histogram": {
    "hst_request_time_00": 0,
    "hst_request_time_01": 0,
    "hst_request_time_02": 0,
    "hst_request_time_03": 0,
    "hst_request_time_04": 0,
    "hst_request_time_05": 0,
    "hst_request_time_06": 0,
    "hst_request_time_07": 0,
    "hst_request_time_08": 0,
    "hst_request_time_09": 0,
    "hst_request_time_10": 0,
    "hst_request_time_cnt": 0,
    "hst_request_time_err": 0
  }
}
```

Though is not present in this example, histogram operation *undo* disables
changing the histogram in the scope where it is declared.

Finally, we can examine how all histograms lay out in counter sets.

```ShellSession
$ curl -s 'http://127.0.0.1:8020/histograms' | jq
{
  "main": {},
  "other": {},
  "test.histogram": {
    "hst_request_time": {
      "range": {
        "hst_request_time_00": "0.005",
        "hst_request_time_01": "0.01",
        "hst_request_time_02": "0.05",
        "hst_request_time_03": "0.1",
        "hst_request_time_04": "0.5",
        "hst_request_time_05": "1.0",
        "hst_request_time_06": "5.0",
        "hst_request_time_07": "10.0",
        "hst_request_time_08": "30.0",
        "hst_request_time_09": "60.0",
        "hst_request_time_10": "+Inf"
      },
      "cnt": [
        "hst_request_time_cnt",
        "cnt"
      ],
      "err": [
        "hst_request_time_err",
        "err"
      ]
    }
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

