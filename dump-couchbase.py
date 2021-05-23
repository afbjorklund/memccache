#!/usr/bin/env python

import couchbase
import sys
import os

config = os.getenv("COUCHBASE_CONF", 'couchbase://localhost/default')
username = os.getenv("COUCHBASE_USERNAME", "")
password = os.getenv("COUCHBASE_PASSWORD", "")
from couchbase.bucket import Bucket
bucket = Bucket(config, username=username, password=password)

key = sys.argv[1]
val = bucket.get(key, quiet=True, no_format=True)
if val.value:
    print "%s: %d" % (key, len(val.value))

else:
    print "key missing"
