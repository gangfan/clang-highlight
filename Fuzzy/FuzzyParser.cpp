//===--- FuzzyParser.cpp - clang-highlight ----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "llvm/Support/Debug.h"
#include "llvm/ADT/STLExtras.h"
#include "FuzzyAST.h"
#include "clang/Basic/OperatorPrecedence.h"

using namespace clang;

namespace clang {
namespace fuzzy {

namespace {
struct TokenFilter {
  AnnotatedToken *First, *Last;
  TokenFilter(AnnotatedToken *First, AnnotatedToken *Last)
      : First(First), Last(Last) {}

  AnnotatedToken *next() {
    auto Ret = First++;
    while (First != Last && (First->Tok.getKind() == tok::unknown ||
                             First->Tok.getKind() == tok::comment))
      ++First;
    if (First == Last || First->Tok.getKind() == tok::eof)
      First = Last = 0;
    assert(Ret->Tok.getKind() != tok::raw_identifier);
    return Ret;
  }

  class TokenFilterState {
    friend class TokenFilter;
    TokenFilterState(AnnotatedToken *First, AnnotatedToken *Last)
        : First(First), Last(Last) {}
    AnnotatedToken *First, *Last;
  };

  TokenFilterState mark() const { return TokenFilterState(First, Last); }
  void rewind(TokenFilterState State) {
    First = State.First;
    Last = State.Last;
  }

  class TokenFilterGuard {
    friend class TokenFilter;
    TokenFilterGuard(TokenFilter *TF, TokenFilterState State)
        : TF(TF), State(State) {}

  public:
    ~TokenFilterGuard() {
      if (TF)
        TF->rewind(State);
    }
    void dismiss() { TF = nullptr; }
    TokenFilter *TF;
    TokenFilterState State;
  };
  TokenFilterGuard guard() { return TokenFilterGuard(this, mark()); }

