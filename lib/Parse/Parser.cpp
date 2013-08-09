//===--- Parser.cpp - Swift Language Parser -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the Swift parser.
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

void DelayedParsingCallbacks::anchor() { }

namespace {
  /// To assist debugging parser crashes, tell us the location of the
  /// current token.
  class PrettyStackTraceParser : public llvm::PrettyStackTraceEntry {
    Parser &P;
  public:
    PrettyStackTraceParser(Parser &P) : P(P) {}
    void print(llvm::raw_ostream &out) const {
      out << "With parser at source location: ";
      printSourceLoc(out, P.Tok.getLoc(), P.Context);
      out << '\n';
    }
  };

/// A visitor that does delayed parsing of function bodies.
class ParseDelayedFunctionBodies : public ASTWalker {
  TranslationUnit *TU;
  PersistentParserState &ParserState;
  CodeCompletionCallbacksFactory *CodeCompletionFactory;

public:
  ParseDelayedFunctionBodies(TranslationUnit *TU,
                             PersistentParserState &ParserState,
                          CodeCompletionCallbacksFactory *CodeCompletionFactory)
    : TU(TU), ParserState(ParserState),
      CodeCompletionFactory(CodeCompletionFactory) {}

  virtual bool walkToDeclPre(Decl *D) {
    if (auto FD = dyn_cast<FuncDecl>(D)) {
      if (auto FE = FD->getBody()) {
        if (FE->getBodyKind() != FuncExpr::BodyKind::Unparsed)
          return false;
        parseFunctionBody(FD);
      }
      return true;
    }
    return true;
  }

private:
  void parseFunctionBody(FuncDecl *FD) {
    auto FE = FD->getBody();
    assert(FE);
    assert(FE->getBodyKind() == FuncExpr::BodyKind::Unparsed);

    int BufferID = TU->Ctx.SourceMgr->FindBufferContainingLoc(FD->getLoc().Value);
    Parser TheParser(BufferID, TU, nullptr, &ParserState);

    std::unique_ptr<CodeCompletionCallbacks> CodeCompletion;
    if (CodeCompletionFactory) {
      CodeCompletion.reset(
          CodeCompletionFactory->createCodeCompletionCallbacks(TheParser));
      TheParser.setCodeCompletionCallbacks(CodeCompletion.get());
    }
    TheParser.parseDeclFuncBodyDelayed(FD);
    CodeCompletion->doneParsing();
  }
};

void parseDelayedTopLevelDecl(
    TranslationUnit *TU,
    PersistentParserState &ParserState,
    CodeCompletionCallbacksFactory *CodeCompletionFactory) {
  assert(CodeCompletionFactory &&
         "delayed parsing of decls only makes sense for code completion");

  if (!ParserState.hasDelayedDecl())
    return;

  int BufferID = TU->Ctx.SourceMgr
      ->FindBufferContainingLoc(ParserState.getDelayedDeclLoc().Value);
  Parser TheParser(BufferID, TU, nullptr, &ParserState);

  std::unique_ptr<CodeCompletionCallbacks> CodeCompletion(
      CodeCompletionFactory->createCodeCompletionCallbacks(TheParser));
  TheParser.setCodeCompletionCallbacks(CodeCompletion.get());
  TheParser.parseTopLevelCodeDeclDelayed();
  CodeCompletion->doneParsing();
}
} // unnamed namespace

/// parseIntoTranslationUnit - Entrypoint for the parser.
bool swift::parseIntoTranslationUnit(TranslationUnit *TU,
                                     unsigned BufferID,
                                     bool *Done,
                                     SILParserState *SIL,
                                     PersistentParserState *PersistentState,
                                     DelayedParsingCallbacks *DelayedParseCB) {
  Parser P(BufferID, TU, SIL, PersistentState);
  PrettyStackTraceParser StackTrace(P);

  if (DelayedParseCB)
    P.setDelayedParsingCallbacks(DelayedParseCB);

  bool FoundSideEffects = P.parseTranslationUnit(TU);
  *Done = P.Tok.is(tok::eof);

  return FoundSideEffects;
}

