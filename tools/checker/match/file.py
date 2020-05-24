# Copyright (C) 2014 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from collections                      import namedtuple
from common.immutables                import ImmutableDict
from common.logger                    import Logger
from file_format.c1visualizer.struct  import C1visualizerFile, C1visualizerPass
from file_format.checker.struct       import CheckerFile, TestCase, TestStatement
from match.line                       import MatchLines, EvaluateLine

MatchScope = namedtuple("MatchScope", ["start", "end"])
MatchInfo = namedtuple("MatchInfo", ["scope", "variables"])

class MatchFailedException(Exception):
  def __init__(self, statement, lineNo, variables):
    self.statement = statement
    self.lineNo = lineNo
    self.variables = variables

def findMatchingLine(statement, c1Pass, scope, variables, excludeLines=[]):
  """ Finds the first line in `c1Pass` which matches `statement`.

  Scan only lines numbered between `scope.start` and `scope.end` and not on the
  `excludeLines` list.

  Returns the index of the `c1Pass` line matching the statement and variables
  values after the match.

  Raises MatchFailedException if no such `c1Pass` line can be found.
  """
  for i in range(scope.start, scope.end):
    if i in excludeLines: continue
    newVariables = MatchLines(statement, c1Pass.body[i], variables)
    if newVariables is not None:
      return MatchInfo(MatchScope(i, i), newVariables)
  raise MatchFailedException(statement, scope.start, variables)

class ExecutionState(object):
  def __init__(self, c1Pass, variables={}):
    self.cursor = 0
    self.c1Pass = c1Pass
    self.c1Length = len(c1Pass.body)
    self.variables = ImmutableDict(variables)
    self.dagQueue = []
    self.notQueue = []

  def moveCursor(self, match):
    assert self.cursor <= match.scope.end

    # Handle any pending NOT statements before moving the cursor
    self.handleNotQueue(MatchScope(self.cursor, match.scope.start))

    self.cursor = match.scope.end + 1
    self.variables = match.variables

  def handleDagQueue(self, scope):
    """ Attempts to find matching `c1Pass` lines for a group of DAG statements.

    Statements are matched in the list order and variable values propagated. Only
    lines in `scope` are scanned and each line can only match one statement.

    Returns the range of `c1Pass` lines covered by this group (min/max of matching
    line numbers) and the variable values after the match of the last statement.

    Raises MatchFailedException when a statement cannot be satisfied.
    """
    if not self.dagQueue:
      return

    matchedLines = []
    variables = self.variables

    for statement in self.dagQueue:
      assert statement.variant == TestStatement.Variant.DAG
      match = findMatchingLine(statement, self.c1Pass, scope, variables, matchedLines)
      variables = match.variables
      assert match.scope.start == match.scope.end
      assert match.scope.start not in matchedLines
      matchedLines.append(match.scope.start)

    match = MatchInfo(MatchScope(min(matchedLines), max(matchedLines)), variables)
    self.dagQueue = []
    self.moveCursor(match)

  def handleNotQueue(self, scope):
    """ Verifies that none of the given NOT statements matches a line inside
        the given `scope` of `c1Pass` lines.

    Raises MatchFailedException if a statement matches a line in the scope.
    """
    for statement in self.notQueue:
      assert statement.variant == TestStatement.Variant.Not
      for i in range(scope.start, scope.end):
        if MatchLines(statement, self.c1Pass.body[i], self.variables) is not None:
          raise MatchFailedException(statement, i, self.variables)
    self.notQueue = []

  def handleEOF(self):
    """ EOF marker always moves the cursor to the end of the file."""
    match = MatchInfo(MatchScope(self.c1Length, self.c1Length), None)
    self.moveCursor(match)

  def handleInOrder(self, statement):
    """ Single in-order statement. Find the first line that matches and move
        the cursor to the subsequent line.

    Raises MatchFailedException if no such line can be found.
    """
    scope = MatchScope(self.cursor, self.c1Length)
    match = findMatchingLine(statement, self.c1Pass, scope, self.variables)
    self.moveCursor(match)

  def handleNextLine(self, statement):
    """ Single next-line statement. Test if the current line matches and move
        the cursor to the next line if it does.

    Raises MatchFailedException if the current line does not match.
    """
    scope = MatchScope(self.cursor, self.cursor + 1)
    match = findMatchingLine(statement, self.c1Pass, scope, self.variables)
    self.moveCursor(match)

  def handleEval(self, statement):
    """ Evaluates the statement in the current context.

    Raises MatchFailedException if the expression evaluates to False.
    """
    if not EvaluateLine(statement, self.variables):
      raise MatchFailedException(statement, self.cursor, self.variables)

  def handle(self, statement):
    variant = None if statement is None else statement.variant

    # First non-DAG statement always triggers execution of any preceding
    # DAG statements.
    if variant is not TestStatement.Variant.DAG:
      self.handleDagQueue(MatchScope(self.cursor, self.c1Length))

    if variant is None:
      self.handleEOF()
    elif variant is TestStatement.Variant.InOrder:
      self.handleInOrder(statement)
    elif variant is TestStatement.Variant.NextLine:
      self.handleNextLine(statement)
    elif variant is TestStatement.Variant.DAG:
      self.dagQueue.append(statement)
    elif variant is TestStatement.Variant.Not:
      self.notQueue.append(statement)
    else:
      assert variant is TestStatement.Variant.Eval
      self.handleEval(statement)

def MatchTestCase(testCase, c1Pass):
  """ Runs a test case against a C1visualizer graph dump.

  Raises MatchFailedException when a statement cannot be satisfied.
  """
  assert testCase.name == c1Pass.name

  state = ExecutionState(c1Pass)
  testStatements = testCase.statements + [ None ]
  for statement in testStatements:
    state.handle(statement)

def MatchFiles(checkerFile, c1File, targetArch, debuggableMode):
  for testCase in checkerFile.testCases:
    if testCase.testArch not in [None, targetArch]:
      continue
    if testCase.forDebuggable != debuggableMode:
      continue

    # TODO: Currently does not handle multiple occurrences of the same group
    # name, e.g. when a pass is run multiple times. It will always try to
    # match a check group against the first output group of the same name.
    c1Pass = c1File.findPass(testCase.name)
    if c1Pass is None:
      with file(c1File.fileName) as cfgFile:
        Logger.log(''.join(cfgFile), Logger.Level.Error)
      Logger.fail("Test case not found in the CFG file",
                  testCase.fileName, testCase.startLineNo, testCase.name)

    Logger.startTest(testCase.name)
    try:
      MatchTestCase(testCase, c1Pass)
      Logger.testPassed()
    except MatchFailedException as e:
      lineNo = c1Pass.startLineNo + e.lineNo
      if e.statement.variant == TestStatement.Variant.Not:
        msg = "NOT statement matched line {}"
      else:
        msg = "Statement could not be matched starting from line {}"
      msg = msg.format(lineNo)
      with file(c1File.fileName) as cfgFile:
        Logger.log(''.join(cfgFile), Logger.Level.Error)
      Logger.testFailed(msg, e.statement, e.variables)