  AnnotatedToken *peek() { return First; }
};
} // end anonymous namespace

static bool checkKind(TokenFilter &TF, tok::TokenKind Kind) {
  return TF.peek() && TF.peek()->Tok.getKind() == Kind;
}

static int PrecedenceUnaryOperator = prec::PointerToMember + 1;
static int PrecedenceArrowAndPeriod = prec::PointerToMember + 2;

static std::unique_ptr<Expr> parseExpression(TokenFilter &TF,
                                             int Precedence = 1,
                                             bool StopAtGreater = false);

static std::unique_ptr<Type> parseType(TokenFilter &TF,
                                       bool WithDecorations = true);

static std::unique_ptr<Expr> parseUnaryOperator(TokenFilter &TF) {
  assert(TF.peek() && "can't parse empty expression");

  if (checkKind(TF, tok::plus) || checkKind(TF, tok::minus) ||
      checkKind(TF, tok::exclaim) || checkKind(TF, tok::tilde) ||
      checkKind(TF, tok::star) || checkKind(TF, tok::amp) ||
      checkKind(TF, tok::plusplus) || checkKind(TF, tok::minusminus)) {
    AnnotatedToken *Op = TF.next();
    return llvm::make_unique<UnaryOperator>(Op, parseUnaryOperator(TF));
  }

  return parseExpression(TF, PrecedenceArrowAndPeriod);
}

static std::unique_ptr<Expr>
parseCallExpr(TokenFilter &TF, std::unique_ptr<DeclRefExpr> FunctionName) {
  assert(checkKind(TF, tok::l_paren));
  auto Func = llvm::make_unique<CallExpr>(std::move(FunctionName));
  Func->setLeftParen(TF.next());
  while (!checkKind(TF, tok::r_paren)) {
    Func->Args.push_back(parseExpression(TF, prec::Comma + 1));
    if (TF.peek()->Tok.getKind() == tok::comma)
      Func->appendComma(TF.next());
    else
      break;
  }
  if (checkKind(TF, tok::r_paren)) {
    Func->setRightParen(TF.next());
    return std::move(Func);
  }
  return {};
}

static bool isLiteralOrConstant(tok::TokenKind K) {
  if (isLiteral(K))
    return true;

  switch (K) {
  case tok::kw_true:
  case tok::kw_false:
  case tok::kw___objc_yes:
  case tok::kw___objc_no:
  case tok::kw_nullptr:
    return true;
  default:
    return false;
  }
}

template <typename QualOwner>
static bool parseQualifiedID(TokenFilter &TF, QualOwner &Qual) {
  auto Guard = TF.guard();

  bool GlobalNamespaceColon = true;
  do {
    if (checkKind(TF, tok::coloncolon))
      Qual.addNameQualifier(TF.next());
    else if (!GlobalNamespaceColon)
      return {};
    GlobalNamespaceColon = false;
    if (!checkKind(TF, tok::identifier))
      return {};
    Qual.addNameQualifier(TF.next());
  } while (checkKind(TF, tok::coloncolon));

  if (checkKind(TF, tok::less)) {
    Qual.makeTemplateArgs();
    bool isFirst = true;
    do {
      Qual.addTemplateSeparator(TF.next());

      if (isFirst && checkKind(TF, tok::greater))
        break;
      isFirst = false;

      if (auto Arg = parseType(TF))
        Qual.addTemplateArgument(std::move(Arg));
      else if (auto E =
                   parseExpression(TF, prec::Comma + 1, /*StopAtGreater=*/true))
        Qual.addTemplateArgument(std::move(E));
      else
        return false;
    } while (checkKind(TF, tok::comma));
    if (!checkKind(TF, tok::greater))
      return false;
    Qual.addTemplateSeparator(TF.next());
  }

  Guard.dismiss();
  return true;
}

static std::unique_ptr<Expr> parseExpression(TokenFilter &TF, int Precedence,
                                             bool StopAtGreater) {
  assert(TF.peek() && "can't parse empty expression");

  if (Precedence == PrecedenceUnaryOperator)
    return parseUnaryOperator(TF);

  if (Precedence > PrecedenceArrowAndPeriod) {
    if (isLiteralOrConstant(TF.peek()->Tok.getKind()))
      return llvm::make_unique<LiteralConstant>(TF.next());

    if (checkKind(TF, tok::identifier) || checkKind(TF, tok::coloncolon)) {
      auto DR = llvm::make_unique<DeclRefExpr>();
      if (!parseQualifiedID(TF, *DR))
        return {};
      if (checkKind(TF, tok::l_paren))
        return parseCallExpr(TF, std::move(DR));
      return std::move(DR);
    }

    return {};
  }
  auto LeftExpr = parseExpression(TF, Precedence + 1, StopAtGreater);

  while (TF.peek()) {
    if (StopAtGreater && checkKind(TF, tok::greater))
      break;

    int CurrentPrecedence =
        getBinOpPrecedence(TF.peek()->Tok.getKind(), true, true);
    if (checkKind(TF, tok::period) || checkKind(TF, tok::arrow))
      CurrentPrecedence = PrecedenceArrowAndPeriod;
    if (CurrentPrecedence == 0)
      return LeftExpr;

    assert(CurrentPrecedence <= Precedence);
    if (CurrentPrecedence < Precedence)
      break;
    assert(CurrentPrecedence == Precedence);

    AnnotatedToken *OperatorTok = TF.next();

    auto RightExpr = parseExpression(TF, Precedence + 1, StopAtGreater);
    if (!RightExpr)
      return {};
    LeftExpr = llvm::make_unique<BinaryOperator>(
        std::move(LeftExpr), std::move(RightExpr), OperatorTok);
  }

  return LeftExpr;
}

static std::unique_ptr<Stmt> parseReturnStmt(TokenFilter &TF) {
  auto Guard = TF.guard();
  if (!checkKind(TF, tok::kw_return))
    return {};
  auto *Return = TF.next();
  std::unique_ptr<Expr> Body;
  if (!checkKind(TF, tok::semi)) {
    Body = parseExpression(TF);
    if (!Body || !checkKind(TF, tok::semi))
      return {};
  }
  assert(checkKind(TF, tok::semi));
  auto *Semi = TF.next();
  Guard.dismiss();
  return llvm::make_unique<ReturnStmt>(Return, std::move(Body), Semi);
}

static void parseTypeDecorations(TokenFilter &TF, Type &T) {
  // TODO: add const and volatile
  while (checkKind(TF, tok::star) || checkKind(TF, tok::amp) ||
         checkKind(TF, tok::ampamp))
    T.Decorations.push_back(Type::Decoration(checkKind(TF, tok::star)
                                                 ? Type::Decoration::Pointer
                                                 : Type::Decoration::Reference,
                                             TF.next()));
  for (auto &Dec : T.Decorations)
    Dec.fix();
}

static bool isBuiltinType(tok::TokenKind K) {
  switch (K) {
  case tok::kw_short:
  case tok::kw_long:
  case tok::kw___int64:
  case tok::kw___int128:
  case tok::kw_signed:
  case tok::kw_unsigned:
  case tok::kw__Complex:
  case tok::kw__Imaginary:
  case tok::kw_void:
  case tok::kw_char:
  case tok::kw_wchar_t:
  case tok::kw_char16_t:
  case tok::kw_char32_t:
  case tok::kw_int:
  case tok::kw_half:
  case tok::kw_float:
  case tok::kw_double:
  case tok::kw_bool:
  case tok::kw__Bool:
  case tok::kw__Decimal32:
  case tok::kw__Decimal64:
  case tok::kw__Decimal128:
  case tok::kw___vector:
    return true;
  default:
    return false;
  }
}

static bool isCVQualifier(tok::TokenKind K) {
  switch (K) {
  case tok::kw_const:
  case tok::kw_volatile:
  case tok::kw_register:
    return true;
  default:
    return false;
  }
}

static std::unique_ptr<Type> parseType(TokenFilter &TF, bool WithDecorations) {
  auto Guard = TF.guard();
  std::unique_ptr<Type> T = llvm::make_unique<Type>();

  while (TF.peek() && isCVQualifier(TF.peek()->Tok.getKind()))
    T->addNameQualifier(TF.next());

  if (checkKind(TF, tok::kw_auto)) {
    T->addNameQualifier(TF.next());
  } else if (TF.peek() && isBuiltinType(TF.peek()->Tok.getKind())) {
    while (TF.peek() && isBuiltinType(TF.peek()->Tok.getKind()))
      T->addNameQualifier(TF.next());
  } else if (!parseQualifiedID(TF, *T)) {
    return {};
  }
  while (TF.peek() && isCVQualifier(TF.peek()->Tok.getKind()))
    T->addNameQualifier(TF.next());

  if (WithDecorations)
    parseTypeDecorations(TF, *T);

  Guard.dismiss();
  return T;
}

static std::unique_ptr<VarDecl>
parseVarDecl(TokenFilter &TF, Type *TypeName = 0, bool NameOptional = false) {
  auto Guard = TF.guard();
  auto VD = llvm::make_unique<VarDecl>();
  VarDecl &D = *VD;

  std::unique_ptr<Type> TypeName2;
  if (!TypeName) {
    TypeName2 = parseType(TF);
    if (!TypeName2)
      return {};
    TypeName = TypeName2.get();
  }

  D.VariableType = TypeName->cloneWithoutDecorations();
  parseTypeDecorations(TF, *D.VariableType);

  if (checkKind(TF, tok::identifier)) {
    D.setName(TF.next());
  } else if (!NameOptional) {
    return {};
  }

  if (checkKind(TF, tok::equal)) {
    auto *EqualTok = TF.next();
    if (auto Value = parseExpression(TF, prec::Comma + 1)) {
      D.Value = VarInitialization();
      D.Value->setAssignmentOps(VarInitialization::ASSIGNMENT, EqualTok);
      D.Value->Value = std::move(Value);
    } else {
      return {};
    }
  } else {
    // TODO: var(init) and var{init} not yet implemented
  }
  Guard.dismiss();
  return VD;
}

static std::unique_ptr<Stmt> parseDeclStmt(TokenFilter &TF) {
  auto Guard = TF.guard();

  auto TypeName = parseType(TF, /*WithDecorations=*/false);
  if (!TypeName)
    return {};
  auto Declaration = llvm::make_unique<DeclStmt>();

  while (TF.peek()) {
    if (checkKind(TF, tok::semi)) {
      Declaration->setSemi(TF.next());
      Guard.dismiss();
      return std::move(Declaration);
    }
    if (auto D = parseVarDecl(TF, TypeName.get()))
      Declaration->Decls.push_back(std::move(D));
    else
      return {};
    if (checkKind(TF, tok::comma))
      Declaration->appendComma(TF.next());
    else if (!checkKind(TF, tok::semi))
      return {};
  }

  return {};
}

static bool parseDestructor(TokenFilter &TF, FunctionDecl &F) {
  if (!checkKind(TF, tok::tilde))
    return false;
  F.setName(TF.next());
  return static_cast<bool>(F.ReturnType = parseType(TF));
}

static std::unique_ptr<FunctionDecl>
parseFunctionDecl(TokenFilter &TF, bool NameOptional = false) {
  auto Guard = TF.guard();
  auto F = llvm::make_unique<FunctionDecl>();
  if (checkKind(TF, tok::kw_static))
    F->setStatic(TF.next());
  if (checkKind(TF, tok::kw_virtual))
    F->setStatic(TF.next());

  bool InDestructor = false;

  if (auto T = parseType(TF)) {
    F->ReturnType = std::move(T);
  } else if (NameOptional && parseDestructor(TF, *F)) {
    InDestructor = true;
  } else {
    return {};
  }

  if (!InDestructor) {
    if (!checkKind(TF, tok::identifier)) {
      if (!NameOptional)
        return {};
    } else {
      F->setName(TF.next());
    }
  }

  if (!checkKind(TF, tok::l_paren))
    return {};

  F->setLeftParen(TF.next());
  while (!checkKind(TF, tok::r_paren)) {
    F->Params.push_back(parseVarDecl(TF, 0, true));
    if (!F->Params.back())
      return {};
    if (checkKind(TF, tok::comma))
      F->appendComma(TF.next());
    else
      break;
  }
  if (!checkKind(TF, tok::r_paren))
    return {};

  F->setRightParen(TF.next());

  // if (InConstructor && checkKind(TF, tok::colon)) {
  // TODO: Don't skip initializer list and [[x]] and const
  while (TF.peek() && !checkKind(TF, tok::l_brace) && !checkKind(TF, tok::semi))
    TF.next();
  //}

  if (checkKind(TF, tok::semi))
    F->setSemi(TF.next());
  Guard.dismiss();
  return std::move(F);
}

static std::unique_ptr<Stmt> skipUnparsable(TokenFilter &TF) {
  assert(TF.peek());
  auto UB = llvm::make_unique<UnparsableBlock>();
  while (TF.peek()) {
    auto Kind = TF.peek()->Tok.getKind();
    UB->push_back(TF.next());
    if (Kind == tok::semi || Kind == tok::r_brace || Kind == tok::l_brace)
      break;
  }
  return std::move(UB);
}

static std::unique_ptr<Stmt> parseLabelStmt(TokenFilter &TF) {
  auto Guard = TF.guard();
  if (!(checkKind(TF, tok::identifier) || checkKind(TF, tok::kw_private) ||
        checkKind(TF, tok::kw_protected) || checkKind(TF, tok::kw_public)))
    return {};
  auto *LabelName = TF.next();
  if (!checkKind(TF, tok::colon))
    return {};
  Guard.dismiss();
  return llvm::make_unique<LabelStmt>(LabelName, TF.next());
}

static std::unique_ptr<Stmt> parseAny(TokenFilter &TF,
                                      bool SkipUnparsable = true,
                                      bool NameOptional = false);

static bool parseScope(TokenFilter &TF, Scope &Sc) {
  if (checkKind(TF, tok::r_brace))
    return true;
  while (auto St = parseAny(TF, true, true)) {
    Sc.addStmt(std::move(St));
    if (!TF.peek())
      return false;
    if (checkKind(TF, tok::r_brace))
      return true;
  }
  return checkKind(TF, tok::r_brace);
}

static std::unique_ptr<CompoundStmt> parseCompoundStmt(TokenFilter &TF) {
  if (!checkKind(TF, tok::l_brace))
    return {};
  auto C = llvm::make_unique<CompoundStmt>();
  C->setLeftParen(TF.next());
  parseScope(TF, *C);
  if (checkKind(TF, tok::r_brace))
    C->setRightParen(TF.next());
  // else: just pass
  return C;
}

static bool parseClassScope(TokenFilter &TF, ClassDecl &C) {
  if (!checkKind(TF, tok::l_brace))
    return false;

  C.setLeftParen(TF.next());
  if (!parseScope(TF, C))
    return false;

  if (checkKind(TF, tok::r_brace))
    C.setRightParen(TF.next());

  if (checkKind(TF, tok::semi))
    C.setSemi(TF.next());
  // else: just pass

  return true;
}
static std::unique_ptr<ClassDecl> parseClassDecl(TokenFilter &TF) {
  auto Guard = TF.guard();

  if (!(checkKind(TF, tok::kw_class) || checkKind(TF, tok::kw_struct) ||
        checkKind(TF, tok::kw_union) || checkKind(TF, tok::kw_enum)))
    return {};
  auto C = llvm::make_unique<ClassDecl>();
  C->setClass(TF.next());

  if (!(C->Name = parseType(TF)))
    return {};

  if (checkKind(TF, tok::colon)) {
    C->setColon(TF.next());
    bool Skip = true;
    for (;;) {
      AnnotatedToken *Accessibility = nullptr;
      if (checkKind(TF, tok::kw_private) || checkKind(TF, tok::kw_protected) ||
          checkKind(TF, tok::kw_public))
        Accessibility = TF.next();
      auto T = parseType(TF, false);
      if (!T)
        break;
      if (checkKind(TF, tok::l_brace)) {
        C->addBaseClass(Accessibility, std::move(T), nullptr);
        Skip = false;
        break;
      }
      if (!checkKind(TF, tok::comma))
        break;
      C->addBaseClass(Accessibility, std::move(T), TF.next());
    }
    if (Skip) {
      while (!checkKind(TF, tok::l_brace))
        TF.next();
    }
  }

  if (checkKind(TF, tok::semi))
    C->setSemi(TF.next());
  else
    parseClassScope(TF, *C);

  Guard.dismiss();
  return C;
}

static std::unique_ptr<Stmt> parseAny(TokenFilter &TF, bool SkipUnparsable,
                                      bool NameOptional) {
  if (auto S = parseReturnStmt(TF))
    return S;
  if (auto S = parseDeclStmt(TF))
    return S;
  if (auto S = parseLabelStmt(TF))
    return S;
  if (auto S = parseFunctionDecl(TF, NameOptional)) {
    if (checkKind(TF, tok::semi))
      S->setSemi(TF.next());
    else if (checkKind(TF, tok::l_brace)) {
      S->Body = parseCompoundStmt(TF);
    }
    return std::move(S);
  }

  if (auto S = parseClassDecl(TF)) {
    if (checkKind(TF, tok::semi))
      S->setSemi(TF.next());
    else if (checkKind(TF, tok::l_brace)) {
      parseClassScope(TF, *S);
    }
    return std::move(S);
  }
  {
    auto Guard = TF.guard();
    if (auto E = parseExpression(TF)) {
      if (checkKind(TF, tok::semi)) {
        Guard.dismiss();
        return llvm::make_unique<ExprLineStmt>(std::move(E), TF.next());
      }
    }
  }
  return SkipUnparsable ? skipUnparsable(TF) : std::unique_ptr<Stmt>();
}

TranslationUnit fuzzyparse(AnnotatedToken *first, AnnotatedToken *last) {
  TranslationUnit TU;
  TokenFilter TF(first, last);
  while (TF.peek())
    TU.addStmt(parseAny(TF));
  return TU;
}

} // end namespace fuzzy
} // end namespace clang