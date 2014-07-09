//===--- FuzzyAST.h - clang-highlight ---------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_CLANG_HIGHLIGHT_FUZZY_AST_H
#define LLVM_CLANG_TOOLS_CLANG_HIGHLIGHT_FUZZY_AST_H

#include "llvm/ADT/StringRef.h"
#include "clang/Basic/SourceManager.h"
#include "AnnotatedToken.h"
#include <memory>

using namespace clang;

namespace clang {
namespace fuzzy {

/// ASTElement: Anything inside the AST that may be referenced by an
/// AnnotatedToken must be an ASTElement.  This class is not strictly needed
/// from an AST point of view.
class ASTElement {
protected:
  ASTElement() = default;
  ~ASTElement() = default; // Not accessible
public:
  // TODO: TableGen
  enum ASTElementClass {
    NoASTElementClass = 0,
    TypeClass,
    VarInitializationClass,
    VarDeclClass,

    LineStmtClass,
    CompoundStmtClass,

    DeclStmtClass,
    DeclRefExprClass,
    LiteralConstantClass,
    UnaryOperatorClass,
    BinaryOperatorClass,
  };

  ASTElement(ASTElementClass SC) : sClass(SC) {}

  ASTElementClass getASTClass() const { return sClass; }

private:
  ASTElementClass sClass;
};

/// In contrast to the clang AST, a Stmt is a real statement, that is either a
/// CompoundStmt or a LineStmt.
class Stmt : public ASTElement {
public:
  virtual ~Stmt() = 0; // Not optimized

  Stmt(ASTElementClass SC) : ASTElement(SC) {}
};
inline Stmt::~Stmt() {}

template <typename Iter, typename Value> class IndirectRange {
public:
  IndirectRange(Iter First, Iter Last) : First(First), Last(Last) {}
  struct IndirectIter {
    IndirectIter(Iter Pos) : Pos(Pos) {}
    Iter Pos;
    friend bool operator==(IndirectIter LHS, IndirectIter RHS) {
      return LHS.Pos == RHS.Pos;
    }
    IndirectIter operator++() {
      ++Pos;
      return *this;
    }
    IndirectIter operator++(int) {
      auto Self = *this;
      ++*this;
      return Self;
    }
    Value &operator*() { return **Pos; }
  };

  IndirectIter begin() { return First; }
  IndirectIter end() { return Last; }

private:
  IndirectIter First, Last;
};

/// By a semicolon terminated statement
class LineStmt : public Stmt {
  LineStmt(AnnotatedToken *Semi)
    : Stmt(LineStmtClass),
      Semi(Semi) {}
  AnnotatedToken *Semi;
protected:
  LineStmt(ASTElementClass SC) : Stmt(SC), Semi(nullptr) {}
};

/// A {}-Block with Statements inside.
class CompoundStmt : public Stmt {
public:
  llvm::SmallVector<std::unique_ptr<Stmt>, 8> Body;
  using child_range = IndirectRange<
      llvm::SmallVector<std::unique_ptr<Stmt>, 8>::iterator, Stmt>;

  enum {
    LBR,
    RBR,
    END_EXPR
  };
  AnnotatedToken *Brackets[END_EXPR];

  CompoundStmt(AnnotatedToken *lbr, AnnotatedToken *rbr)
      : Stmt(CompoundStmtClass) {
    setBracket(LBR, lbr);
    setBracket(RBR, rbr);
  }

  void setBracket(int BracIdx, AnnotatedToken *Tok) {
    assert(0 <= BracIdx && BracIdx < END_EXPR);
    if (Tok)
      Tok->setASTReference(this);
    Brackets[BracIdx] = Tok;
  }

  void addStmt(std::unique_ptr<Stmt> Statement) {
    Body.push_back(std::move(Statement));
  }

  child_range children() { return child_range(Body.begin(), Body.end()); }

  static bool classof(const ASTElement *T) {
    return T->getASTClass() == CompoundStmtClass;
  }
};

// A Type with it's decorations.
struct Type : ASTElement {
  Type(AnnotatedToken *NameTok) : ASTElement(TypeClass), NameTok(NameTok) {}

