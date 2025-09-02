
# CODSWALLOP RPL (a zen garden)
# #####################################################
# Trivia

# This contains constants and settings which might be of interest globally. 

# Version string, and our base directory.
VERSION = "Codswallop RPL"
BASDIR =  "./"

# Hard limit on the interpreter's call stack size.  Should be healthy, 
# but bounded.
CALLDEPTH = 2048

# Hard limit on object (directory, specifically) copy/circulation recursion.
CPDEPTH = 64

# Where to put the sysRPL functions in the named store.
INTERNALSDIR = 'I*'

# Boot program: read and execute 'boot.rpl' out of the base directory.
LAUNCHCODE = ':: BASDIR "boot.rpl" '+INTERNALSDIR+'.+str '+INTERNALSDIR+'.dsk> ;'

# Maximum number of bytes to read from a file into the parser.
MAXREAD = 256000

# Symbol to evaluate when ded.
DEDEVAL = ['EXCEPT']
