
# CODSWALLOP RPL (a zen garden)
# #####################################################
# Runtime

# This module includes popular features such as the ability
# to execute RPL code and manipulate the named store.

from trivia import *
from rtypes import typedir, typelst, typerem, typeint, typestr, typetag, typecontext, typebinproc, typesym, typecode

# Drop out of a call unconditionally: 'ret'.
def ret(x):
  if x.Context is x.Context.next:
    # We're done here.  Erase the current context, such that a BEVAL will
    # replace it.
    x.Context.code = x.nullcode
    x.Context.ip = 0
    x.Running = False
  else:
    x.Context = x.Context.next
  return x.Context.eval
  
# Interpreter flags and stuff, easier to have in its own namespace.
class rplruntime:
  def __init__(self, types):
    # Types object.  These can be shared between instances.
    self.Types = types

    # Some helpful type constants.
    self.symtype = self.Types.id['Symbol']
    self.dirtype = self.Types.id['Directory']
    
    # Error state.  The caller string is generally set by builtins, and
    # the error flag is generally only set when errorcont is true.
    self.nullcaller = typestr('')
    self.rtcaller = typestr('a higher power')
    self.Caller = self.nullcaller
    self.Reason = ''
    self.Interrupt = False
    
    # Stack stack stack stack.
    self.Stack = typelst([])
    
    # Catch sigints with a bit more aplomb.
    self.Break = False
    self.dieanyway = False
            
    # Democratize the power to drop a context. 
    self.Return = typebinproc(ret)

    # An empty tag used as filler in directories.
    self.nulltag=typetag('', typerem('NIL'))
    
    # Make the self-referencing last entry for the named store.
    self.lastobj=typedir(self.nulltag, None)
    
    # Empty code also for filler.
    self.nullcode=typecode([self.Return])
    
    # Running flag is cleared when we're out of contexts.
    self.Running = True    

    # This is the first Context object.
    self.Context = typecontext(self.nullcode, self.firstdir())
    
    # And for our opening act, populate our types within the named store.
    types.updatestore(self)
    

  # Runtime error handler.  This attempts to force a new context and evaluate
  # the exception object within it.
  def ded(self, reason):
    # Hang onto the reason.
    self.Reason = reason

    # Force a new context.  This exempts the error handler from the 
    # recursion limit, and also keeps it from landing on top of code we might
    # like to trace back through.
    self.Context = typecontext(self.nullcode, self.Context.names, self.Context)
    
    # We might end up here due to excess recursion.  That's fine, but if it's
    # because the handler itself threw an error, that could quickly get out of 
    # hand.  If we're already into negative depths, we've been betrayed thus.
    if self.Context.depth < -1:
      print('...panik!  Excess recursion while already trying to handle an error')
      self.Running = False 
      
    return typesym(DEDEVAL).eval
      
  # "Run/stop": The innermost loop.  It accepts an object's eval method;
  # each eval method returns the next eval method.  This loop continues
  # until the Running flag is cleared (either under program control, or when
  # the lowest Context object runs out of things to do.)
  def rs(self, next):
    while self.Running:
      next = next(self)

  # Queue a new context.  This will add a line to the call stack unless there
  # is a tail call to optimize.
  def newcall(self, obj):
    if self.Context.code.data[self.Context.ip] is self.Return:
      self.Context.ip = 0
      self.Context.code = obj
    else:
      if self.Context.depth:
        self.Context = typecontext(obj, self.Context.names, self.Context)
      else:
        self.Caller = self.rtcaller
        return self.ded('You asked for '+str(CALLDEPTH)+' recursions and not a penny more')
    return self.Context.eval

  # New call with locals.
  def newlocall(self, obj, names):
    if self.Context.code.data[self.Context.ip] is self.Return:    
      self.Context.ip = 0
      self.Context.code = obj
      self.Context.names = self.firstdir(names)
    else:
      if self.Context.depth:
        self.Context = typecontext(obj, self.firstdir(names), self.Context)
      else:
        self.Caller = self.rtcaller
        return self.ded('You asked for '+str(CALLDEPTH)+' recursions and not a penny more')       
    return self.Context.eval


  # #####################################################
  # Hierarchical named store routines.

  # Prepare a new first directory entry.  Only one lastobj is required, but
  # first entries are all unique.
  def firstdir(self, obj=None):
    if obj is None:
      obj = self.lastobj
    # Nulltag is a null-named tag containing a null remark.
    return typedir(self.nulltag, obj)
  
  # Try to find an object.
  def rcl(self, namelist):
    # Start from the top.
    current = self.Context.names
    for i in namelist:
      # Make sure we're about to parade through an actual directory first.
      if current.typenum == self.dirtype:
        # Start the parade, and return nothing if we didn't find a match.
        while current.tag.name != i:
          current = current.next
          if current is self.lastobj:
            return
        # If we got here, we did find a match, so return the object in it.
        current = current.tag.obj
      else:
        return
    return current

  # Modified rcl, deref, which returns the tag and not the obj.
  def deref(self, namelist):
    # Start from the top.
    current = self.Context.names
    for i in range(len(namelist)):
      # Make sure we're about to parade through an actual directory first.
      if current.typenum == self.dirtype:
        # Start the parade, and return nothing if we didn't find a match.
        while current.tag.name != namelist[i]:
         current = current.next
         if current is self.lastobj:
           return
        # If we got here, we did find a match, so return the object in it.
        if i+1 == len(namelist):
          return current.tag
        else:
          current = current.tag.obj
      else:
        return

  # Try to store an object.
  def sto(self, namelist, value):
    # Counter
    counter = len(namelist)-1
    # Start from the top.
    current = self.Context.names
    for i in namelist:
      # Make sure we're about to parade through an actual directory first.
      if current.typenum == self.dirtype:
        # Start the parade.
        while current.tag.name != i:
          if current.next is self.lastobj:
            # If we failed to find a subdirectory somewhere, cheese it.
            if counter:
              return False
            # But if we got to the end of our name list, append a new entry.
            else:
              current.next = typedir(typetag(i, value), self.lastobj)
              return True
          current = current.next
        # If we got here, we found a match, so decrement our counter.
        # But don't return the object unless we're still chasing down the tree.
        if counter:
          counter -= 1
          current = current.tag.obj
      # Walked out of the directory tree?  That's a big fat False.
      else:
        return False
    # If we got down here, we found an extant entry to update.
    current.tag.obj = value
    return True


  # Rummage through an already-stored directory tree and see if any symbols 
  # within it circulate (to a point.)
  def circdir(self, dirtop):
    def recurse(prefix, dirtop, depth):
      # Anything past our recursion limit is assumed good.
      if depth:
        dirtop = dirtop.next
        while dirtop is not self.lastobj:
          # If we find a symbol, return true if circsym says it circulates.
          if dirtop.tag.obj.typenum == self.symtype:
            if self.circsym(prefix+dirtop.tag.obj.data):
              return True
          # And if we find another directory, recurse and return true if
          # something further down the line claims to circulate.
          elif dirtop.tag.obj.typenum == self.dirtype:
            if recurse(prefix+[dirtop.tag.name], dirtop.tag.obj, depth-1):
              return True
          dirtop = dirtop.next
      # After traversing the whole list, if we didn't return True before, we
      # made it.
      return False
    return recurse([], dirtop, CPDEPTH)
    
  # Check if an already-stored item circulates.
  def circsym(self, proposedname):
    # Recall a symbol.
    names = [proposedname]
    symbol = self.rcl(proposedname)

    while True:
      if symbol == None:
        # If the thing our thing points to is nonexistent, no circulation.
        return False
      # If the object we recalled is a symbol, we'll check for circulation
      # and loop.  If we got to a non-symbolic object, there is no problem.
      if symbol.typenum == self.symtype:
        # If the thing our thing points to is in this chain, something
        # is circulating, even if it isn't what we're storing.
        if symbol.data in names:
          return True
        else:
          names += [symbol.data]
          symbol = self.rcl(symbol.data)
      else:
        return False

  def rm(self, namelist):
    # Start from the top.
    current = self.Context.names
    last = current
    for i in namelist:
      # Make sure we're about to parade through an actual directory first.
      # We also exempt empty directories here.
      if current.typenum == self.dirtype and \
         current.next is not self.lastobj:
        # Start the parade, and return False if we didn't find a match.
        # We check the next link's name to a) skip the null-named firstdir
        # and b) hang onto the current link to update its nextobj.
        while current.next.tag.name != i:
         current = current.next
         if current.next is self.lastobj:
           return False
        # If we got here, we did find a match, so return the object in it,
        # but keep our last link for the final event.
        last = current
        current = current.next.tag.obj
      else:
        return False
    # And if we got here, we don't care much about what the current object is;
    # we're just going to pop it out of the chain.
    last.next = last.next.next
    return True
