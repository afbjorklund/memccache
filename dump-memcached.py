#!/usr/bin/env python3

from pymemcache.client.base import Client
from pymemcache.client.hash import HashClient
import struct
import sys
import os
import binascii

"""
/* blob format for storing:

    char magic[4]; # 'CCH1', might change for other version of ccache
                   # ccache will erase the blob in memcached if wrong magic
    uint32_t obj_len; # network endian
    char *obj[obj_len];
    uint32_t stderr_len; # network endian
    char *stderr[stderr_len];
    uint32_t dia_len; # network endian
    char *dia[dia_len];
    uint32_t dep_len; # network endian
    char *dep[dep_len];

*/
"""
MEMCCACHE_MAGIC = b'CCH1'

def get_blob(token):
    return token[4:4+struct.unpack('!I', val[0:4])[0]]

"""
/* blob format for big values:

    char magic[4]; # 'CCBM'
    uint32_t numkeys; # network endian
    uint32_t hash_size; # network endian
    uint32_t reserved; # network endian
    uint32_t value_length; # network endian

    <hash[0]>       hash of include file                (<hash_size> bytes)
    <size[0]>       size of include file                (4 bytes unsigned int)
    ...
    <hash[n-1]>
    <size[n-1]>

*/
"""
MEMCCACHE_BIG = b'CCBM'

server = os.getenv("MEMCACHED_SERVERS", "localhost")
if ',' in server:
    servers = []
    for s in server.split(','):
        if ':' in s:
            servers.append(tuple(s.split(':')))
        else:
            servers.append((s, 11211))
    mc = HashClient(servers)
else:
    s = server
    if ':' in s:
        server = tuple(s.split(':'))
    else:
        server = (s, 11211)
    mc = Client(server)

key = sys.argv[1]
val = mc.get(key)
if val[0:4] == MEMCCACHE_BIG:
    numkeys = struct.unpack('!I', val[4:8])[0]
    assert struct.unpack('!I', val[8:12])[0] == 16
    assert struct.unpack('!I', val[12:16])[0] == 0
    size = struct.unpack('!I', val[16:20])[0]
    val = val[20:]
    buf = ""
    while val:
        md4 = val[0:16]
        size = struct.unpack('!I', val[16:20])[0]
        val = val[20:]
        subkey = "%s-%d" % (binascii.hexlify(md4), size)
        subval = mc.get(subkey)
        if not subval:
            print("%s not found" % subkey)
        buf = buf + subval
    val = buf
if val:
    magic = val[0:4]
    if magic == MEMCCACHE_MAGIC:
        val = val[4:]
        obj = get_blob(val)
        val = val[4+len(obj):]
        stderr = get_blob(val)
        val = val[4+len(stderr):]
        dia = get_blob(val)
        val = val[4+len(dia):]
        dep = get_blob(val)
        val = val[4+len(dep):]
        assert len(val) == 0
        print("%s: %d %d %d %d" % (key, len(obj), len(stderr), len(dia), len(dep)))
    else:
        print("wrong magic")
else:
    print("key missing")
