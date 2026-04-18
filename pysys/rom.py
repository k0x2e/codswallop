
# CODSWALLOP RPL (a zen garden)
# #####################################################
# ROM start routine

# This is a deliberately limited means to deserialize a string of certain
# object types, for bootstrapping purposes.  It's meant to break if fed bad
# data or unsupported types.

# Serialized data is of the form:
# ROM name[typenumber:typename,...]CROMobjcount typenumber:objdata ... .
# Numbers are read directly
# Strings and comments begin with a character count
# Lists and code begin with item count and contain only extant references
# Objects are implicitly numbered from 0

# The last object in the string is the one that's evaluated (or the most
# recent builtin if the last object is a dispatch table.)

from .rtypes import typeint, typefloat, typestr, typerem, typebin
from .rtypes import typelst, typecode, typebinproc, typesym, typequote
from .rtypes import typetag, typedir, typecontext
from .trivia import INTERNALSDIR, CALLDEPTH

# Evaluate the object returned by a string.
def evalrom(rt):
  loadrom(rt)
  return rt.Stack.pop().eval

# For boot time only, load from a file and set the context.  Only works if
# the stored object is a context to begin with.
def romboot(rt, file):
  file = open(file)
  ints=rt.rcl(['I*'])
  rt.Stack.push(typestr(file.read()))
  loadrom(rt)
  rt.Context = rt.Stack.pop()
  rt.sto(['i**'],ints)


# Load an object from a string.
def loadrom(rt):
  # Used here and there to fetch blocks of text.
  def advance(delimiter):
    nonlocal text, cursor
    ncursor = text.find(delimiter, cursor)
    word = text[cursor:ncursor]
    cursor = ncursor + 1
    return word
  
  def nextint():
    return int(advance(' '))
    
  # First extract and print the title, if any.
  split = rt.Stack.pop().data.split('[',1)
  if len(split[0]): print(split[0], end='')
  
  # Then fetch all the type numbers for the stored stream.
  split = split[1].split(']',1)
  romtypes = {}
  for pair in split[0].split(','):
    pair = pair.split(':')
    romtypes[pair[0]] = pair[1]

  # The remainder of our text should start with CROM and an object count.
  text = split[1]
  cursor = 4
  count = nextint()
  
  # Now scan through and read in objects.
  try:
    store = [None]*count
    for i in range(count):
      if text[cursor] != '-':
        objtype = romtypes[advance(':')]
      else:
        cursor += 1
        objtype = 'Dispatch'
        binobj = int(advance(':'))
      objraw = advance(' ')
      try:
        objdata = int(objraw)
      except:
        objdata = None
      if objtype == 'Integer':
        store[i] = typeint(objdata)
      elif objtype == 'Float':
        store[i] = typefloat(float(objraw))
      elif objtype == 'String':
        store[i] = typestr(text[cursor:cursor+objdata])
        cursor = cursor+objdata+1
      elif objtype == 'Comment':
        store[i] = typerem(text[cursor:cursor+objdata])
        cursor = cursor+objdata+1   
      elif objtype == 'Directory':
        if objdata == i:
          store[i] = rt.lastobj
        else:
          store[i] = typedir(store[nextint()],store[objdata])
      elif objtype == 'Symbol':
        store[i] = typesym(text[cursor:cursor+objdata].split('.'))
        cursor = cursor+objdata+1
      elif objtype == 'Internal':
        store[i] = rt.rcl([INTERNALSDIR, text[cursor:cursor+objdata]])
        cursor = cursor+objdata+1
      elif objtype == 'Quote':
        store[i] = typequote(store[objdata])
      elif objtype == 'Tag':
        if objdata == -1:
          store[i] = rt.nulltag
        else:
          store[i] = typetag(store[objdata].data[0], store[int(advance(' '))])
        if store[i].obj is None:
          print(store[i].name)
      elif objtype == 'List':
        store[i] = typelst([None]*objdata)
        for j in range(objdata):
          store[i].data[j] = store[nextint()]
      elif objtype == 'Code':
        store[i] = typecode([None]*objdata)
        for j in range(objdata):
          store[i].data[j] = store[nextint()]    
        store[i].data.append(rt.Return)
      elif objtype == 'Context':
        store[i] = typecontext(store[objdata], store[nextint()])
        store[i].depth = CALLDEPTH-store[nextint()].data
        store[i].ip = store[nextint()].data
        store[i].next = store[nextint()]
      elif objtype == 'Builtin':
        store[i] = typebin()
        store[i].data = store[objdata].data[0]
        store[i].hint = store[nextint()].data
        store[i].argct = store[nextint()].data
      elif objtype == 'Dispatch':
        dispatches = []
        argck = []
        for dispatchline in store[objdata].data:
          dispatches.append(dispatchline.data[0])
          argckline = []
          for typenum in dispatchline.data[1:]:
            argckline.append(typenum.data)
          argck.append(argckline)
        store[binobj].argck = argck
        store[binobj].dispatches = dispatches
      else:
        return rt.ded('This was a foolhardy proposal')
  except:
    print('poopd:', objtype, objdata, cursor, i)
    print('context:', text[cursor-5:cursor+5]) 
    ded    

  # Close out our read by printing the final text.
  print(text[cursor:], end=' ')
  # Return the final object (unless that final is a dispatch table.)
  if objtype == 'Dispatch':
    rt.Stack.push(store[binobj])
  else:
    rt.Stack.push(store.pop())
