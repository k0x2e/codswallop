
# CODSWALLOP RPL (a zen garden)
# #####################################################
# Internals

# This file contains something analogous to SysRPL functions.  They are
# just anonymous Python functions that receive the runtime as an argument,
# do what they gotta do, and return.  makebinprocs() builds a list of them,
# and stoprocs stores the list into a directory in a runtime's named store.

# The business of turning these internal functions into builtins, with 
# their user-level safety of argument checking and so forth, is done at boot
# with builtins.rpl.

from trivia import *
from runtime import ret
import rtypes, parse

import time, random, copy

# Windows doesn't include readline for some stupid reason
try:
  import readline
except:
  print("\n** Windows scrubs don't get nice editing keys **\n")


# Build a list of anonymous functions, each entry being [name, function].

# No, it is not ironic or contradictory for functions to be both named
# and anonymous.
def makebinprocs():
  bins = []
   
  ### Documentation  
  
  # Current main store (including locals and whatever).
  def x(rt):
    rt.Stack.push(rt.Context.names)
    return rt.Context.eval
  bins += [['firstobj', x]]
  
  # List of names in a directory.
  def x(rt):
    n = rtypes.typelst()
    nam = rt.Stack.pop().next
    while nam is not rt.lastobj:
      # Skip firstdirs.
      if len(nam.tag.name):
        n.push(rtypes.typesym([nam.tag.name]))
      nam = nam.next
    rt.Stack.push(n)
    return rt.Context.eval
  bins += [['dir', x]]
    
  # Object type.
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().typenum))
    return rt.Context.eval
  bins += [['type', x]]
  
  # Introspect
  def x(rt):
    rt.Stack.push(rt.Context.code)
    return rt.Context.eval
  bins += [['self', x]]

  # Temporary?  Context manipulation.
  # Introspect
  def x(rt):
    rt.Stack.push(rt.Context)
    return rt.Context.eval
  bins += [['getcontext', x]]  
  
  # Reset current context
  def x(rt):
    rt.Context = rt.Stack.pop()
    return rt.Context.eval
  bins += [['setcontext', x]]  
  
  # Return next context.
  def x(rt):
    rt.Stack.push(rt.Stack.pop().next)
    return rt.Context.eval
  bins += [['nextcontext', x]] 

  # Clear Running flag.
  def x(rt):
    rt.Running = False
    return rt.Context.eval
  bins += [['clrrun', x]]  

  # Return error messages and clear internal state.
  def x(rt):
    rt.Stack.push(rtypes.typestr(rt.Caller.data))
    rt.Stack.push(rtypes.typestr(rt.Reason))
    rt.Stack.push(rtypes.typeint(rt.Interrupt))
    rt.Caller=rt.nullcaller
    rt.Reason=''
    rt.Interrupt=False
    return rt.Context.eval
  bins += [['errstate', x]]

  # Invoke last caller.
  def x(rt):
    return rt.Caller.eval
  bins += [['lastcall', x]]
    
  ### Input/Output
  
  # Open file.
  def x(rt):
    options = rt.Stack.pop()
    filename = rt.Stack.pop()
    try:
      rt.Stack.push(rtypes.typeio(open(filename.data, options.data)))
    except:
      rt.Stack.push(filename)
      rt.Stack.push(options)
      return rt.ded('Perhaps opening this file was a daydream after all')
    return rt.Context.eval
  bins += [['fopen', x]]

  def x(rt):
    if rt.Stack.pop().eof:
      rt.Stack.push(rtypes.typeint(1))
    else:
      rt.Stack.push(rtypes.typeint(0))
    return rt.Context.eval
  bins += [['feof', x]]
  
  # Close file.
  def x(rt):
    handle = rt.Stack.pop()
    try:
      handle.data.close()
    except:
      rt.Stack.push(handle)
      return rt.ded('One can only close a file so hard')
    return rt.Context.eval
  bins += [['fclose', x]]
  
  # Read line from file, but strip newline.
  def x(rt):
    handle = rt.Stack.pop()
    try:
      string = handle.data.readline(MAXREAD)
      if not len(string):
        handle.eof = True
      rt.Stack.push(rtypes.typestr(string.rstrip('\n')))
    except:
      rt.Stack.push(handle)
      return rt.ded('You may read a book, but not this file')
    return rt.Context.eval
  bins += [['freadline', x]]
  
  # Read some number of characters from a file.
  def x(rt):
    chars = rt.Stack.pop()
    handle = rt.Stack.pop()
    try:
      if chars.data > 0 and chars.data < MAXREAD:
        string = handle.data.read(chars.data)
        if len(string)<chars.data:
          handle.eof = True
      else:
        rt.Stack.push(rtypes.typestr(handle.data.read(MAXREAD)))
        if len(string)<MAXREAD:
          handle.eof = True
    except:
      rt.Stack.push(handle)
      rt.Stack.push(chars)
      return rt.ded('You may read a book, but not this file')
    return rt.Context.eval
  bins += [['fread', x]]

  # Write a line to a file, no newline.
  def x(rt):
    handle = rt.Stack.pop()
    text = rt.Stack.pop()
    try:
      handle.data.write(text.data)
    except:
      rt.Stack.push(text)
      rt.Stack.push(handle)
      return rt.ded('You may write a friend, but not this file')
    return rt.Context.eval
  bins += [['fwriten', x]]
  
  # Write a line to a file with newline.
  def x(rt):
    handle = rt.Stack.pop()
    text = rt.Stack.pop()
    try:
      handle.data.write(text.data)
      handle.data.write('\n')
    except:
      rt.Stack.push(text)
      rt.Stack.push(handle)
      return rt.ded('You may write a friend, but not this file')
    return rt.Context.eval
  bins += [['fwrite', x]]
  
  # Display.
  def x(rt):
    print(rt.Stack.pop().data)
    return rt.Context.eval
  bins += [['disp', x]]
  
  # Display;.
  def x(rt):
    print(rt.Stack.pop().data, end="")
    return rt.Context.eval
  bins += [['dispn', x]]

  # Console input.
  def x(rt):
    rt.dieanyway = True
    x = rt.Stack.pop()
    try:
      rt.Stack.push(rtypes.typestr(input(x.data)))
    except:
      rt.Stack.push(x)
      rt.Break = False
      return rt.ded('The user has typed unforgivably')
    rt.dieanyway = False
    return rt.Context.eval
  bins += [['prompt', x]]
  
  # Time.
  def x(rt):
    rt.Stack.push(rtypes.typefloat(time.time()))
    return rt.Context.eval
  bins += [['epoch', x]]
 
 
  ### Stack manipulation
  
  # Return whole stack.
  def x(rt):
    rt.Stack.push(rt.Stack.cp())
    return rt.Context.eval
  bins += [['stack', x]]
  
  # Drop line 1.
  def x(rt):
    rt.Stack.pop()
    return rt.Context.eval
  bins += [['drop', x]]
  
  # Drop n lines.
  def x(rt):
    lines = rt.Stack.pop()
    if lines.data<0 or lines.data>len(rt.Stack.data):
      rt.Stack.push(lines)
      return rt.ded('That is not a reasonable number of lines to drop')
    else:
      rt.Stack.data = rt.Stack.data[:len(rt.Stack.data)-lines.data]
    return rt.Context.eval
  bins += [['dropn', x]]
  
  # Pick a line.
  def x(rt):
    line = rt.Stack.pop()
    if line.data<=0 or line.data>len(rt.Stack.data):
      rt.Stack.push(line)
      return rt.ded('A pick beyond one\'s reach')
    rt.Stack.push(rt.Stack.data[len(rt.Stack.data)-line.data])
    return rt.Context.eval
  bins += [['pick', x]]
  
  # Evaluate.
  def x(rt):
    return rt.Stack.pop().eval
  bins += [['eval', x]]
  
  # Swap.
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    rt.Stack.push(x)
    rt.Stack.push(y)
    return rt.Context.eval
  bins += [['swap', x]]
  
  # Duplicate.
  def x(rt):
    rt.Stack.data += rt.Stack.data[len(rt.Stack.data)-1:]
    return rt.Context.eval
  bins += [['dup', x]]
  
  def x(rt):
    rt.Stack.data += rt.Stack.data[len(rt.Stack.data)-2:]
    return rt.Context.eval
  bins += [['dup2', x]]
  
  def x(rt):
    count = rt.Stack.pop()
    if len(rt.Stack.data)>=count.data>0:
      rt.Stack.data += rt.Stack.data[len(rt.Stack.data)-count.data:]
    else:
      rt.Stack.push(count)
      return rt.ded('Duplicate how many things now')
    return rt.Context.eval
  bins += [['dupn', x]]
  
  # Rotate.
  def x(rt):
    splice = len(rt.Stack.data)-3
    rt.Stack.data[splice:] = rt.Stack.data[splice+1:] + [rt.Stack.data[splice]]
    return rt.Context.eval
  bins += [['rot', x]]

  def x(rt):  
    splice = len(rt.Stack.data)-3
    last = len(rt.Stack.data)-1
    rt.Stack.data[splice:] = [rt.Stack.data[last]] + rt.Stack.data[splice:last] 
    return rt.Context.eval
  bins += [['rotd', x]]  
  
  # Roll.
  def x(rt):
    qty = rt.Stack.pop()
    if len(rt.Stack) < qty.data:
      rt.Stack.push(qty)
      return rt.ded('Your katamari is not big enough to roll this much')
    elif qty.data>0:
      splice = len(rt.Stack.data)-qty.data
      rt.Stack.data[splice:] = rt.Stack.data[splice+1:] + [rt.Stack.data[splice]]
    return rt.Context.eval
  bins += [['roll', x]]
                   
  def x(rt):
    qty = rt.Stack.pop()
    if len(rt.Stack) < qty.data:
      rt.Stack.push(qty)
      return rt.ded('Your katamari is not big enough to roll this much')
    elif qty.data>0:
      splice = len(rt.Stack.data)-qty.data
      last = len(rt.Stack.data)-1
      rt.Stack.data[splice:] = [rt.Stack.data[last]] + rt.Stack.data[splice:last] 
    return rt.Context.eval
  bins += [['rolld', x]]

  # Require.
  def x(rt):
    qty = rt.Stack.pop()
    if len(rt.Stack) < qty.data:
      rt.Stack.push(qty)
      return rt.ded('Successful persons have '+str(qty.data)+' or more objects on the stack')
    return rt.Context.eval
  bins += [['require', x]]
  
  ### Disk store

  # Parse an entire file as a code object.
  def x(rt):
    name = rt.Stack.pop()
    try:
      with open(name.data, 'r') as file:
        text = ':: '+file.read(MAXREAD)+' ;'
    except:
      rt.Stack.push(name)
      return rt.ded('The operating system says no')
    obj = parse.parse(rt, text)
    if obj is None:
      return rt.ded('The parser did not care for your shenanigans')
    return obj.eval
  bins += [['dsk>', x]]
  

  ### Named storage
  # Recall symbol.
  def x(rt):
    sym = rt.Stack.pop()
    obj = rt.rcl(sym.data)
    if obj == None:
      rt.Stack.push(sym)
      return rt.ded("What even is "+rtypes.symtostr(sym.data))
    else:
      rt.Stack.push(obj)
    return rt.Context.eval
  bins += [['rcl', x]]
  
  # Pull object out of tag.
  def x(rt):
    tag = rt.Stack.pop()
    rt.Stack.push(tag.obj)
    return rt.Context.eval
  bins += [['rclfrom', x]]
  
  # Store symbol.
  def x(rt):
    def usded(og, one, two, reason):
      rt.Stack.push(one)
      rt.Stack.push(two)
      # To unwind a failed store, we need to either write the original object
      # back to an extant name, or erase the name we added.
      if og != None:
        rt.sto(two.data, og)
      else:
        rt.rm(two.data)
      return rt.ded(reason)

    name = rt.Stack.pop()
    obj = rt.Stack.pop()
    og = rt.rcl(name.data)

    # First try to store the object.  If that doesn't work, there was a
    # directory traverse failure.
    if not rt.sto(name.data, obj):
      return usded(None,obj,name,'To store to a directory, first the directory must exist')
    else:
      # Now, if the thing we just stored is a directory, make sure it didn't
      # contain anything circulatory:
      if obj.typenum == rt.dirtype and rt.circdir(obj):
        return usded(og,obj,name,'That directory contains circular references')
      elif obj.typenum == rt.symtype and rt.circsym(obj.data):
        return usded(og,obj,name,'cDonalds Theorem does not apply to symbolic references')
    return rt.Context.eval
  bins += [['sto', x]]

  # Put object into tag.
  def x(rt):
    tag = rt.Stack.pop()
    thing = rt.Stack.pop()
    if thing.typenum == rt.symtype:
      original = tag.obj
      tag.obj = thing
      if rt.circsym(thing.data):
        tag.obj = original
        rt.Stack.push(thing)
        rt.Stack.push(tag)
        return rt.ded('A valiant effort to reference oneself, thwarted')
    elif thing is tag:
      rt.Stack.push(thing)
      rt.Stack.push(tag)
      return rt.ded('A valiant effort to reference oneself, thwarted')
    else:
      tag.obj = thing
    return rt.Context.eval
  bins += [['stoto', x]]
  
  # Dereference.
  def x(rt):
    sym = rt.Stack.pop()
    obj = rt.deref(sym.data)
    if obj == None:
      rt.Stack.push(sym)
      return rt.ded('It is difficult to dereference what does not exist')
    else:
      rt.Stack.push(obj)        
    return rt.Context.eval
  bins += [['deref', x]]

  # Does it exist?
  def x(rt):
    sym = rt.Stack.pop()
    if rt.rcl(sym.data) is None:
      rt.Stack.push(rtypes.typeint(0))
    else:
      rt.Stack.push(rtypes.typeint(1))
    return rt.Context.eval
  bins += [['exists', x]]
  
  # Erase name.
  def x(rt):
    x=rt.Stack.pop()
    if not rt.rm(x.data):
      rt.Stack.push(x)
      return rt.ded("You have failed to erase what isn't here!")
    return rt.Context.eval
  bins += [['rm', x]]

  # Copy object explicitly.
  def x(rt):
    rt.Stack.push(rt.Stack.pop().cp())
    return rt.Context.eval
  bins += [['cp', x]]

  # Find object memory ID.
  def x(rt):
    rt.Stack.push(rtypes.typeint(id(rt.Stack.pop())))
    return rt.Context.eval
  bins += [['id', x]]

  # Tag local variable context, for use in user objects.
  def x(rt):
    prog = rt.Stack.pop()
    tag = rt.Stack.pop()
    return rt.newlocall(prog, rtypes.typedir(tag, rt.Context.names))
  bins += [['tlocal', x]]

  # Register new type.
  def x(rt):
    name = rt.Stack.pop()
    usreval = rt.Stack.pop()
    proto = rt.Stack.pop()
    proto.typename = name.data[0]
    # If an evaluator is a comment, skip it for speed.
    if usreval.typenum != rt.Types.id['Comment']:
      proto.usreval = usreval.eval
    rt.Types.registerusr(proto)
    rt.Types.updatestore(rt)
    rt.Stack.push(rtypes.typeint(proto.typenum))
    return rt.Context.eval
  bins += [['regtype', x]]
  
  # Local variable context.
  def x(rt):
    # In case of emergency, pull this lever and return.
    def usded(reason):
      rt.Context = origcontext
      rt.Stack.data = origstack
      return rt.ded(reason)
      
    # Hang onto our whole stack and our current context.
    origstack = rt.Stack.data[:]
    origcontext = rt.Context
    
    # Make a new temporary context for purposes of checking for circulation.
    # In the future, it might make more sense, and be a lot faster, to just
    # not check for circulation at all, now that such a reference will softly
    # hang the inner loop instead of crashing all the way out of Python.
    names = rt.Stack.pop().data
    prog = rt.Stack.pop()
    rt.Context = rtypes.typecontext(prog, origcontext.names)
    nextob = origcontext.names
    
    dirtype = rt.dirtype
    tagtype = rt.Types.id['Tag']
    comtype = rt.Types.id['Comment']
    symtype = rt.symtype
    
    for i in names:
      if i.typenum == symtype:
        # If it's a symbol, verify it's a valid one.
        if len(i.data)>1:
          return usded("Ain't no dots in local variable names")
        # Try popping an object off the stack and assigning it to a name.
        thisob = rt.Stack.pop()
        if thisob is not None:
          nextob = rtypes.typedir(rtypes.typetag(i.data[0], thisob), nextob)
          circname = i.data
        else:
          return usded('You gotta have '+str(len(names))+' things on the stack!')
      elif i.typenum == tagtype:
        # Tags are copied and assigned without pulling anything off the stack.
        circname = [i.name]
        nextob = rtypes.typedir(i.cp(), nextob)
      elif i.typenum == comtype:
        # Comments are suppressed (this repeats last circulation check.)
        pass
      else:
        usded("Only symbols and tags lead to success")
      # Check for circular references as we go.
      rt.Context.names = nextob
      if nextob.tag.obj.typenum == symtype:
        if rt.circsym(circname):
          return usded('Round and round the '+rtypes.symtostr(circname)+' bush the '+
                rtypes.symtostr(circname)+' chased the '+rtypes.symtostr(circname))
      elif nextob.tag.obj.typenum==dirtype:
        if rt.circdir(nextob.tag.obj):
          return usded('This directory circulates if you put it there')
    # And queue a new local variable context.
    
    # Two ways to call: one if we stored no names, the other if we did.
    # Restore our original context first, discarding the test context.
    nextob = rt.Context.names
    rt.Context = origcontext
    if nextob is origcontext.names:
      return rt.newcall(prog)
    else:
      return rt.newlocall(prog, nextob)
      
  bins += [['local', x]]
  
  
  ### Flow control
  # Step once.
  def x(rt):
    # Grab a thing to evaluate.
    next = rt.Stack.pop()
    # Click forward one object in our running code.
    this = rt.Context.eval(rt)

    # This pains me, but apparently is/is not will not work here
    # because the IDs are different, even though the string representations
    # of these function calls match.  I think this is the dumbest thing
    # I have ever had to do in Python.
    while str(this) != str(rt.Context.eval):
      this = this(rt)
      
    return next.eval
    
  bins += [['evalnext', x]]

  # Bail (pop one off the call stack)
  def x(rt):
    # If we're at the end of this context, bail from the next one too.
    if rt.Context.ip+1 == len(rt.Context.code.data):
      ret(rt)
    return ret(rt)
  bins += [['bail', x]]

  # BEVAL: bail and evaluate (goto)
  def x(rt):
    # If we're at the end of this context, bail from the next one too.
    if rt.Context.ip+1 == len(rt.Context.code.data):
      ret(rt)
    ret(rt)
    # Suppress any premature notions that we're done here.
    rt.Running = True
    return rt.Stack.pop().eval
  bins += [['beval', x]]

  # If-then
  def x(rt):
    th = rt.Stack.pop()
    if rt.Stack.pop().data: 
      return th.eval
    else:
      return rt.Context.eval

  bins += [['ift', x]]

  # If-then-else
  def x(rt):
    el = rt.Stack.pop()
    th = rt.Stack.pop()
    if rt.Stack.pop().data: return th.eval
    else: return el.eval
  bins += [['ifte', x]]


  ### Mathemagics
  # Parity
  def x(rt):
    rt.Stack.push(rtypes.typeint(bool(rt.Stack.pop().data % 2)))
    return rt.Context.eval
  bins += [['odd', x]]
  
  # Absolute value
  def x(rt):
    rt.Stack.push(rtypes.typeint(abs(rt.Stack.pop().data)))
    return rt.Context.eval
  bins += [['absint', x]]

  def x(rt):
    rt.Stack.push(rtypes.typefloat(abs(rt.Stack.pop().data)))
    return rt.Context.eval
  bins += [['absfloat', x]]
  
  # Modulo
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    if x.data:
      rt.Stack.push(rtypes.typefloat(y.data % x.data))
    else:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('Excuse you')
    return rt.Context.eval
  bins += [['modfloat', x]]

  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    if x.data:
      rt.Stack.push(rtypes.typeint(y.data % x.data))
    else:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('Excuse you')
    return rt.Context.eval
  bins += [['modint', x]]

  
  # Add
  def x(rt):
    rt.Stack.push(rtypes.typefloat(rt.Stack.pop().data+rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['+float', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data+rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['+int', x]]
  
  def x(rt):
    x = rt.Stack.pop().data
    y = rt.Stack.pop().data
    rt.Stack.push(rtypes.typestr(y+x))
    return rt.Context.eval
  bins += [['+str', x]]
  
  def x(rt):
    src = rt.Stack.pop()
    dest = rt.Stack.pop().cp()
    dest.push(src)
    rt.Stack.push(dest)
    return rt.Context.eval
  bins += [['+list', x]]

  def x(rt):
    x = rt.Stack.pop().cp()
    x.data = [rt.Stack.pop().cp()]+x.data
    rt.Stack.push(x)
    return rt.Context.eval
  bins += [['list+', x]]

  def x(rt):
    x = rt.Stack.pop().data
    y = rt.Stack.pop().data
    rt.Stack.push(rtypes.typelst(y+x))
    return rt.Context.eval
  bins += [['catlist', x]]

  def x(rt):
    x = rt.Stack.pop().data
    y = rt.Stack.pop().data
    # Suppress the return call from the left hand side of the code.
    rt.Stack.push(rtypes.typecode(y[:len(y)-1]+x))
    return rt.Context.eval
  bins += [['catcode', x]]

  def x(rt):
    x = rt.Stack.pop().data
    y = rt.Stack.pop().data
    rt.Stack.push(rtypes.typesym(y+x))
    return rt.Context.eval
  bins += [['+sym', x]]
 
  # POWER^^^^^^
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    try:
      rt.Stack.push(rtypes.typefloat(y.data**x.data))
    except:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('This produces an intolerably high number')
    return rt.Context.eval
  bins += [['^float', x]]
 
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    try:
      rt.Stack.push(rtypes.typeint(y.data**x.data))
    except:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('This produces an intolerably high number')
    return rt.Context.eval
    
  bins += [['^int', x]]

  # Multiply
  def x(rt):
    rt.Stack.push(rtypes.typefloat(rt.Stack.pop().data*rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['*float', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data*rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['*int', x]]
  
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    if x.data >= 0:
      rt.Stack.push(rtypes.typestr(y.data*x.data))
    else:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('wat')
    return rt.Context.eval
  bins += [['*str', x]]
  
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    z = rtypes.typelst()
    if x.data >= 0:
      for i in range(x.data):
        for j in y.data: 
          z.push(j)
      rt.Stack.push(z)
    else:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('wat')
    return rt.Context.eval
  bins += [['*lst', x]]

  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    z = rtypes.typecode()
    if x.data >= 0:
      y = y.data[:len(y.data)-1]
      for i in range(x.data):
        for j in y: 
          z.push(j)
      z.push(rt.Return)
      rt.Stack.push(z)
    else:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('wat')
    return rt.Context.eval
  bins += [['*code', x]]


  # Subtract
  def x(rt):
    rt.Stack.push(rtypes.typefloat(-rt.Stack.pop().data+rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['-float', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typeint(-rt.Stack.pop().data+rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['-int', x]]
 
  # Divide
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    if x.data:
      rt.Stack.push(rtypes.typefloat(y.data/x.data))
    else:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('Excuse you')
    return rt.Context.eval
  bins += [['/float', x]]
  
  def x(rt):
    x = rt.Stack.pop()
    y = rt.Stack.pop()
    if x.data:
      rt.Stack.push(rtypes.typeint(y.data/x.data))
    else:
      rt.Stack.push(y)
      rt.Stack.push(x)
      return rt.ded('Excuse you')
    return rt.Context.eval
  bins += [['/int', x]]

  # Negate
  def x(rt):
    num = copy.copy(rt.Stack.pop())
    num.data = -num.data
    rt.Stack.push(num)
    return rt.Context.eval
  bins += [['neg', x]]
 
  # Random
  def x(rt):
    rt.Stack.push(rtypes.typefloat(random.random()))
    return rt.Context.eval
  bins += [['rnd', x]]
  
  # Integer portion
  def x(rt):
    rt.Stack.push(rtypes.typefloat(int(rt.Stack.pop().data)))
    return rt.Context.eval
  bins += [['ip', x]]


  ### Conversions
  # to quote
  def x(rt):
    rt.Stack.push(rtypes.typequote(rt.Stack.pop()))
    return rt.Context.eval
  bins += [['>quote', x]]
  
  # to tag
  def x(rt):
    name = rt.Stack.pop()
    obj = rt.Stack.pop()
    if len(name.data)>1:
      rt.Stack.push(obj)
      rt.Stack.push(name)
      return rt.ded("Tags don't have last names")
    else:
      rt.Stack.push(rtypes.typetag(name.data[0],obj))
    return rt.Context.eval
  bins += [['>tag', x]]
  
  # to builtin (add dispatch table later with binhook)
  def x(rt):
    newbin = rtypes.typebin()
    oldstack = rt.Stack.data[:]
    # Don't let user get fresh with dotted names
    newbin.data = rt.Stack.pop().data[0]
    newbin.hint = rt.Stack.pop().data
    newbin.argct = rt.Stack.pop().data
    newbin.dispatches = []
    newbin.argck = [] 
    if newbin.argct < 0:
      rt.Stack.data = oldstack
      return rt.ded("It's hard to win a negative argument")
    rt.Stack.push(newbin)
    return rt.Context.eval
  bins += [['>bin', x]]
  
  # Break apart a builtin.  Removal is the opposite of installation.
  def x(rt):
    ourbin = rt.Stack.pop()
    table = rtypes.typelst([])
    for i in range(len(ourbin.argck)):
      line = [ourbin.dispatches[i]]
      for j in range(ourbin.argct):
        line += [rtypes.typeint(ourbin.argck[i][j])]
      table.push(rtypes.typelst(line))
    rt.Stack.push(table)
    rt.Stack.push(rtypes.typeint(ourbin.argct))
    rt.Stack.push(rtypes.typestr(ourbin.hint))
    rt.Stack.push(rtypes.typesym([ourbin.data]))
    return rt.Context.eval
  bins += [['bin>', x]]

  # Record a complete dispatch table to a builtin.
  def x(rt):
    table = rt.Stack.pop().data
    ourbin = rt.Stack.pop()
    ourbin.dispatches = []
    ourbin.argck = []
    for i in table:
      ourbin.dispatches += [i.data[0]]
      argck = []
      for j in range(ourbin.argct):
        argck += [i.data[j+1].data]
      ourbin.argck += [argck]        
    return rt.Context.eval
  bins += [['setdispatch', x]]
        
  # Hook new dispatch lines into an extant builtin.
  def x(rt):
    oldstack = rt.Stack.data[:]
    bin = rt.Stack.pop()
    patches = rt.Stack.pop()
    
    # Try to add a new dispatch line for each one the user wants.
    newdispatches = []
    newargck = []
    for i in patches.data:
      if i.typenum == rt.Types.id['List']:
        if len(i)>bin.argct:
          # If we're handed a symbol, try to recall it first.  This is the
          # object we'll dispatch to.
          ourdispatch = i.data[0]
          if ourdispatch.typenum == rt.symtype:
            tryin = rt.rcl(ourdispatch.data)
            if tryin is not None:
              ourdispatch = tryin
          newdispatches.append(ourdispatch)
          
          # Then try to turn our arguments into an argck line.
          argline = []
          for j in range(bin.argct):
            ourarg = i.data[j+1]

            # Here also, if we find a symbol, try to recall it.  This will
            # reduce an otherwise mandatory two-step when making builtins of
            # custom types.
            if ourarg.typenum == rt.symtype:
              tryin = rt.rcl(ourarg.data)
              if tryin is not None:
                ourarg = tryin
                
            if ourarg.typenum != rt.Types.id['Integer'] or\
               not ourarg.typenum in rt.Types.id.values():
              rt.Stack.data = oldstack
              return rt.ded("Type numbers have to be a number which represents a type")
            else:          
              argline.append(ourarg.data)
          newargck.append(argline)
        else:
          rt.Stack.data = oldstack
          return rt.ded("Next time try including the number of arguments you asked for")
      else:
        rt.Stack.data = oldstack
        return rt.ded("If you want a built-in, you should consider a less broken dispatch table")
    # Assuming we got this far, we made it, so put our new dispatches to the
    # front of the line, and return our object:
    bin.argck = newargck + bin.argck
    bin.dispatches = newdispatches + bin.dispatches
    rt.Stack.push(bin)
    return rt.Context.eval
  bins += [['binhook', x]]


  # Make empty directory
  def x(rt):
    rt.Stack.push(rt.firstdir(rt.lastobj))
    return rt.Context.eval
  bins += [['mkdir', x]]

  # Number to integer
  def x(rt):
    rt.Stack.push(rtypes.typeint(int(rt.Stack.pop().data)))
    return rt.Context.eval
  bins += [['num>int', x]]
  
  def x(rt):
    x = rt.Stack.pop()
    try:
      rt.Stack.push(rtypes.typeint(int(x.data)))
    except:
      rt.Stack.push(x)
      return rt.ded('That will never be an integer, my friend')
    return rt.Context.eval
  bins += [['str>int', x]]
  
  # Number to float
  def x(rt):
    rt.Stack.push(rtypes.typefloat(float(rt.Stack.pop().data)))
    return rt.Context.eval
  bins += [['num>float', x]]
  
  def x(rt):
    x = rt.Stack.pop()
    try:
      rt.Stack.push(rtypes.typefloat(float(x.data)))
    except:
      rt.Stack.push(x)
      return rt.ded("Sir/ma'am, this is a Wendy's")
    return rt.Context.eval
  bins += [['str>float', x]]

  # Basic VAL
  def x(rt):
    x = rt.Stack.pop()
    try:
      rt.Stack.push(rtypes.typefloat(float(x.data)))
    except:
      rt.Stack.push(rtypes.typefloat(0))
    return rt.Context.eval
  bins += [['basicval', x]]

  # Character to integer.  
  def x(rt):
    string = rt.Stack.pop()
    if len(string.data):
      rt.Stack.push(rtypes.typeint(ord(string.data[0])))
    else:
      rt.Stack.push(string)
      return rt.ded('It would be 0 if it was anything at all')
    return rt.Context.eval
  bins += [['asc>', x]]

  # Integer to character.
  def x(rt):
    string = rt.Stack.pop()
    if string.data >= 0 and string.data < 1114112:
      rt.Stack.push(rtypes.typestr(chr(string.data)))
    else:
      rt.Stack.push(string)
      return rt.ded('This number could not possibly be a character')
    return rt.Context.eval
  bins += [['>asc', x]]
  
  # String to objects.
  def x(rt):
    text = rt.Stack.pop()
    x = parse.parse(rt, text.data)
    if x is None:
      rt.Stack.push(text)
      return rt.ded('This is no RPL that I can see')
    else:
      rt.Stack.push(x)
      return rt.Context.eval
  bins += [['parse', x]]
  
  # To function.
  def x(rt):
    ourstring = rt.Stack.pop()
    if parse.validatename(ourstring.data):
      rt.Stack.push(rtypes.typesym(ourstring.data.split('.')))
    else:
      rt.Stack.push(ourstring)
      return rt.ded("This can be a string, but it won't be a symbol")
    return rt.Context.eval
  bins += [['str>sym', x]]

  # String or comment to string.
  def x(rt):
    rt.Stack.push(rtypes.typestr(rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['str>str', x]]
  
  # Number to string.
  def x(rt):
    rt.Stack.push(rtypes.typestr(rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['num>str', x]]
  
  # Symbol to string.
  def x(rt):
    rt.Stack.push(rtypes.typestr(rtypes.symtostr(rt.Stack.pop().data)))
    return rt.Context.eval
  bins += [['sym>str', x]]
  
  ### Comparisons

  # Equality
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data==rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['==', x]]

  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data!=rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['!=', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop() is rt.Stack.pop()))
    return rt.Context.eval
  bins += [['==ref', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop() is not rt.Stack.pop()))
    return rt.Context.eval
  bins += [['!=ref', x]]
  
  # Less than
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data>rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['<', x]]

  # Greater than
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data<rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['>', x]]

  # Less than or equal to
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data>=rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['<=', x]]

  # Greater than or equal to
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data<=rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['>=', x]]

  # Logical AND
  def x(rt):
    x = bool(rt.Stack.pop().data)
    y = bool(rt.Stack.pop().data)
    rt.Stack.push(rtypes.typeint(x and y))
    return rt.Context.eval
  bins += [['and', x]]
  
  # Logical OR
  def x(rt):
    x = bool(rt.Stack.pop().data)
    y = bool(rt.Stack.pop().data)
    rt.Stack.push(rtypes.typeint(x or y))
    return rt.Context.eval
  bins += [['or', x]]
  
  # Logical NOT
  def x(rt):
    x = bool(rt.Stack.pop().data)
    rt.Stack.push(rtypes.typeint(not x))
    return rt.Context.eval
  bins += [['not', x]]
  
  
  ### List functions
  # Length of whatever.
  def x(rt):
    rt.Stack.push(rtypes.typeint(len(rt.Stack.pop().data)))
    return rt.Context.eval
  bins += [['len', x]]
  
  # Code has its trailing Return call suppressed.
  def x(rt):
    rt.Stack.push(rtypes.typeint(len(rt.Stack.pop().data)-1))
    return rt.Context.eval
  bins += [['lencode', x]]
  
  # Pop
  def x(rt):
    list = rt.Stack.pop()
    if len(list.data):
      newlist = list.data[:]
      thing = newlist.pop()
      rt.Stack.push(rtypes.typelst(newlist))
      rt.Stack.push(thing)
    else:
      rt.Stack.push(list)
      return rt.ded('Once you pop, you must eventually stop')
    return rt.Context.eval
  bins += [['pop', x]]
  
  # Break up a composite and stuff.
  def x(rt):
    obj = rt.Stack.pop().data
    for i in obj:
      rt.Stack.push(i)
    rt.Stack.push(rtypes.typeint(len(obj)))
    return rt.Context.eval
  bins += [['composite>', x]]
  
  # Un-binned, torn out of obj>.
  def x(rt):
    obj = rt.Stack.pop()
    rt.Stack.push(obj.tag)
    rt.Stack.push(obj.next)
    return rt.Context.eval
  bins += [['dir>', x]]
  
  # Return contents of tag
  def x(rt):
    obj = rt.Stack.pop()
    rt.Stack.push(obj.obj)
    rt.Stack.push(rtypes.typesym([obj.name]))
    return rt.Context.eval
  bins += [['tag>', x]]
  
  # Return contents of context
  def x(rt):
    obj = rt.Stack.pop()
    rt.Stack.push(rtypes.typeint(CALLDEPTH-obj.depth))
    rt.Stack.push(obj.code)
    rt.Stack.push(rtypes.typeint(obj.ip))
    rt.Stack.push(obj.names)
    rt.Stack.push(obj.next)
    return rt.Context.eval
  bins += [['context>', x]]
  
  # Construct a new context -- not quite reflective with above, because
  # depth is inferred from next context
  def x(rt):
    next = rt.Stack.pop()
    names = rt.Stack.pop()
    ip = rt.Stack.pop()
    code = rt.Stack.pop()
    newcontext = rtypes.typecontext(code, names, next)
    newcontext.ip = ip.data
    rt.Stack.push(newcontext)
    return rt.Context.eval
  bins += [['>context', x]]
  
  # List subset from left
  def x(rt):
    j = rt.Stack.pop().data
    lst = rt.Stack.pop()
    if j >= 0:
      if lst.typenum == rt.Types.id['String']:
        rt.Stack.push(rtypes.typestr(lst.data[:j]))
      else:
        lst = lst.cp()
        lst.data = lst.data[:j]
        rt.Stack.push(lst)
    else:
      rt.Stack.push(lst)
      rt.Stack.push(rtypes.typeint(j))
      return rt.ded('Ask at least for zero, maybe more')
    return rt.Context.eval
  bins += [['left', x]]
  
  # List subset from right
  def x(rt):
    j = rt.Stack.pop().data
    lst = rt.Stack.pop()
    if j >= 0:
      start = len(lst.data)-j
      start *= (start>=0)
      if lst.typenum == rt.Types.id['String']:
        rt.Stack.push(rtypes.typestr(lst.data[start:]))
      else:
        lst = lst.cp()
        lst.data = lst.data[start:]
        rt.Stack.push(lst)
    else:
      rt.Stack.push(lst)
      rt.Stack.push(rtypes.typeint(j))
      return rt.ded('Ask at least for zero, maybe more')
    return rt.Context.eval
  bins += [['right', x]]

  # List subset
  def x(rt):
    j = rt.Stack.pop().data
    i = rt.Stack.pop().data
    lst = rt.Stack.pop()
    if i >= 0 and i < len(lst.data):
      if lst.typenum == rt.Types.id['String']:
        rt.Stack.push(rtypes.typestr(lst.data[i:j+1]))
      else:
        lst = lst.cp()
        lst.data = lst.data[i:j+1]
        rt.Stack.push(lst)
    else:
      rt.Stack.push(lst)
      rt.Stack.push(rtypes.typeint(j))
      rt.Stack.push(rtypes.typeint(i))
      return rt.ded('It would help to have a valid starting subscript')
    return rt.Context.eval
  bins += [['subs', x]]

  # Get from list and evaluate
  def x(rt):
    i = rt.Stack.pop().data
    lst = rt.Stack.pop()
    if i >= 0 and i < len(lst.data):
      return lst.data[i].eval
    else:
      rt.Stack.push(lst)
      rt.Stack.push(rtypes.typeint(i))
      return rt.ded('This '+lst.typename+' deserves a better subscript')
    return rt.Context.eval
  bins += [['gete', x]]

  
  # Get from list
  def x(rt):
    i = rt.Stack.pop().data
    lst = rt.Stack.pop()
    if i >= 0 and i < len(lst.data):
      if lst.typenum == rt.Types.id['String']:
        rt.Stack.push(rtypes.typestr(lst.data[i]))
      else:
        rt.Stack.push(lst.data[i])
    else:
      rt.Stack.push(lst)
      rt.Stack.push(rtypes.typeint(i))
      return rt.ded('This '+lst.typename+' deserves a better subscript')
    return rt.Context.eval
  bins += [['get', x]]

  # Put to list
  def x(rt):
    i = rt.Stack.pop().data
    obj = rt.Stack.pop()
    lst = rt.Stack.pop()
    if i >= 0 and i < len(lst.data):
      lst = lst.cp()
      lst.data[i]=obj
      rt.Stack.push(lst)
    else:
      rt.Stack.push(lst)
      rt.Stack.push(obj)
      rt.Stack.push(rtypes.typeint(i))
      return rt.ded('This '+lst.typename+' deserves a better subscript')
    return rt.Context.eval
  bins += [['put', x]]
  
  # Make a list or convert code to list.
  def x(rt):
    items = rt.Stack.pop().data
    if len(rt.Stack)<items:
      rt.Stack.push(rtypes.typeint(items))
      return rt.ded('If you want '+str(items)+' things in a list, maybe you should have '+str(items)+' things on the stack')
    else:
      lst = rt.Stack.data[len(rt.Stack.data)-items:]
      rt.Stack.data = rt.Stack.data[:len(rt.Stack)-items]+[rtypes.typelst(lst)]
    return rt.Context.eval
  bins += [['>lst', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typelst(rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['composite>lst', x]]
  
  
  ### Error handling
  # Cause error
  def x(rt):
    return rt.ded(rt.Stack.pop().data)
  bins += [['ded', x]]

  # Assign blame
  def x(rt):
    rt.Caller = rt.Stack.pop()
    return rt.Context.eval
  bins += [['blame', x]]
  
  # ### Bitwise operations
  def x(rt):
    rt.Stack.push(rtypes.typeint(~rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['bnot', x]]

  def x(rt):
    bits = rt.Stack.pop().data
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data << bits))
    return rt.Context.eval
  bins += [['bshl', x]]
  
  def x(rt):
    bits = rt.Stack.pop().data
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data >> bits))
    return rt.Context.eval
  bins += [['bshr', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data & rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['band', x]]

  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data | rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['bor', x]]
  
  def x(rt):
    rt.Stack.push(rtypes.typeint(rt.Stack.pop().data ^ rt.Stack.pop().data))
    return rt.Context.eval
  bins += [['bxor', x]]

  return bins

# Store all the procedures we know how to make into an extant directory,
# in an extant runtime.
def stoprocs(rt, dir):
  for i in makebinprocs():
    rt.sto([dir]+[i[0]], rtypes.typebinproc(i[1]))
