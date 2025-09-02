
# CODSWALLOP RPL (a zen garden)
# #####################################################
# Types

# This contains all the object type classes, and also includes the
# rpltypes class, which is what the runtime takes.  The runtime also
# requires this module, since it needs a few of these types for its
# own purposes, but more types can be added freely.

# Normally one would just call baseregistry(), which returns an
# rpltypes object prefabulated with all the types in this module.
# From there, more types can be added with the rpltypes.register method,
# with the rpltypes.updatestore method called afterward to keep the
# in-interpreter Types directory up to date.

from trivia import *
import parse

import copy

# Type registry.  This contains a dictionary matching human readable
# names with type numbers, a matching list to do the reverse, and a
# list of classes matching these types for parsing purposes.  Calling
# register(class) will add a new type, and updatestore() will record
# the registry into the Types directory within RPL.

class rpltypes:
  def __init__(self):
    # Our name to number dictionary.
    self.id = {'Any': 0}
    # Our number to name list.
    self.n = ['Any']
    # Our list of classes for the parser.
    self.parsetypes = []
    # Finally, reference to the object class itself.
    self.usrproto = {}

  def register(self, obj):
    newnumber = len(self.n)
    # Put new objects at the beginning of the parse list.  This way,
    # unquoted symbols can be added first and always get last priority.
    # The preprocessor still goes first though.
    self.parsetypes = [obj] + self.parsetypes
    self.id[obj.typename] = newnumber
    self.n += [obj.typename]
    obj.typenum = newnumber

  # Register a new user-created type.
  def registerusr(self, obj):
    newnumber = len(self.n)
    self.id[obj.typename] = newnumber
    self.n += [obj.typename]
    obj.typenum = newnumber
    self.usrproto[obj.typename] = obj
      
  def updatestore(self, runtime):
    # Create a new Types directory and populate it with type names
    # containing their matching type numbers, as well as a list 'n'
    # with all the names for given numbers.
    # We do have to reverse the reverse list since parsetypes is backwards.
    runtime.sto(['Types'], runtime.firstdir())
    reverse = []
    runtime.sto(['Types','Any'],typeint(0))
    for i in self.parsetypes:
      runtime.sto(['Types',i.typename], typeint(i.typenum))
      reverse = [typestr(i.typename)]+reverse
    for i in self.usrproto.values():
      runtime.sto(['Types',i.typename], typeint(i.typenum))
      reverse += [typestr(i.typename)]
    reverse = [typestr('Any')]+reverse
    runtime.sto(['Types','n'], typelst(reverse))
    if len(self.usrproto):
      runtime.sto(['Types','Proto'], runtime.firstdir())
      for i in self.usrproto:
        runtime.sto(['Types','Proto',i], self.usrproto[i])


# Archetypal object.  An RPL typed object always contains a type number
# and the actual data.  It will also, at minimum, have these methods.
class objarchetype:
  # The human-readable name of our type.
  typename = 'Archetype'
  
  # And a number to be filled in when the type is registered.
  typenum = None
  
  # A hint about its function, for builtins.
  hint = None

  # A string parser, which receives a token, attempts to turn it into an
  # object, and returns some indication of why it can't, if it can't.
  # This is a function, not a method.
  def parse(token):
    pass

  # A constructor to initialize the data payload common to all objects.
  def __init__(self, x=None):
    if x != None:
      self.data = x

  # Return a duplicate object.  For immutable types, it returns itself.
  def cp(self):
    return self
    
  # A self-evaluation routine, which usually pushes the object to the stack.
  def eval(self, runtime):
    runtime.Stack.push(self)
    return runtime.Context.eval


# Binary call, the barest wrapper around a Python function.
class typebinproc(objarchetype):
  typename = 'Internal'
  data = '(internal)'
  def __init__(self, procedure):
    self.eval = procedure

# Call context, the basis for the call stack.
#   code: the code object for this context
#   names: the first name for this context
#   next: the next context
#   ip: instruction pointer
#   new: 
class typecontext(objarchetype):
  typename = 'Context'
  data = '(context)'
  def __init__(self, code, names, next=None):
    self.code = code
    self.names = names
    if next is None:
      self.next = self
      self.depth = CALLDEPTH
    else:
      self.next = next
      self.depth = next.depth-1
    self.ip = 0
    
  def eval(self, runtime):
    # Check for ^C before actually doing the next thing.
    if runtime.Break:
      runtime.Break = False
      runtime.Interrupt = True
      return runtime.ded('Break')

    next = self.code.data[self.ip].eval
    self.ip += 1
    return next

