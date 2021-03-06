// Copyright 2011 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef V8_PREPARSER_H
#define V8_PREPARSER_H

namespace v8 {
namespace preparser {

// Preparsing checks a JavaScript program and emits preparse-data that helps
// a later parsing to be faster.
// See preparse-data-format.h for the data format.

// The PreParser checks that the syntax follows the grammar for JavaScript,
// and collects some information about the program along the way.
// The grammar check is only performed in order to understand the program
// sufficiently to deduce some information about it, that can be used
// to speed up later parsing. Finding errors is not the goal of pre-parsing,
// rather it is to speed up properly written and correct programs.
// That means that contextual checks (like a label being declared where
// it is used) are generally omitted.

namespace i = v8::internal;

class PreParser {
 public:
  enum PreParseResult {
    kPreParseStackOverflow,
    kPreParseSuccess
  };

  ~PreParser() { }

  // Pre-parse the program from the character stream; returns true on
  // success (even if parsing failed, the pre-parse data successfully
  // captured the syntax error), and false if a stack-overflow happened
  // during parsing.
  static PreParseResult PreParseProgram(i::JavaScriptScanner* scanner,
                                        i::ParserRecorder* log,
                                        bool allow_lazy,
                                        uintptr_t stack_limit) {
    return PreParser(scanner, log, stack_limit, allow_lazy).PreParse();
  }

 private:
  // These types form an algebra over syntactic categories that is just
  // rich enough to let us recognize and propagate the constructs that
  // are either being counted in the preparser data, or is important
  // to throw the correct syntax error exceptions.

  enum ScopeType {
    kTopLevelScope,
    kFunctionScope
  };

  class Expression;

  class Identifier {
   public:
    static Identifier Default() {
      return Identifier(kUnknownIdentifier);
    }
    static Identifier Eval()  {
      return Identifier(kEvalIdentifier);
    }
    static Identifier Arguments()  {
      return Identifier(kArgumentsIdentifier);
    }
    static Identifier FutureReserved()  {
      return Identifier(kFutureReservedIdentifier);
    }
    static Identifier FutureStrictReserved()  {
      return Identifier(kFutureStrictReservedIdentifier);
    }
    bool IsEval() { return type_ == kEvalIdentifier; }
    bool IsArguments() { return type_ == kArgumentsIdentifier; }
    bool IsEvalOrArguments() { return type_ >= kEvalIdentifier; }
    bool IsFutureReserved() { return type_ == kFutureReservedIdentifier; }
    bool IsFutureStrictReserved() {
      return type_ == kFutureStrictReservedIdentifier;
    }
    bool IsValidStrictVariable() { return type_ == kUnknownIdentifier; }

   private:
    enum Type {
      kUnknownIdentifier,
      kFutureReservedIdentifier,
      kFutureStrictReservedIdentifier,
      kEvalIdentifier,
      kArgumentsIdentifier
    };
    explicit Identifier(Type type) : type_(type) { }
    Type type_;

    friend class Expression;
  };

  // Bits 0 and 1 are used to identify the type of expression:
  // If bit 0 is set, it's an identifier.
  // if bit 1 is set, it's a string literal.
  // If neither is set, it's no particular type, and both set isn't
  // use yet.
  // Bit 2 is used to mark the expression as being parenthesized,
  // so "(foo)" isn't recognized as a pure identifier (and possible label).
  class Expression {
   public:
    static Expression Default() {
      return Expression(kUnknownExpression);
    }

    static Expression FromIdentifier(Identifier id) {
      return Expression(kIdentifierFlag | (id.type_ << kIdentifierShift));
    }

    static Expression StringLiteral() {
      return Expression(kUnknownStringLiteral);
    }

    static Expression UseStrictStringLiteral() {
      return Expression(kUseStrictString);
    }

    static Expression This() {
      return Expression(kThisExpression);
    }

    static Expression ThisProperty() {
      return Expression(kThisPropertyExpression);
    }

    static Expression StrictFunction() {
      return Expression(kStrictFunctionExpression);
    }

    bool IsIdentifier() {
      return (code_ & kIdentifierFlag) != 0;
    }

    // Only works corretly if it is actually an identifier expression.
    PreParser::Identifier AsIdentifier() {
      return PreParser::Identifier(
          static_cast<PreParser::Identifier::Type>(code_ >> kIdentifierShift));
    }

    bool IsParenthesized() {
      // If bit 0 or 1 is set, we interpret bit 2 as meaning parenthesized.
      return (code_ & 7) > 4;
    }

    bool IsRawIdentifier() {
      return !IsParenthesized() && IsIdentifier();
    }

    bool IsStringLiteral() { return (code_ & kStringLiteralFlag) != 0; }

    bool IsRawStringLiteral() {
      return !IsParenthesized() && IsStringLiteral();
    }

    bool IsUseStrictLiteral() {
      return (code_ & kStringLiteralMask) == kUseStrictString;
    }

    bool IsThis() {
      return code_ == kThisExpression;
    }

    bool IsThisProperty() {
      return code_ == kThisPropertyExpression;
    }

    bool IsStrictFunction() {
      return code_ == kStrictFunctionExpression;
    }

