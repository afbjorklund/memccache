SUITE_couchbase_SETUP() {
    generate_code 1 test1.c
}

SUITE_couchbase() {
    # need to start couchbase-server first, for this to work
    export CCACHE_COUCHBASE_CONF=couchbase://localhost/default
    base_tests
    unset CCACHE_COUCHBASE_CONF
}