# Integer type.
class typeint(objarchetype):
  typename = 'Integer'
  
  def parse(token):
    # Common failure routine.
    def parseerror():
      token.invalidate('This integer is barely even an integer at all')
      
    # All integers start with a pound sign.
    cursor = token.cursor   
    if token.text[cursor] == '#':
      cursor += 1
      prefix = ''
      if cursor >= len(token.text):
        parseerror()
        return
      # Check for optional sign.
      if token.text[cursor] == '+':
        cursor += 1
      elif token.text[cursor] == '-':
        prefix = '-'
        cursor += 1
        
      # Now rummage for numbers.
      text, cursor = parse.getnumber(token.text, cursor)
            
      # Now try to make whatever we got be a number.  Parsenumber must have
      # returned something, and that something must have been followed by
      # whitespace or EOF to be valid.
      if len(text) and\
         (cursor == len(token.text) or token.text[cursor] in token.whitespace):
        try:
          # If this works, advance the cursor and return our object.
          token.validnext(typeint(int(prefix+text)), cursor)
        except:
          parseerror()
      else:
        parseerror()      
    
  def __init__(self, x):
    self.data = int(x)


# Float type.
class typefloat(objarchetype):
  typename = 'Float'
  
  def parse(token):
    # Common failure routine.
    def parseerror():
      token.invalidate('This number is not much of a float')
              
    # First, check for a sign.
    cursor = token.cursor
    text = ''	
    if token.text[cursor] == '+':
      cursor += 1
    elif token.text[cursor] == '-':
      text += '-'
      cursor += 1      
    
    # Second, try to read an integer or decimal point, but fall through 
    # error-free otherwise.
    if cursor < len(token.text) and token.text[cursor] in '.0123456789':
      # This will be our integer portion, if there is one.
      value, cursor = parse.getnumber(token.text, cursor)
      text += value
                  
      # If we have a decimal point, add it and whatever integer may follow.
      if cursor < len(token.text) and token.text[cursor] == '.':
        cursor += 1
        value, cursor = parse.getnumber(token.text, cursor)
        text += '.' + value

      # If there's an exponent, add it and whatever integer may follow also.
      if cursor+1 < len(token.text) and token.text[cursor] == 'e':
        cursor += 1
        text += 'e'
        if token.text[cursor] == '+':
          cursor += 1
        elif token.text[cursor] == '-':
          text += '-'
          cursor += 1      

        value, cursor = parse.getnumber(token.text, cursor)
        # If there's an e but no exponent, fail.
        if len(value):
          text += value
        else:
          parseerror()
        
      # If we're at whitespace or EOF, return an object.  If there's
      # trailing garbage, raise an error.
      if cursor >= len(token.text) or token.text[cursor] in token.whitespace:
        try:
          # If this works, advance the cursor and return our object.
          token.validnext(typefloat(float(text)), cursor)
        except:
          parseerror()      
      else:
        parseerror()

  def __init__(self, x):
    self.data = float(x)


# String type.
class typestr(objarchetype):
  typename = 'String'

  def parse(token):    
    # All strings begin and end with a quote.
    cursor = token.cursor
    if token.text[cursor] == '"':
      cursor += 1
      ourtext, cursor = parse.getstring(token.text, cursor, '"')
      if cursor < len(token.text) and token.text[cursor] == '"':
        token.validnext(typestr(ourtext), cursor+1)
      else:
        token.invalidate("Strings don't just start with quotes")
        
  def __init__(self, x):
    self.data = str(x)


# Generic quote type.  When evaluated, it returns its contents, useful for
# preventing the immediate evaluation of code and symbols.
class typequote(objarchetype):
  typename = 'Quote'
  def parse(token):
    # Quotes start with an apostrophe.
    if token.text[token.cursor] == "'": 
      token.cursor += 1
      token.whiteskip()
      # Catch quotes not followed by any text.
      if token.stop:
        token.invalidate("Quote... what", token.cursor)
      else:
        cursor = token.cursor
        token.nextobj()
        
        # Invalidated tokens fall through with whatever error they may have
        # flagged.  We must also check that no alt-mode comments have 
        # slipped through, for example.
        if token.valid:
          if token.data is None:
            token.invalidate("You should put something corporeal here", cursor)
          else:
            token.validnext(typequote(token.data), token.cursor)

  def eval(self, runtime):
    runtime.Stack.push(self.data)
    return runtime.Context.eval
    

