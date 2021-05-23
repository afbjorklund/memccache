SUITE_moxi_SETUP() {
    generate_code 1 test1.c

    # need to start moxi first, for this to work
    export CCACHE_MEMCACHED_CONF=--SERVER=localhost:11311
    # -Z usr=ccache,pwd=ccache,port_listen=11311
    # http://localhost:8091/pools/default/bucketsStreaming/default
}

SUITE_moxi() {
    base_tests
    unset CCACHE_MEMCACHED_CONF
}

