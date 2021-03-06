SUITE_memcached_socket_SETUP() {
    generate_code 1 test1.c

    export CCACHE_MEMCACHED_CONF=--SOCKET=\"/tmp/memcached.$$\"
}

SUITE_memcached_socket() {
    memcached -s /tmp/memcached.$$ &
    memcached_pid=$!
    base_tests
    kill $memcached_pid
    rm /tmp/memcached.$$
    unset CCACHE_MEMCACHED_CONF
}

