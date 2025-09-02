
# CODSWALLOP RPL (a zen garden)
# #####################################################
# Parser

# Or really parse coordinator.  The actual per-type parsing is done by
# functions for each type class, and coordinated by parse() and various
# helpers.

# Parse token.  This is handed back and forth between the parser and different
# registered object types to turn text into code.  It also contains the
# common methods whiteskip, nextobj, and the callback method validnext.

from trivia import *

class parsetoken:
  whitespace = ' \t\r\n'
  delimiters = ['}', '{', ':', ';', '[', ']']
  
  def __init__(self, runtime, text=''):
    self.text = text		# The string to parse
    self.cursor = 0		# Current position within string
    self.runtime = runtime	# Current runtime
    self.types = runtime.Types  # Current types object
    self.valid = False		# Flag: current object is valid
    self.stop = False   	# Flag: stop parsing, either error or done
    self.alternate = False	# Flag: alternate mode (preprocess)
    self.data = None    	# Current object
    self.error = ''		# Error message text on invalid stop
    self.whiteskip()		# Advance past any starting whitespace.

  # Skip all the whitespace under the cursor.
  def whiteskip(self):
    while self.cursor < len(self.text) and \
          self.text[self.cursor] in self.whitespace:
      self.cursor += 1
    # And if we reached the end, stop.
    if self.cursor >= len(self.text):
      self.stop = True
  
  # Click through one object.
  def nextobj(self):
    # Check for preprocess (`) or anti-preprocess (~) character.
    while not self.stop and (self.text[self.cursor] == '`' or 
          self.text[self.cursor] == '~'):
      self.alternate = self.text[self.cursor] == '`'
      self.cursor += 1
      self.whiteskip()
    
    if not self.stop:
      # Reset valid flag.
      self.valid = False
      # Bounce through all the types, and if we get one, or one throws an
      # error, stop there.
      for i in self.types.parsetypes:
        i.parse(self)
        if self.valid or self.stop:
          return
      # If no type claimed ownership, that too is an error.
      self.invalidate('Whatever this is, it isn\'t')
      
  # Callback method: record valid data returned by an object's parser, 
  # and skip cursor ahead.  Reset the alt mode flag if necessary.
  def validnext(self, data, cursor):
    self.data = data
    self.valid = True
    self.alternate = False
    self.cursor = cursor
    self.whiteskip()
    
  # Callback method: record an error message from an object's parser.
  def invalidate(self, message, cursor=None):
    if cursor is not None:
      self.cursor = cursor
    self.error = message
    self.valid = False
    self.stop = True
      
# Parse helpers.

# Look only for numerals and return the final cursor position and whatever
# we got.
def getnumber(text, cursor):
  number = ''
  while cursor < len(text) and text[cursor] in '0123456789':
    number += text[cursor]
    cursor += 1
  return number, cursor

# Retrieve a text string up to a closing character or EOF, and do some
# rudimentary escape character things.
def getstring(text, cursor, delimiter):
  newstring = ''
  while cursor < len(text) and not text[cursor] in delimiter:
    if text[cursor] == '\\':
      cursor += 1
    if cursor < len(text):
      newstring += text[cursor]
      cursor += 1
  return newstring, cursor

# Build a list within a composite type.
def parsecomposite(token, obj, delta, delimiter):
  # Hang onto our starting cursor and alt setting.
  cursor = token.cursor
  alternate = token.alternate
  
  # Advance cursor to the meat and/or potatoes.
  token.cursor += delta
  token.whiteskip()
  
  # Initially declare our token valid to cover null strings.
  token.valid = True
  while token.valid and not token.stop and \
        token.text[token.cursor] != delimiter:
    # Then try to grab a new object.
    token.alternate = alternate
    token.nextobj()
    if token.valid and token.data is not None:
      # Silently skip a valid None (e.g. an alternate comment.)
      obj.data += [token.data]
  
  # Check to see if we got to the end without stopping for a delimiter.
  if token.valid and token.stop and len(delimiter):
    token.invalidate('Consider ending this '+obj.typename+' with a '+delimiter, cursor)


# Check to see if text contains any symbolic naughties.
def validatename(text):
  for i in range(len(text)):
    if text[i] in parsetoken.delimiters or\
       text[i] in parsetoken.whitespace:
      return False
  return True

# Squeeze one object out of text.
def parse(runtime, text):
  token = parsetoken(runtime, text)
  token.nextobj()
  
  # Did we receive something valid?
  if token.valid:
    # Yes.  Print a warning if there was any trailing garbage.
    if not token.stop:
      print('Ignoring spurious text:',token.text[token.cursor:])
    return token.data
  else:
    # If invalid, try to show the user roughly where things went sideways.
    print('\nYour words fail to become actions.\n')
    # Get our overall line number.
    linenum = token.text[:token.cursor].count('\n')+1
    # Look back from the cursor to find our last newline.
    newlinecursor = 0
    spotonline = 0
    for i in range(token.cursor-1, -1, -1):
      if token.text[i] == '\n':
        newlinecursor = i+1
        break
      else:
        spotonline += 1
    # Then just fetch this exact line and show cursor position.
    print('Stopped on line '+str(linenum)+', position '+str(spotonline+1)+':')
    print(token.text[newlinecursor:].split('\n')[0])
    print(' '*spotonline+'↳✞')
    print('In particular:', token.error)    
    # And return nothing.