void swift::performDelayedParsing(
    TranslationUnit *TU, PersistentParserState &PersistentState,
    CodeCompletionCallbacksFactory *CodeCompletionFactory) {
  ParseDelayedFunctionBodies Walker(TU, PersistentState,
                                    CodeCompletionFactory);
  for (Decl *D : TU->Decls) {
    D->walk(Walker);
  }

  if (CodeCompletionFactory)
    parseDelayedTopLevelDecl(TU, PersistentState, CodeCompletionFactory);
}

/// \brief Tokenizes a string literal, taking into account string interpolation.
void getStringPartTokens(const Token &Tok, SourceManager &SM, int BufID,
                         std::vector<Token> &Toks) {
  assert(Tok.is(tok::string_literal));
  SmallVector<Lexer::StringSegment, 4> Segments;
  Lexer::getStringLiteralSegments(Tok, Segments, /*Diags=*/0);
  for (unsigned i = 0, e = Segments.size(); i != e; ++i) {
    Lexer::StringSegment &Seg = Segments[i];
    bool isFirst = i == 0;
    bool isLast = i == e-1;
    if (Seg.Kind == Lexer::StringSegment::Literal) {
      SourceLoc Loc = Seg.Loc;
      unsigned Len = Seg.Length;
      if (isFirst) {
        // Include the quote.
        Loc = Loc.getAdvancedLoc(-1);
        ++Len;
      }
      if (isLast) {
        // Include the quote.
        ++Len;
      }
      StringRef Text(Loc.Value.getPointer(), Len);
      Token NewTok;
      NewTok.setToken(tok::string_literal, Text);
      Toks.push_back(NewTok);

    } else {
      unsigned Offset = SM.getLocOffsetInBuffer(Seg.Loc, BufID);
      unsigned EndOffset = Offset + Seg.Length;
      std::vector<Token> NewTokens = swift::tokenize(SM, BufID, Offset,
                                                     EndOffset,
                                                     /*KeepComments=*/true);
      Toks.insert(Toks.end(), NewTokens.begin(), NewTokens.end());
    }
  }
}

std::vector<Token> swift::tokenize(SourceManager &SM, unsigned BufferID,
                                   unsigned Offset, unsigned EndOffset,
                                   bool KeepComments,
                                   bool TokenizeInterpolatedString) {
  if (Offset == 0 && EndOffset == 0)
    EndOffset = SM->getMemoryBuffer(BufferID)->getBufferSize();

  Lexer L(SM, BufferID, /*Diags=*/nullptr, /*InSILMode=*/false, KeepComments,
          Offset, EndOffset);
  std::vector<Token> Tokens;
  do {
    Tokens.emplace_back();
    L.lex(Tokens.back());
    if (Tokens.back().is(tok::string_literal) && TokenizeInterpolatedString) {
      Token StrTok = Tokens.back();
      Tokens.pop_back();
      getStringPartTokens(StrTok, SM, BufferID, Tokens);
    }
  } while (Tokens.back().isNot(tok::eof));
  Tokens.pop_back(); // Remove EOF.
  return Tokens;
}

//===----------------------------------------------------------------------===//
// Setup and Helper Methods
//===----------------------------------------------------------------------===//

Parser::Parser(unsigned BufferID, TranslationUnit *TU, SILParserState *SIL,
               PersistentParserState *PersistentState)
  : SourceMgr(TU->getASTContext().SourceMgr),
    Diags(TU->getASTContext().Diags),
    TU(TU),
    L(new Lexer(TU->getASTContext().SourceMgr, BufferID,
                &TU->getASTContext().Diags,
                /*InSILMode=*/SIL != nullptr, /*KeepComments=*/false)),
    SIL(SIL),
    Component(TU->getComponent()),
    Context(TU->getASTContext()) {

  State = PersistentState;
  if (!State) {
    OwnedState.reset(new PersistentParserState());
    State = OwnedState.get();
  }

  // Set the token to a sentinel so that we know the lexer isn't primed yet.
  // This cannot be tok::unknown, since that is a token the lexer could produce.
  Tok.setKind(tok::NUM_TOKENS);

  auto ParserPos = State->takeParserPosition();
  if (ParserPos.isValid() &&
      SourceMgr->FindBufferContainingLoc(ParserPos.Loc.Value) == int(BufferID)) {
    auto BeginParserPosition = getParserPosition(ParserPos);
    restoreParserPosition(BeginParserPosition);
  }
}