# Symbol type.  This evaluates whatever it's pointed to as soon as it's
# encountered.
class typesym(objarchetype):
  typename = 'Symbol'
  def __init__(self, x):
    self.data = x
  
  def parse(token):    
    cursor = token.cursor
    ourtext, cursor = parse.getstring(token.text, cursor, token.whitespace)
    
    if parse.validatename(ourtext):
      # Split up our directory tree here.
      ourtext = ourtext.split('.')
      # Alternate flag indicates a request to recall the symbol right now.
      if token.alternate:
        thing = token.runtime.rcl(ourtext)
        if thing is None:
          token.invalidate("This symbol has to exist at parse time")
        else:
          token.validnext(thing, cursor)
      else:
        token.validnext(typesym(ourtext), cursor)
    else:
      token.invalidate("Are you trying to break shit with delimiters in symbol names?")
  
  # Evaluating a symbol attempts to retrieve it by name and evaluate that.
  def eval(self, runtime):
    x = runtime.rcl(self.data)
    if x is None:
      # Couldn't find symbol.
      runtime.Caller = runtime.rtcaller
      oursym = symtostr(self.data)        
      return runtime.ded('We seek '+oursym+' but we cannot always find '+oursym)
    elif runtime.Break:
      # Most but not all circular references are caught at store time, so
      # we catch ^C here too.
      runtime.Interrupt = True
      runtime.Break = False
      return runtime.ded('Break')
    else:
      # We did retrieve something, so pass it along to be evaluated.
      return x.eval
    

# Comment string.  A special case string that's retained in programs and lists
# but vanishes when evaluated.
class typerem(typestr):
  typename = 'Comment'

  def parse(token):    
    # Comments begin with ( and end with ).
    if token.text[token.cursor] == ')':
      token.invalidate("This looks like a shut and open case")
    elif token.text[token.cursor] == '(':
      depth = 1
      cursor = token.cursor+1
      # Scan text for matching nested parentheses, so blocks of code can be
      # easily commented out without escaping all the close parens.
      while cursor < len(token.text) and depth:
        depth += (token.text[cursor] == '(') - (token.text[cursor] == ')')
        cursor += 1
      # Remaining depth means we ran out of text to scan.
      if depth:
        token.invalidate("These remarks have gone on far too long")
      else:
        if token.alternate:
          token.validnext(None, cursor)
        else:
          token.validnext(typerem(token.text[token.cursor+1:cursor-1]), cursor)
  
  def eval(self, runtime):
    return runtime.Context.eval
  

# Directory type.  
# A specific purpose, singly linked list used for named storage:
# 'tag' is a Tag,
# 'next' is the next Directory.
class typedir(objarchetype):
  typename = 'Directory'
  
  def __init__(self, name, nextobj):
    self.tag = name
    if nextobj is None:
      self.next = self
    else:
      self.next = nextobj
    # Here so == can hopefully tell us apart by address.
    self.data = self

  def parse(token):
    # Just to be a good sport, catch spurious closed brackets too.
    if token.text[token.cursor] == ']':
      token.invalidate('Wherever this was supposed to go, it wasn\'t here')
    elif token.text[token.cursor:token.cursor+5] == '[dir:':
      # Start digging for tags.
      alternate = token.alternate
      oldcursor = token.cursor
      token.cursor += 5
      token.whiteskip()
      firstdir = token.runtime.firstdir()
      nextdir = firstdir
      running = True
      
      while running:
        # If we hit the end, that means we're missing a close bracket.
        if token.stop:
          token.invalidate('A directory has failed to ]', oldcursor)
          running = False
        # If we found a close bracket, we're done here.
        elif token.text[token.cursor] == ']':
          running = False
          token.validnext(firstdir, token.cursor+1)
        # Otherwise it's tag time.
        else:
          # Only try to parse a tag.
          token.alternate = alternate
          typetag.parse(token)
          # We got one, so add it to the chain.
          if token.valid:
            token.valid = False
            nextdir.next = typedir(token.data, nextdir.next)
            nextdir = nextdir.next
          # Or tag threw an error, in which case pass it along.  
          elif token.stop:
            running = False
          # Or tag didn't throw an error, in which case it wasn't a tag.
          else:
            token.invalidate('Directories can only contain tags')
            running = False
   
  # Duplicating a directory is trickier, because all entries and tags
  # need to be copied.  This was so hairy I had to take a shower to make it.
  def cp(self, depth=CPDEPTH):
    if depth:
      # We have to hang onto 'ourcopy' because that's what we're returning.
      # Rest is our new directory entry of interest, and current is the old
      # structure we're following.
      ourcopy = typedir(self.tag.cp(), self.next)
      rest = ourcopy
      current = self
      # Lastobjs point to themselves, so we only follow til we get to a self-ref.
      while current.next is not current.next.next:
        # Each new 'rest' initially points back to the original list, retaining
        # our global lastobj when we drop out of the loop.
        current = current.next
        rest.next = typedir(rest.next.tag.cp(), current.next)
        rest = rest.next
        # We have to recurse to catch subdirectories, but only to a point.
        if rest.tag.obj.typenum == self.typenum:
          rest.tag.obj = rest.tag.obj.cp(depth-1)
      return ourcopy
    else:
      # If we're out of recursion depth, silently return the original.
      return self
    

