# vi:filetype=

use Test::Nginx::Socket;

repeat_each(1);
plan tests => repeat_each() * (2 * blocks());

no_shuffle();
run_tests();

__DATA__

=== TEST 1: check 0
--- http_config
    variables_hash_max_size 2048;

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

    #counters_persistent_storage /var/lib/nginx/counters.json 10s;

    server {
        listen          8010;
        counter_set_id  main;

        counter $cnt_all_requests inc;

        set $inc_a_requests 0;
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
            default_type application/json;
            echo $cnt_collection;
        }

        location /histograms {
            default_type application/json;
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
--- config
        location ~ ^/8010/(.*) {
            proxy_pass http://127.0.0.1:8010/$1$is_args$args;
        }

        location ~ ^/8020/(.*) {
            proxy_pass http://127.0.0.1:8020/$1;
        }
--- request
GET /8020/
--- response_body
all = 0 | all?a = 0 | /test = 0 | /test?a = 0 | /test?b = 0 | /test/rewrite = 0
--- error_code: 200

=== TEST 2: test 1
--- request
GET /8010/test
--- response_body
All requests before this: 0
--- error_code: 200

=== TEST 3: check 1
--- request
GET /8020/
--- response_body
all = 1 | all?a = 0 | /test = 1 | /test?a = 0 | /test?b = 0 | /test/rewrite = 0
--- error_code: 200

=== TEST 4: test a=1
--- request
GET /8010/?a=1
--- response_body
--- error_code: 200

=== TEST 5: check 2
--- request
GET /8020/
--- response_body
all = 2 | all?a = 1 | /test = 1 | /test?a = 0 | /test?b = 0 | /test/rewrite = 0
--- error_code: 200

=== TEST 6: test b=1
--- request
GET /8010/test?b=1
--- response_body
--- error_code: 200

=== TEST 7: check 3
--- request
GET /8020/
--- response_body
all = 3 | all?a = 1 | /test = 2 | /test?a = 0 | /test?b = 1 | /test/rewrite = 0
--- error_code: 200

=== TEST 8: test b=1 a=2
--- request
GET /8010/test?b=1&a=2
--- response_body
All requests before this: 3
--- error_code: 200

=== TEST 9: check 4
--- request
GET /8020/
--- response_body
all = 4 | all?a = 2 | /test = 3 | /test?a = 1 | /test?b = 1 | /test/rewrite = 0
--- error_code: 200

=== TEST 10: test rewrite b=1 a=2
--- request
GET /8010/test/rewrite?b=1&a=2
--- response_body
All requests before this: 4
--- error_code: 200

=== TEST 11: check 5
--- request
GET /8020/
--- response_body
all = 5 | all?a = 3 | /test = 4 | /test?a = 2 | /test?b = 1 | /test/rewrite = 1
--- error_code: 200

=== TEST 12: reset a 0
--- request
GET /8020/reset/a/0
--- response_body
--- error_code: 200

=== TEST 13: check 6
--- request
GET /8020/
--- response_body
all = 5 | all?a = 0 | /test = 4 | /test?a = 0 | /test?b = 1 | /test/rewrite = 1
--- error_code: 200

=== TEST 14: reset a 9
--- request
GET /8020/reset/a/9
--- response_body
--- error_code: 200

=== TEST 15: check 7
--- request
GET /8020/
--- response_body
all = 5 | all?a = 9 | /test = 4 | /test?a = 9 | /test?b = 1 | /test/rewrite = 1
--- error_code: 200

=== TEST 16: check bytes_sent
--- request
GET /8020/bytes_sent
--- response_body
bytes_sent = 737
--- error_code: 200

=== TEST 17: check all
--- request
GET /8020/all
--- response_body
{"main":{"cnt_all_requests":5,"cnt_a_requests":9,"cnt_test1_requests":5,"cnt_test2_requests":5,"cnt_test3_requests":5,"cnt_test_requests":4,"cnt_test_a_requests":9,"cnt_test_b_requests":1,"ecnt_test_requests":1,"cnt_bytes_sent":737},"other":{"cnt_test1_requests":0},"test.histogram":{"hst_request_time_00":0,"hst_request_time_01":0,"hst_request_time_02":0,"hst_request_time_03":0,"hst_request_time_04":0,"hst_request_time_05":0,"hst_request_time_06":0,"hst_request_time_07":0,"hst_request_time_08":0,"hst_request_time_09":0,"hst_request_time_10":0,"hst_request_time_cnt":0,"hst_request_time_err":0}}
--- error_code: 200

=== TEST 18: histograms layout
--- request
GET /8020/histograms
--- response_body
{"main":{},"other":{},"test.histogram":{"hst_request_time":{"range":{"hst_request_time_00":"0.005","hst_request_time_01":"0.01","hst_request_time_02":"0.05","hst_request_time_03":"0.1","hst_request_time_04":"0.5","hst_request_time_05":"1.0","hst_request_time_06":"5.0","hst_request_time_07":"10.0","hst_request_time_08":"30.0","hst_request_time_09":"60.0","hst_request_time_10":"+Inf"},"cnt":["hst_request_time_cnt","cnt"],"err":["hst_request_time_err","err"]}}}
--- error_code: 200

