#!/usr/bin/env python

import couchbase
import os

config = os.getenv("COUCHBASE_CONF", 'couchbase://localhost/default')
from couchbase.bucket import Bucket
bucket = Bucket(config)

ccache = os.getenv("CCACHE_DIR", os.path.expanduser("~/.ccache"))
filelist = []
for dirpath, dirnames, filenames in os.walk(ccache):
    # sort by modification time, most recently used last
    for filename in filenames:
        stat = os.stat(os.path.join(dirpath, filename))
        filelist.append((stat.st_mtime, dirpath, filename))
filelist.sort()
files = blobs = toobig = objects = manifest = 0
for mtime, dirpath, filename in filelist:
    dirname = dirpath.replace(ccache + os.path.sep, "")
    if filename == "CACHEDIR.TAG":
        # ignore these
        files = files + 1
    else:
        (base, ext) = os.path.splitext(filename)
        if (ext == '.o' or
            ext == '.stderr' or
            ext == '.dia' or
            ext == '.d' or
            ext == '.manifest'):
            if ext == '.o':
                objects = objects + 1
            if ext == '.manifest':
                manifest = manifest + 1
            key = "".join(list(os.path.split(dirname)) + [filename])
            val = open(os.path.join(dirpath, filename)).read() or None
            if val:
                print "%s: %d" % (key, len(val))
                try:
                    bucket.upsert(key, val)
                    blobs = blobs + 1
                except couchbase.exceptions.TooBigError:
                    print "# TOO BIG! (%dM)" % (len(val) >> 20)
                    toobig = toobig + 1
            files = files + 1
print "%d files, %d objects (%d manifest) = %d blobs (%d toobig)" % (files, objects, manifest, blobs, toobig)
