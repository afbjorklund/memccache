SUITE_couchbase_SETUP() {
    generate_code 1 test1.c

    # need to start couchbase-server first, for this to work
    export CCACHE_COUCHBASE_CONF=couchbase://localhost/default
    export CCACHE_COUCHBASE_USERNAME=ccache
    export CCACHE_COUCHBASE_PASSWORD=ccache
}

SUITE_couchbase() {
    base_tests
    unset CCACHE_COUCHBASE_CONF
}