Parser::~Parser() {
  delete L;
}

/// peekToken - Return the next token that will be installed by consumeToken.
const Token &Parser::peekToken() {
  return L->peekNextToken();
}

SourceLoc Parser::consumeToken() {
  SourceLoc Loc = Tok.getLoc();
  assert(Tok.isNot(tok::eof) && "Lexing past eof!");
  L->lex(Tok);
  PreviousLoc = Loc;
  return Loc;
}

SourceLoc Parser::getEndOfPreviousLoc() {
  return Lexer::getLocForEndOfToken(SourceMgr, PreviousLoc);
}

SourceLoc Parser::consumeStartingLess() {
  assert(startsWithLess(Tok) && "Token does not start with '<'");
  
  if (Tok.getLength() == 1)
    return consumeToken();
  
  // Skip the starting '<' in the existing token.
  SourceLoc Loc = Tok.getLoc();
  Tok = L->getTokenAt(Loc.getAdvancedLoc(1));
  return Loc;
}

SourceLoc Parser::consumeStartingGreater() {
  assert(startsWithGreater(Tok) && "Token does not start with '>'");
  
  if (Tok.getLength() == 1)
    return consumeToken();
  
  // Skip the starting '>' in the existing token.
  SourceLoc Loc = Tok.getLoc();
  Tok = L->getTokenAt(Loc.getAdvancedLoc(1));
  return Loc;
}

void Parser::skipSingle(bool StopAtCodeComplete) {
  switch (Tok.getKind()) {
  case tok::l_paren:
    consumeToken();
    skipUntil(tok::r_paren, StopAtCodeComplete);
    consumeIf(tok::r_paren);
    break;
  case tok::l_brace:
    consumeToken();
    skipUntil(tok::r_brace, StopAtCodeComplete);
    consumeIf(tok::r_brace);
    break;
  case tok::l_square:
    consumeToken();
    skipUntil(tok::r_square, StopAtCodeComplete);
    consumeIf(tok::r_square);
    break;
  case tok::code_complete:
    if (!StopAtCodeComplete)
      consumeToken();
    break;
  default:
    consumeToken();
    break;
  }
}

void Parser::skipUntil(tok T1, tok T2, bool StopAtCodeComplete) {
  // tok::unknown is a sentinel that means "don't skip".
  if (T1 == tok::unknown && T2 == tok::unknown) return;
  
  while (Tok.isNot(tok::eof) && Tok.isNot(T1) && Tok.isNot(T2) &&
         (!StopAtCodeComplete || Tok.isNot(tok::code_complete)))
    skipSingle(StopAtCodeComplete);
}

void Parser::skipUntilAnyOperator() {
  while (Tok.isNot(tok::eof) && Tok.isNotAnyOperator())
    skipSingle();
}

void Parser::skipUntilDeclRBrace() {
  while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_brace) &&
         !isStartOfDecl(Tok, peekToken()))
    skipSingle();
}

void Parser::skipUntilDeclStmtRBrace(bool StopAtCodeComplete) {
  while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_brace) &&
         !isStartOfStmt(Tok) &&
         !isStartOfDecl(Tok, peekToken()) &&
         (!StopAtCodeComplete || Tok.isNot(tok::code_complete))) {
    skipSingle(StopAtCodeComplete);
  }
}


//===----------------------------------------------------------------------===//
// Primitive Parsing
//===----------------------------------------------------------------------===//

/// parseIdentifier - Consume an identifier (but not an operator) if
/// present and return its name in Result.  Otherwise, emit an error and
/// return true.
bool Parser::parseIdentifier(Identifier &Result, SourceLoc &Loc,
                             const Diagnostic &D) {
  switch (Tok.getKind()) {
#define IDENTIFIER_KEYWORD(kw) case tok::kw_##kw:
#include "swift/Parse/Tokens.def"
  case tok::identifier:
    Result = Context.getIdentifier(Tok.getText());
    Loc = Tok.getLoc();
    consumeToken();
    return false;
  default:
    Diags.diagnose(Tok.getLoc(), D);
    return true;
  }
}

