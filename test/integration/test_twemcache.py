from base import GenericTest
from base import GenericPmemTest

import os
import sys
import unittest
import ConfigParser
import StringIO

def defineTest(suite, fname, test_type):
    test = test_type()
    test.load('twemcache/' + fname)
    suite.addTest(test)

def twemcache():
    suite = unittest.TestSuite()
    for fname in sorted(os.listdir('twemcache')):
        if not os.path.isdir('twemcache/' + fname):
            defineTest(suite, fname, GenericTest)
            defineTest(suite, fname, GenericPmemTest)

    return suite

def removeDevice():
    with open('twemcache.conf') as f:
        file_content = StringIO.StringIO('[dummy_section]\n' + f.read())
        #[dummy_section] is needed, because original pelikan config file does not have sections.

    config = ConfigParser.ConfigParser()
    config.readfp(file_content)
    devpath = config.get('dummy_section','slab_datapool')

    if os.path.exists(devpath):
        os.remove(devpath)

if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(twemcache())
    removeDevice()
    if result.wasSuccessful():
        sys.exit(0)
    sys.exit(1)