# IO type.  Used as handles for files and character devices, probably.
class typeio(objarchetype):
  typename = 'Handle'
  
  # Python's EOF handling is kind of garbage, but I feex.
  eof = False
  def __init__(self, data):
    self.data = data
  def __del__(self):
    try:
      self.data.close()
    except:
      print('By the way, a terrible fate has befallen a forgotten file handle')


# Tag type.
# Also a specific purpose thing used for named storage:
# 'name' is a string, an unqualified name with no periods
# 'obj' is any old thing, but has to be an RPL type.
# Tags are expressly mutable and can be used to pass a reference
# when that's useful, such as for closures, or when the original
# name may be covered by a local variable.  It's also the basis
# of the user type scheme.
class typetag(objarchetype):
  typename = 'Tag'
  
  def __init__(self, data, obj):
    self.name = data
    self.obj = obj
    # This is so == can hopefully tell if we're equal by address.
    # Should now be obsolete since SAME exists.
    self.data = self

  def usreval(self, runtime):
    return runtime.Context.eval

  def eval(self, runtime):
    runtime.Stack.push(self)
    return self.usreval

  def cp(self):
    # Make a copy of the tag, but not the object we contain.
    newtag = typetag(self.name, self.obj)
    # And save our type number, because it may have changed.
    newtag.typenum = self.typenum
    return newtag
    
  def parse(token):    
    # Tags will look like :name:thing, so we have to make sure we're
    # not actually looking at the beginning of a :: code block.
    cursor = token.cursor
    if token.text[cursor] == ":":
      # Increment our cursor and see what we got for a name.
      cursor += 1
      ourtext, nextcursor = parse.getstring(token.text, cursor, ":")
      
      # If the cursor did not advance, that means we found ::.
      if nextcursor != cursor:
        # It's possible a chucklefuck could end a sentence with a colon,
        # or fail to close the name section.
        cursor = nextcursor
        if cursor < len(token.text) and token.text[cursor] == ":":
          # And it's also possible they didn't give us a valid name.
          if parse.validatename(ourtext) and not '.' in ourtext:
            # But if they did, now increment and try to retrieve a valid object.
            token.cursor = cursor+1
            token.whiteskip()
            cursor = token.cursor
            if token.stop:
              # A stop here means EOF before we even try for an object.
              token.invalidate("Half a tag yields none of the goods")
            else:
              token.nextobj()            
              # Invalid tokens fall through with their errors intact.
              if token.valid:
                if token.data is None:
                  token.invalidate("You should put something corporeal here", cursor)
                else:
                  token.validnext(typetag(ourtext, token.data), token.cursor)
          else:
            token.invalidate("This must be a kind-hearted, pure, and dotless symbol", token.cursor+1)
        else:
          token.invalidate("Colon what, dear")


# List type.
class typelst(objarchetype):
  typename = 'List'

  def parse(token):
    # Just to be a good sport, catch spurious closed brackets too.
    if token.text[token.cursor] == '}':
      token.invalidate('Wherever this was supposed to go, it wasn\'t here')
    elif token.text[token.cursor] == '{':
      ourlist = typelst([])
      parse.parsecomposite(token, ourlist, 1, '}')
      # If parsecomposite wasn't stopped, there was no error or EOF.
      if not token.stop:
        token.validnext(ourlist, token.cursor+1)
  
  def __init__(self, x=None):
    if x:
      self.data = x
    else:
      self.data = []

  def __len__(self):
    return len(self.data)
    
  # Duplicating a list makes a new list, but points to old objects.
  def cp(self):
    newme = copy.copy(self)
    newme.data = newme.data[:]
    return newme

  # Helpers for stack use.
  def push(self, value):
    self.data.append(value)
  def pop(self):
    if len(self.data):
      return self.data.pop()
    else:
      return None

 