    Expression Parenthesize() {
      int type = code_ & 3;
      if (type != 0) {
        // Identifiers and string literals can be parenthesized.
        // They no longer work as labels or directive prologues,
        // but are still recognized in other contexts.
        return Expression(code_ | kParentesizedExpressionFlag);
      }
      // For other types of expressions, it's not important to remember
      // the parentheses.
      return *this;
    }

   private:
    // First two/three bits are used as flags.
    // Bit 0 and 1 represent identifiers or strings literals, and are
    // mutually exclusive, but can both be absent.
    // If bit 0 or 1 are set, bit 2 marks that the expression has
    // been wrapped in parentheses (a string literal can no longer
    // be a directive prologue, and an identifier can no longer be
    // a label.
    enum  {
      kUnknownExpression = 0,
      // Identifiers
      kIdentifierFlag = 1,  // Used to detect labels.
      kIdentifierShift = 3,

      kStringLiteralFlag = 2,  // Used to detect directive prologue.
      kUnknownStringLiteral = kStringLiteralFlag,
      kUseStrictString = kStringLiteralFlag | 8,
      kStringLiteralMask = kUseStrictString,

      kParentesizedExpressionFlag = 4,  // Only if identifier or string literal.

      // Below here applies if neither identifier nor string literal.
      kThisExpression = 4,
      kThisPropertyExpression = 8,
      kStrictFunctionExpression = 12
    };

    explicit Expression(int expression_code) : code_(expression_code) { }

    int code_;
  };

  class Statement {
   public:
    static Statement Default() {
      return Statement(kUnknownStatement);
    }

    static Statement FunctionDeclaration() {
      return Statement(kFunctionDeclaration);
    }

    // Creates expression statement from expression.
    // Preserves being an unparenthesized string literal, possibly
    // "use strict".
    static Statement ExpressionStatement(Expression expression) {
      if (!expression.IsParenthesized()) {
        if (expression.IsUseStrictLiteral()) {
          return Statement(kUseStrictExpressionStatement);
        }
        if (expression.IsStringLiteral()) {
          return Statement(kStringLiteralExpressionStatement);
        }
      }
      return Default();
    }

    bool IsStringLiteral() {
      return code_ != kUnknownStatement;
    }

    bool IsUseStrictLiteral() {
      return code_ == kUseStrictExpressionStatement;
    }

    bool IsFunctionDeclaration() {
      return code_ == kFunctionDeclaration;
    }

   private:
    enum Type {
      kUnknownStatement,
      kStringLiteralExpressionStatement,
      kUseStrictExpressionStatement,
      kFunctionDeclaration
    };

    explicit Statement(Type code) : code_(code) {}
    Type code_;
  };

  enum SourceElements {
    kUnknownSourceElements
  };

  typedef int Arguments;

  class Scope {
   public:
    Scope(Scope** variable, ScopeType type)
        : variable_(variable),
          prev_(*variable),
          type_(type),
          materialized_literal_count_(0),
          expected_properties_(0),
          with_nesting_count_(0),
          strict_((prev_ != NULL) && prev_->is_strict()) {
      *variable = this;
    }
    ~Scope() { *variable_ = prev_; }
    void NextMaterializedLiteralIndex() { materialized_literal_count_++; }
    void AddProperty() { expected_properties_++; }
    ScopeType type() { return type_; }
    int expected_properties() { return expected_properties_; }
    int materialized_literal_count() { return materialized_literal_count_; }
    bool IsInsideWith() { return with_nesting_count_ != 0; }
    bool is_strict() { return strict_; }
    void set_strict() { strict_ = true; }
    void EnterWith() { with_nesting_count_++; }
    void LeaveWith() { with_nesting_count_--; }

   private:
    Scope** const variable_;
    Scope* const prev_;
    const ScopeType type_;
    int materialized_literal_count_;
    int expected_properties_;
    int with_nesting_count_;
    bool strict_;
  };

  // Private constructor only used in PreParseProgram.
  PreParser(i::JavaScriptScanner* scanner,
            i::ParserRecorder* log,
            uintptr_t stack_limit,
            bool allow_lazy)
      : scanner_(scanner),
        log_(log),
        scope_(NULL),
        stack_limit_(stack_limit),
        strict_mode_violation_location_(i::Scanner::Location::invalid()),
        strict_mode_violation_type_(NULL),
        stack_overflow_(false),
        allow_lazy_(true),
        parenthesized_function_(false) { }

  // Preparse the program. Only called in PreParseProgram after creating
  // the instance.
  PreParseResult PreParse() {
    Scope top_scope(&scope_, kTopLevelScope);
    bool ok = true;
    int start_position = scanner_->peek_location().beg_pos;
    ParseSourceElements(i::Token::EOS, &ok);
    if (stack_overflow_) return kPreParseStackOverflow;
    if (!ok) {
      ReportUnexpectedToken(scanner_->current_token());
    } else if (scope_->is_strict()) {
      CheckOctalLiteral(start_position, scanner_->location().end_pos, &ok);
    }
    return kPreParseSuccess;
  }

