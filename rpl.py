#!/usr/bin/python3

# CODSWALLOP RPL (a zen garden)
# #####################################################
# Main

from pysys.trivia import *
from pysys.rtypes import baseregistry, typestr
from pysys.runtime import rplruntime
from pysys.internals import stoprocs
from pysys.rom import romboot
    
import signal, sys

# Our simple little Ctrl-C handler.  In some cases we want to raise the
# error regardless, but usually we just want our runtime to catch it
# between evals and trace back.

def catchsigint(signal, frame):
  if ourRT.dieanyway:
    raise KeyboardInterrupt
  else:
    ourRT.Break = True

# Create a new runtime containing just our base types (extra types
# can be added whenever, but the runtime will roll with just these.)
ourtypes = baseregistry()
ourRT = rplruntime(ourtypes)

# Store our internals where the language can get them.
ourRT.sto([INTERNALSDIR], ourRT.firstdir(ourRT.lastobj))
stoprocs(ourRT, INTERNALSDIR)
ourRT.sto([INTERNALSDIR, 'semicolon'], ourRT.Return)
ourRT.sto([INTERNALSDIR, 'lastobj'], ourRT.lastobj)
ourRT.sto([INTERNALSDIR, 'nulltag'], ourRT.nulltag)

# Drop our commandline argument on the stack as a string, if there is one.
# -p is the only system level option, to load different personalities.
argv = sys.argv
if len(argv)>2 and argv[1]=='-p':
  personality = argv[2]
  argv = argv[3:]
else:
  personality = PERSONALITY

if len(argv)>1:
  ourRT.Stack.push(typestr(argv[1]))
else:
  ourRT.Stack.push(typestr(""))

# Load a personality ROM.
romboot(ourRT, f'personality/{personality}.rom')

ourtypes.updatestore(ourRT)

# Turn on Ctrl-C signal handling.
signal.signal(signal.SIGINT, catchsigint)

# And start running.
ourRT.rs(ourRT.Context.eval)