/// parseAnyIdentifier - Consume an identifier or operator if present and return
/// its name in Result.  Otherwise, emit an error and return true.
bool Parser::parseAnyIdentifier(Identifier &Result, SourceLoc &Loc,
                                const Diagnostic &D) {
  if (Tok.is(tok::identifier) || Tok.isAnyOperator()) {
    Result = Context.getIdentifier(Tok.getText());
    Loc = Tok.getLoc();
    consumeToken();
    return false;
  }
  
  Diags.diagnose(Tok.getLoc(), D);
  return true;
}

/// parseToken - The parser expects that 'K' is next in the input.  If so, it is
/// consumed and false is returned.
///
/// If the input is malformed, this emits the specified error diagnostic.
bool Parser::parseToken(tok K, SourceLoc &TokLoc, const Diagnostic &D) {
  if (Tok.is(K)) {
    TokLoc = consumeToken(K);
    return false;
  }
  
  diagnose(Tok.getLoc(), D);
  return true;
}

/// parseMatchingToken - Parse the specified expected token and return its
/// location on success.  On failure, emit the specified error diagnostic, and a
/// note at the specified note location.
bool Parser::parseMatchingToken(tok K, SourceLoc &TokLoc, Diag<> ErrorDiag,
                                SourceLoc OtherLoc) {
  Diag<> OtherNote;
  switch (K) {
  case tok::r_paren:  OtherNote = diag::opening_paren;    break;
  case tok::r_square: OtherNote = diag::opening_bracket;  break;
  case tok::r_brace:  OtherNote = diag::opening_brace;    break;
  default:            assert(!"unknown matching token!"); break;
  }
  if (parseToken(K, TokLoc, ErrorDiag)) {
    diagnose(OtherLoc, OtherNote);
    return true;
  }

  return false;
}

/// parseList - Parse the list of statements, expressions, or declarations.
bool Parser::parseList(tok RightK, SourceLoc LeftLoc, SourceLoc &RightLoc,
                       tok SeparatorK, bool OptionalSep, Diag<> ErrorDiag,
                       std::function<bool()> callback) {
  assert(SeparatorK == tok::comma || SeparatorK == tok::semi);

  if (Tok.is(RightK)) {
    RightLoc = consumeToken(RightK);
    return false;
  }

  bool Invalid = false;
  while (true) {
    while (Tok.is(SeparatorK)) {
      diagnose(Tok, diag::unexpected_separator,
               SeparatorK == tok::comma ? "," : ";")
        .fixItRemove(SourceRange(Tok.getLoc()));
      consumeToken();
    }
    SourceLoc StartLoc = Tok.getLoc();
    Invalid |= callback();
    if (Tok.is(RightK))
      break;
    // If the lexer stopped with an EOF token whose spelling is ")", then this
    // is actually the tuple that is a string literal interpolation context.
    // Just accept the ")" and build the tuple as we usually do.
    if (Tok.is(tok::eof) && Tok.getText() == ")") {
      RightLoc = Tok.getLoc();
      return Invalid;
    }
    if (consumeIf(SeparatorK))
      continue;
    if (!OptionalSep) {
      SourceLoc InsertLoc = Lexer::getLocForEndOfToken(SourceMgr, PreviousLoc);
      StringRef Separator = (SeparatorK == tok::comma ? "," : ";");
      diagnose(Tok, diag::expected_separator, Separator)
        .fixItInsert(InsertLoc, Separator);
      Invalid = true;
    }
    // If we haven't made progress, skip ahead
    if (Tok.getLoc() == StartLoc) {
      skipUntil(RightK, SeparatorK);
      if (Tok.is(RightK))
        break;
      if (Tok.is(tok::eof) || Tok.is(tok::code_complete))
        return true;
      consumeIf(SeparatorK);
    }
  }

  Invalid |= parseMatchingToken(RightK, RightLoc, ErrorDiag, LeftLoc);
  return Invalid;
}

/// diagnoseRedefinition - Diagnose a redefinition error, with a note
/// referring back to the original definition.

void Parser::diagnoseRedefinition(ValueDecl *Prev, ValueDecl *New) {
  assert(New != Prev && "Cannot conflict with self");
  diagnose(New->getLoc(), diag::decl_redefinition, New->isDefinition());
  diagnose(Prev->getLoc(), diag::previous_decldef, Prev->isDefinition(),
             Prev->getName());
}