  // Report syntax error
  void ReportUnexpectedToken(i::Token::Value token);
  void ReportMessageAt(int start_pos,
                       int end_pos,
                       const char* type,
                       const char* name_opt) {
    log_->LogMessage(start_pos, end_pos, type, name_opt);
  }

  void CheckOctalLiteral(int beg_pos, int end_pos, bool* ok);

  // All ParseXXX functions take as the last argument an *ok parameter
  // which is set to false if parsing failed; it is unchanged otherwise.
  // By making the 'exception handling' explicit, we are forced to check
  // for failure at the call sites.
  SourceElements ParseSourceElements(int end_token, bool* ok);
  Statement ParseStatement(bool* ok);
  Statement ParseFunctionDeclaration(bool* ok);
  Statement ParseBlock(bool* ok);
  Statement ParseVariableStatement(bool* ok);
  Statement ParseVariableDeclarations(bool accept_IN, int* num_decl, bool* ok);
  Statement ParseExpressionOrLabelledStatement(bool* ok);
  Statement ParseIfStatement(bool* ok);
  Statement ParseContinueStatement(bool* ok);
  Statement ParseBreakStatement(bool* ok);
  Statement ParseReturnStatement(bool* ok);
  Statement ParseWithStatement(bool* ok);
  Statement ParseSwitchStatement(bool* ok);
  Statement ParseDoWhileStatement(bool* ok);
  Statement ParseWhileStatement(bool* ok);
  Statement ParseForStatement(bool* ok);
  Statement ParseThrowStatement(bool* ok);
  Statement ParseTryStatement(bool* ok);
  Statement ParseDebuggerStatement(bool* ok);

  Expression ParseExpression(bool accept_IN, bool* ok);
  Expression ParseAssignmentExpression(bool accept_IN, bool* ok);
  Expression ParseConditionalExpression(bool accept_IN, bool* ok);
  Expression ParseBinaryExpression(int prec, bool accept_IN, bool* ok);
  Expression ParseUnaryExpression(bool* ok);
  Expression ParsePostfixExpression(bool* ok);
  Expression ParseLeftHandSideExpression(bool* ok);
  Expression ParseNewExpression(bool* ok);
  Expression ParseMemberExpression(bool* ok);
  Expression ParseMemberWithNewPrefixesExpression(unsigned new_count, bool* ok);
  Expression ParsePrimaryExpression(bool* ok);
  Expression ParseArrayLiteral(bool* ok);
  Expression ParseObjectLiteral(bool* ok);
  Expression ParseRegExpLiteral(bool seen_equal, bool* ok);
  Expression ParseV8Intrinsic(bool* ok);

  Arguments ParseArguments(bool* ok);
  Expression ParseFunctionLiteral(bool* ok);

  Identifier ParseIdentifier(bool* ok);
  Identifier ParseIdentifierName(bool* ok);
  Identifier ParseIdentifierNameOrGetOrSet(bool* is_get,
                                           bool* is_set,
                                           bool* ok);

  // Logs the currently parsed literal as a symbol in the preparser data.
  void LogSymbol();
  // Log the currently parsed identifier.
  Identifier GetIdentifierSymbol();
  // Log the currently parsed string literal.
  Expression GetStringSymbol();

  i::Token::Value peek() {
    if (stack_overflow_) return i::Token::ILLEGAL;
    return scanner_->peek();
  }

  i::Token::Value Next() {
    if (stack_overflow_) return i::Token::ILLEGAL;
    {
      int marker;
      if (reinterpret_cast<uintptr_t>(&marker) < stack_limit_) {
        // Further calls to peek/Next will return illegal token.
        // The current one will still be returned. It might already
        // have been seen using peek.
        stack_overflow_ = true;
      }
    }
    return scanner_->Next();
  }

  bool peek_any_identifier();

  void set_strict_mode() {
    scope_->set_strict();
  }

  bool strict_mode() { return scope_->is_strict(); }

  void Consume(i::Token::Value token) { Next(); }

  void Expect(i::Token::Value token, bool* ok) {
    if (Next() != token) {
      *ok = false;
    }
  }

  bool Check(i::Token::Value token) {
    i::Token::Value next = peek();
    if (next == token) {
      Consume(next);
      return true;
    }
    return false;
  }
  void ExpectSemicolon(bool* ok);

  static int Precedence(i::Token::Value tok, bool accept_IN);

  void SetStrictModeViolation(i::Scanner::Location,
                              const char* type,
                              bool *ok);

  void CheckDelayedStrictModeViolation(int beg_pos, int end_pos, bool* ok);

  void StrictModeIdentifierViolation(i::Scanner::Location,
                                     const char* eval_args_type,
                                     Identifier identifier,
                                     bool* ok);

  i::JavaScriptScanner* scanner_;
  i::ParserRecorder* log_;
  Scope* scope_;
  uintptr_t stack_limit_;
  i::Scanner::Location strict_mode_violation_location_;
  const char* strict_mode_violation_type_;
  bool stack_overflow_;
  bool allow_lazy_;
  bool parenthesized_function_;
};
} }  // v8::preparser

#endif  // V8_PREPARSER_H