# Code type.  This is very much a list.
class typecode(typelst):
  typename = 'Code'
  
  def parse(token):
    # Catch semicolons here; if they got this far, they're spurious.
    if token.text[token.cursor] == ';':
      token.invalidate('Perhaps this semicolon should be somewhere else')
    elif token.text[token.cursor:token.cursor+2] == '::':
      ourcode = typecode([])
      parse.parsecomposite(token, ourcode, 2, ";")
      # If parsecomposite wasn't stopped, there was no error or EOF.
      if not token.stop:
        # Code lists end with a mandatory internal which drops the current
        # context, permitting the runtime loop to be a little dumber.
        ourcode.data += [token.runtime.Return]
        token.validnext(ourcode, token.cursor+1)
        
  # Evaluating code pushes it to the call stack rather than the data stack.
  def eval(self, runtime):
    return runtime.newcall(self)

    
# RPL built-in command type.  This does basic type and argument count
# checking.  It then hops along to dispatch where the command-specific work
# is done.

# For legibility purposes when printing code, data equals the name of the
# function.

class typebin(objarchetype):
  typename = 'Builtin'
  
  data = 'NOP'
  hint = 'A very lazy person has declined to document this built-in.'

  # A list of possible stack configurations.
  argck = []
  # A matching list of dispatch functions.
  dispatches = []
  # Number of expected arguments.
  argct = 0
  
  # Conceptually useful for saving a snapshot of a builtin before hooking it.
  def cp(self):
    newbin = typebin()
    newbin.data = self.data
    newbin.argck = self.argck[:]
    newbin.dispatches = self.dispatches[:]
    newbin.argct = self.argct
    newbin.hint = self.hint
    return newbin
  
  def evalnotimplemented(self, runtime):
    # Preemptively claim responsibility for errors.
    runtime.Caller = self
    
    # First check to see that we have enough arguments.
    if len(runtime.Stack)<self.minargs:
      return runtime.ded('How about '+str(self.minargs)+' arguments instead of '+\
          str(len(runtime.Stack))+'?')
    else:
      # We do have enough, so what are their types?
      types = [None]
      for arg in runtime.Stack.data[len(runtime.Stack.data)-self.minargs:]:
        types += [arg.typenum]
      # Search dispatch table top to bottom.
      for line in self.table:
        match = True
        i = self.minargs
        # Check each argument to match type number.  0 will match any type.
        while match and i:
          if line[i] and line[i] != types[i]:
            match = False
          i -= 1
        # Suggest the runtime call the first matching dispatch.
        if match: 
          return line[i].eval
      # It is an error to fall out the bottom of the dispatch table.
      return runtime.ded('There are '+str(len(self.table))+' ways to call and you tried #'+\
      str(len(self.table)+1))

  
  def eval(self, runtime):
    # Preemptively claim responsibility for errors.
    runtime.Caller = self
    
    # First check to see that we have enough arguments.
    if len(runtime.Stack)<self.argct:
      return runtime.ded('How about '+str(self.argct)+' arguments instead of '+\
          str(len(runtime.Stack))+'?')
    else:
      # We do have enough args, so what are they?
      wegot = []
      for i in range(len(runtime.Stack.data)-self.argct, 
                     len(runtime.Stack.data)):
        wegot += [runtime.Stack.data[i].typenum]

      for i in range(len(self.argck)):
        # Check each argument to match type number.  0 will match any type.
        match = True
        for j in range(self.argct):
          if self.argck[i][j] and self.argck[i][j] != wegot[j]: match = False
        # Suggest the runtime call the first matching dispatch.
        if match: 
          return self.dispatches[i].eval
          
      return runtime.ded('There are '+str(len(self.argck))+' ways to call and you tried #'+\
      str(len(self.argck)+1))
  

# Prepare an rpltypes object to hand to the runtime, containing our basic
# types.
def baseregistry():
  Types = rpltypes()
  for i in [typecontext, typebinproc, typesym, typefloat, typestr, typerem,
            typebin, typedir, typetag, typelst, typecode, typeint, typeio,
            typequote]:
    Types.register(i)
  return Types

# Return an unparsed dot name from a symbol list.
def symtostr(sym):
  decimalname = sym[0]
  for i in range(1, len(sym)):
    decimalname += '.'+sym[i]
  return decimalname
