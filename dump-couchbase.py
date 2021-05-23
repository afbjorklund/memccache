#!/usr/bin/env python

import couchbase
import sys
import os

config = os.getenv("COUCHBASE_CONF", 'couchbase://localhost/default')
from couchbase.bucket import Bucket
bucket = Bucket(config)

key = sys.argv[1]
val = bucket.get(key, quiet=True, no_format=True)
if val.value:
    print val.value
else:
    print "key missing"