  struct Decoration {
    enum DecorationClass {
      Pointer,
      Reference,
    };
    Decoration(DecorationClass Class, AnnotatedToken *Tok)
        : Class(Class), Tok(Tok) {}
    DecorationClass Class;
    AnnotatedToken *Tok;
  };
  llvm::SmallVector<Decoration, 1> Decorations;
  AnnotatedToken *NameTok;
};

class Expr;

// Initialization of a variable
struct VarInitialization : ASTElement {
  enum InitializationType {
    ASSIGNMENT,
    CONSTRUCTOR,
    BRACE,
  };
  VarInitialization(InitializationType InitType,
                    AnnotatedToken AssignmentOps[2],
                    std::unique_ptr<Expr> Value)
      : ASTElement(VarInitializationClass), InitType(InitType),
        Value(std::move(Value)) {
    if (InitType == ASSIGNMENT) {
      this->AssignmentOps[0] = &AssignmentOps[0];
      this->AssignmentOps[1] = nullptr;
    } else {
      this->AssignmentOps[0] = &AssignmentOps[0];
      this->AssignmentOps[1] = &AssignmentOps[1];
    }
  }
  InitializationType InitType;
  AnnotatedToken *AssignmentOps[2]; // '=' or '('+')' or '{'+'}'
  std::unique_ptr<Expr> Value;
};

// Declaration of a variable with optional initialization
struct VarDecl : ASTElement {
  VarDecl(Type VariableType, AnnotatedToken *NameTok)
      : ASTElement(VarDeclClass), VariableType(VariableType), NameTok(NameTok),
        Value() {}
  Type VariableType;
  AnnotatedToken *NameTok;
  llvm::Optional<VarInitialization> Value;
};

// Only for variable declarations (for now)
struct DeclStmt : LineStmt {
  llvm::SmallVector<VarDecl, 1> Decls;

  DeclStmt() : LineStmt(DeclStmtClass) {}

  static bool classof(const ASTElement *T) {
    return T->getASTClass() == DeclStmtClass;
  }
};

/// An expression in it's classical sense.  If an expression is used as a
/// statement, it has to be embedded into a ExprStmt (yet to be implemented).
/// Rationale is that there is otherwise no way to store the semicolon.
struct Expr : ASTElement {
  Expr(ASTElementClass SC) : ASTElement(SC) {}
  virtual ~Expr() = 0;
};
inline Expr::~Expr() {}

// Presumably a variable name inside an expression.
class DeclRefExpr : public Expr {
public:
  AnnotatedToken *Tok;
  DeclRefExpr(AnnotatedToken *Tok) : Expr(DeclRefExprClass), Tok(Tok) {
    Tok->setASTReference(this);
  }

  static bool classof(const ASTElement *T) {
    return T->getASTClass() == DeclRefExprClass;
  }
};

/// Int, char or string literals
class LiteralConstant : public Expr {
public:
  AnnotatedToken *Tok;
  LiteralConstant(AnnotatedToken *Tok) : Expr(LiteralConstantClass), Tok(Tok) {
    Tok->setASTReference(this);
  }

  static bool classof(const ASTElement *T) {
    return T->getASTClass() == LiteralConstantClass;
  }
};

/// Any unary operator, even the overloaded ones.
class UnaryOperator : public Expr {
public:
  AnnotatedToken *OperatorTok;
  std::unique_ptr<Expr> Value;

  UnaryOperator(AnnotatedToken *OperatorTok, std::unique_ptr<Expr> Value)
      : Expr(UnaryOperatorClass), OperatorTok(OperatorTok),
        Value(std::move(Value)) {
    OperatorTok->setASTReference(this);
  }

  static bool classof(const ASTElement *T) {
    return T->getASTClass() == UnaryOperatorClass;
  }
};

/// Used to store any kind of binary operators, even the overloaded ones.
class BinaryOperator : public Expr {
  enum {
    LHS,
    RHS,
    END_EXPR
  };
  std::unique_ptr<Expr> SubExprs[END_EXPR];

public:
  AnnotatedToken *OperatorTok;

  BinaryOperator(std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs,
                 AnnotatedToken *OperatorTok)
      : Expr(BinaryOperatorClass) {
    SubExprs[LHS] = std::move(lhs);
    SubExprs[RHS] = std::move(rhs);
    this->OperatorTok = OperatorTok;
    this->OperatorTok->setASTReference(this);
  }

  static bool classof(const ASTElement *T) {
    return T->getASTClass() == BinaryOperatorClass;
  }

  Expr *getLHS() { return cast<Expr>(SubExprs[LHS].get()); }
  const Expr *getLHS() const { return cast<Expr>(SubExprs[LHS].get()); }
  Expr *getRHS() { return cast<Expr>(SubExprs[RHS].get()); }
  const Expr *getRHS() const { return cast<Expr>(SubExprs[RHS].get()); }
};

std::unique_ptr<Stmt> fuzzyparse(AnnotatedToken *first, AnnotatedToken *last);

void printAST(const Stmt &Root, const SourceManager &SourceMgr);

} // end namespace fuzzy
} // end namespace clang

#endif // LLVM_CLANG_TOOLS_CLANG_HIGHLIGHT_FUZZY_AST_H
