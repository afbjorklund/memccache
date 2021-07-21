SUITE_compression_SETUP() {
    generate_code 1 test.c
}

SUITE_compression() {
    # -------------------------------------------------------------------------
    TEST "Hash sum equal for compressed and uncompressed files"

    CCACHE_COMPRESS=1 $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 0
    expect_stat 'cache miss' 1

    CCACHE_COMPRESS=1 $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 1
    expect_stat 'cache miss' 1

    $CCACHE_COMPILE -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 2
    expect_stat 'cache miss' 1

    testname="compression default level"
    CCACHE_COMPRESS=1 CCACHE_COMPRESSLEVEL=-1 $CCACHE $COMPILER -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 3
    expect_stat 'cache miss' 1

    testname="compression default type"
    CCACHE_COMPRESS=1 CCACHE_COMPRESSTYPE=gzip $CCACHE $COMPILER -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 4
    expect_stat 'cache miss' 1

    $CCACHE -C -z >/dev/null
    testname="compression lz4f"
    CCACHE_COMPRESS=1 CCACHE_COMPRESSTYPE=lz4f $CCACHE $COMPILER -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 0
    expect_stat 'cache miss' 1

    CCACHE_COMPRESS=1 CCACHE_COMPRESSTYPE=lz4f $CCACHE $COMPILER -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 1
    expect_stat 'cache miss' 1

    $CCACHE $COMPILER -c test.c
    expect_stat 'cache hit (direct)' 0
    expect_stat 'cache hit (preprocessed)' 2
    expect_stat 'cache miss' 1
}
