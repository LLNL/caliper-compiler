// Copyright (c) 2015, Lawrence Livermore National Security, LLC.  
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of Caliper.
// Written by David Boehme, boehme3@llnl.gov.
// LLNL-CODE-678900
// All rights reserved.
//
// For details, see https://github.com/scalability-llnl/Caliper.
// Please also see the LICENSE file for our additional BSD notice.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the disclaimer below.
//  * Redistributions in binary form must reproduce the above copyright notice, this list of
//    conditions and the disclaimer (as noted below) in the documentation and/or other materials
//    provided with the distribution.
//  * Neither the name of the LLNS/LLNL nor the names of its contributors may be used to endorse
//    or promote products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// LAWRENCE LIVERMORE NATIONAL SECURITY, LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


//------------------------------------------------------------------------------
// Various pieces of Clang infrastructure used by tools in this repo
//
// Highly experimental and undocumented, would be cautious of integrating 
// into other projects
//
// David Poliakoff (poliakoff1@llnl.gov)
//------------------------------------------------------------------------------

#ifndef CLANG_RAJA_INFRASTRUCTURE_LLNL_STATIC_ANALYSIS_UTILITIES_H
#define CLANG_RAJA_INFRASTRUCTURE_LLNL_STATIC_ANALYSIS_UTILITIES_H
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/Support/raw_ostream.h"

#include <regex>
#include <type_traits>
#include <iterator>
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Rewrite/Frontend/Rewriters.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"
using namespace clang;
using clang::ast_type_traits::DynTypedNode;

namespace
{

template <typename T>
const T *identity(const T *in)
{
  return in;
}
template <class castType, class inType, typename func_modifier>
auto operateOnType(const inType input,
                   func_modifier func,
                   bool printOnFail = false) ->
    typename std::result_of<func_modifier(const castType *)>::type
{
  if (const castType *casted_variable = dyn_cast<castType>(input)) {
    return func(casted_variable);
  }
  if
    constexpr(!std::is_void<typename std::result_of<func_modifier(
                  const castType *)>::type>::value)
    {
      return func(NULL);
    }
}
template <class castType, class inType>
const castType *operateOnType(const inType od)
{
  return dyn_cast<castType>(od);
}
auto getAncestor(ASTContext &ctx, DynTypedNode in, int levels) -> DynTypedNode
{
  if (levels == 0) {
    return in;
  } else {
    return getAncestor(ctx, ctx.getParents(in)[0], levels - 1);
  }
}

template <typename T>
auto getAncestor(ASTContext &ctx, T &in, int levels) -> DynTypedNode
{
  return getAncestor(ctx, DynTypedNode::create(in), levels);
}

const ValueDecl *getAssignedDeclOnLHS(const Expr *in)
{
  const ValueDecl *ret = NULL;
  operateOnType<DeclRefExpr>(in, [&](const DeclRefExpr *lhs) {
    ret = lhs->getDecl();
  });
  operateOnType<ArraySubscriptExpr>(in, [&](const ArraySubscriptExpr *lhs) {
    ret = getAssignedDeclOnLHS(lhs->getLHS());
  });
  operateOnType<ImplicitCastExpr>(in, [&](const ImplicitCastExpr *lhs) {
    ret = getAssignedDeclOnLHS(lhs->getSubExpr());
  });
  operateOnType<BinaryOperator>(in, [&](const BinaryOperator *lhs) {
    ret = getAssignedDeclOnLHS(lhs->getLHS());
  });
  operateOnType<MemberExpr>(in, [&](const MemberExpr *lhs) {
    ret = lhs->getMemberDecl();
  });
  return ret;
}
using lhsType = VarDecl;
using assignmentType = std::tuple<const lhsType *, const Expr *>;
const VarDecl *getLHS(assignmentType in) { return std::get<0>(in); }
const Expr *getRHS(assignmentType in) { return std::get<1>(in); }
bool hasValidLHS(assignmentType candidate) { return getLHS(candidate); }
std::string getNameOfCalled(const CallExpr *call)
{
  return call->getDirectCallee()
             ? call->getDirectCallee()->getNameInfo().getAsString()
             : "invalid function";
}
const CallExpr *validateCall(const Expr *candidate, std::regex verifier)
{
  const CallExpr *candidateCall = NULL;
  operateOnType<CallExpr>(candidate, [&](const CallExpr *call) {
    const FunctionDecl *calledFunc = call->getDirectCallee();
    if (calledFunc) {
      if (std::regex_search(calledFunc->getNameInfo().getAsString(),
                            verifier)) {
        candidateCall = call;
      }
    }
  });
  return candidateCall;
}
//TODO: refactor, this only works with single assignment semantics
assignmentType getAssignmentImpl(const DeclStmt *decl)
{
  //for (auto const decl_child : decl->decls()){
  return operateOnType<VarDecl>(
      decl->getSingleDecl(), [=] (const VarDecl *candidate) {
        if (candidate) {
          if (candidate->hasInit()) {
            return std::make_tuple(candidate, candidate->getInit());
          }
          return std::make_tuple(candidate, (const Expr *)NULL);
        }
        return std::make_tuple(candidate, (const Expr *)NULL);
      });
   //}
}
assignmentType getAssignmentImpl(const CXXOperatorCallExpr *call)
{
  const lhsType *finalLHS = NULL;
  auto lhs = call->getArg(0);
  operateOnType<DeclRefExpr>(lhs, [&](const DeclRefExpr *lhsRef) {
    operateOnType<lhsType>(lhsRef->getFoundDecl(),
                           [&](const lhsType *candidate) {
                             finalLHS = candidate;
                           });
  });
  auto rhs = call->getArg(1);
  return std::make_tuple(finalLHS, rhs);
}
template<typename InType>
struct HasIterableChildren{
        template<typename Shadow>
        static constexpr auto check(Shadow*) -> typename std::is_destructible<decltype(std::declval<Shadow>().children())>::type;
        template<typename>
        static constexpr std::false_type check(...);
        typedef decltype(check<InType>(0)) type;
        static constexpr bool value = type::value;

};
assignmentType getAssignmentImpl(const BinaryOperator *op)
{

  const lhsType *decl =
      operateOnType<lhsType>(getAssignedDeclOnLHS(op),
                             [&](const lhsType *decl) { return decl; });
  return std::make_tuple(decl, op->getRHS());
}
assignmentType getAssignment(const Stmt *candidate)
{
  assignmentType test;
  bool validCast = false;
  operateOnType<BinaryOperator>(candidate, [&](const BinaryOperator *in) {
    if (in->isAssignmentOp()) {
      test = getAssignmentImpl(in);
      validCast = true;
    }
  });
  if (!validCast) {
    operateOnType<DeclStmt>(candidate, [&](const DeclStmt *in) {
      test = getAssignmentImpl(in);
      validCast = true;
    });
  }
  if (!validCast) {
    operateOnType<CXXOperatorCallExpr>(candidate,
                                       [&](const CXXOperatorCallExpr *in) {
                                         test = getAssignmentImpl(in);
                                         validCast = true;
                                       });
  }
  return test;
}
template <class T>
struct alwaysTrue {
  bool operator()(const T *in) { return true; }
};


SourceLocation getLocation(const SourceLocation* location){
  return *location;
}
SourceLocation getLocation(const SourceLocation location){
  return location;
}
SourceLocation getLocation(const Stmt* stmt){
  return stmt->getLocStart();
}
template<typename T>
struct child_vec{
  using underlying_iterable_type = std::vector<T>;
  underlying_iterable_type child_iterable;
  child_vec(){
  }
  child_vec(underlying_iterable_type in){
    child_iterable = in;
  }
  void push_back(T back){
    child_iterable.push_back(back);
  }
  T& operator[](const size_t idx){
    return child_iterable[idx];
  }
  decltype(child_iterable.cbegin()) begin() const {
    return child_iterable.cbegin();
  }
  decltype(child_iterable.cend()) end() const {
    return child_iterable.cend();
  }
  child_vec children() const{
    return *this;
  }
  template<typename... Args>
  void insert(Args... args){
    child_iterable.insert(args...);
  }
};

//the intent of deepScan is to do things like pierce if-else's
template <class ScannedType,class Iterable, class Locatable>
child_vec<const ScannedType *> deepScanFuncBefore(
    ASTContext &ctx,
    Iterable scanned_series,
    const Locatable locatable,
    std::function<bool(const ScannedType *)> filter = alwaysTrue<ScannedType>())
{
  clang::BeforeThanCompare<SourceLocation> comparator(ctx.getSourceManager());
  child_vec<const ScannedType *> matches;
  if(scanned_series.end() == scanned_series.begin()){
          return matches;
  }
  for(auto series_element : scanned_series){
    if((!series_element->getLocStart().isValid()) || !comparator(series_element->getLocStart(), getLocation(locatable))){
      continue;
    }
    //series_element->dumpColor();
    operateOnType<ScannedType>(series_element, [&] (const ScannedType* candidate){
      if(filter(candidate)){
        matches.push_back(candidate);
      }
      auto childMatches = deepScanFuncBefore<ScannedType>(ctx, series_element->children(), locatable, filter);
      matches.insert(matches.end(), childMatches.begin(),childMatches.end());
    });
    //TODO TOMORROW: make this work with the outermost std::vector call. Perhaps a simple iterable rather than a vector for the prologue? Perhaps replace it with the whole function and rely on the comparator?
  }
  return matches;
}


template <class ScannedType>
std::vector<const ScannedType *> scanFuncBefore(
    ASTContext &ctx,
    const FunctionDecl *scanned_func,
    const Expr *expr,
    std::function<bool(const ScannedType *)> filter = alwaysTrue<ScannedType>())
{
  clang::BeforeThanCompare<SourceLocation> comparator(ctx.getSourceManager());
  std::vector<const ScannedType *> matches;
  if(!scanned_func){
          return matches;
  }
  return operateOnType<CompoundStmt>(
      scanned_func->getBody(), [&](const CompoundStmt *func_body) {
        for (const Stmt *statement_in_body : func_body->body()) {
          const Expr *asExpr = operateOnType<Expr>(statement_in_body);
          if (asExpr
              && !comparator(asExpr->getLocStart(), expr->getLocStart())) {
            return matches;
          }
          operateOnType<ScannedType>(statement_in_body,
                                     [&](const ScannedType *candidate) {
                                       if (filter(candidate)) {
                                         matches.push_back(candidate);
                                       }
                                     });
        }
        return matches;
      });
}

template <typename OutType>
auto walkUpTo(ASTContext &ctx,
              DynTypedNode in,
              std::function<bool(const OutType *)> filter) -> const OutType *
{
  DynTypedNode parent = getAncestor(ctx, in, 1);
  const Stmt *retExpr = parent.get<Stmt>();
  if (!retExpr) {
    return NULL;
  }
  bool matchesConditions = false;
  operateOnType<OutType>(retExpr, [&](const OutType *candidate) {
    if (candidate && filter(candidate)) {
      matchesConditions = true;
      retExpr = candidate;
    }
  });
  return matchesConditions ? (OutType *)retExpr
                           : walkUpTo<OutType>(ctx, parent, filter);
}

template <typename OutType, typename inType>
auto walkUpTo(ASTContext &ctx,
              inType &in,
              std::function<bool(const OutType *)> filter =
                  alwaysTrue<OutType>()) -> const OutType *
{
  return walkUpTo<OutType>(ctx, DynTypedNode::create(*in), filter);
}

template <typename decl_processor>
void operateOnDecls(const DeclStmt *decl, decl_processor processor)
{
  for (auto declvar : decl->decls()) {
    processor(declvar);
  }
}

template <typename class_type, typename func_modifier>
void annotate_class_methods(const class_type *in_class, func_modifier func)
{
  for (auto method : in_class->methods()) {
    func(method->getDefinition());
  }
}

template <typename func_type, typename func_modifier>
void annotate_class_and_specializations(const func_type *in_func,
                                        func_modifier func)
{
  for (auto specialization : in_func->specializations()) {
    annotate_class_methods(specialization, [&](const FunctionDecl *method) {
      func(method->getDefinition());
    });
  }
  annotate_class_methods(in_func->getTemplatedDecl(),
                         [&](const FunctionDecl *method) {
                           func(method->getDefinition());
                         });
}

template <typename func_type, typename func_modifier>
void annotate_method_and_specializations(const func_type *in_func,
                                         func_modifier func)
{
  for (auto specialization : in_func->specializations()) {
    func(specialization->getDefinition());
  }
  func(in_func->getTemplatedDecl());
}
const CompoundStmt *getFunctionBody(const FunctionDecl *function)
{
  const CompoundStmt *returnedBody = NULL;
  if (function && function->hasBody()) {
    operateOnType<CompoundStmt>(function->getBody(),
                                [&](const CompoundStmt *body) {
                                  returnedBody = body;
                                });
  }
  return returnedBody;
}
std::vector<const Stmt *> explodeFunctionBody(
    const Stmt *possibly_compound_statement,
    int depth)
{
  std::vector<const Stmt *> returnVec;
  if (!possibly_compound_statement) return returnVec;
  bool interesting = false;
  returnVec.push_back(possibly_compound_statement);
  operateOnType<ExprWithCleanups>(
      possibly_compound_statement, [&](const ExprWithCleanups *block) {
        if (depth && block) {
          for (auto &inner_statement :
               explodeFunctionBody(block->getSubExpr(), depth)) {
            returnVec.push_back(inner_statement);
          }
        }
        interesting = true;
      });
  operateOnType<CompoundStmt>(possibly_compound_statement,
                              [&](const CompoundStmt *compound) {
                                for (auto &inner : compound->body()) {
                                  for (auto &exploded :
                                       explodeFunctionBody(inner, depth)) {
                                    returnVec.push_back(exploded);
                                  }
                                }
                                interesting = true;
                              });
  operateOnType<IfStmt>(
      possibly_compound_statement, [&](const IfStmt *conditional) {
        for (auto &inner_statement :
             explodeFunctionBody(conditional->getThen(), depth)) {
          returnVec.push_back(inner_statement);
        }
        for (auto &inner_statement :
             explodeFunctionBody(conditional->getElse(), depth)) {
          returnVec.push_back(inner_statement);
        }

      });
  operateOnType<CallExpr>(possibly_compound_statement,
                          [&](const CallExpr *call) {
                            const FunctionDecl *calledFunc =
                                call->getDirectCallee();
                            const CompoundStmt *bodyAsStmt =
                                getFunctionBody(calledFunc);
                            if (depth && calledFunc && calledFunc->hasBody()) {
                              for (auto &inner_statement :
                                   explodeFunctionBody(bodyAsStmt, depth - 1)) {
                                returnVec.push_back(inner_statement);
                              }
                            }
                            interesting = true;
                          });

  return returnVec;
}

bool isLambda(const FunctionDecl *candidate)
{
  return candidate && candidate->isCXXClassMember()
         && operateOnType<CXXMethodDecl>(
                candidate, [&](const CXXMethodDecl *candidate) {
                  return candidate->getParent()->isLambda();
                });
}
template <typename func_modifier>
void alter_methods(DeclGroupRef DG, func_modifier func)
{
  for (DeclGroupRef::iterator i = DG.begin(), e = DG.end(); i != e; ++i) {
    const Decl *D = *i;
    operateOnType<FunctionDecl>(D, [&](const FunctionDecl *ND) {
      func(ND->getDefinition());
    });
    operateOnType<CXXRecordDecl>(D, [&](const CXXRecordDecl *ND) {
      annotate_class_methods(ND, [&](const FunctionDecl *method) {
        func(method->getDefinition());
      });
    });
    operateOnType<ClassTemplateSpecializationDecl>(
        D, [&](const ClassTemplateSpecializationDecl *ND) {
          annotate_class_methods(ND, [&](const FunctionDecl *method) {
            func(method->getDefinition());
          });
        });
    operateOnType<ClassTemplateDecl>(D, [&](const ClassTemplateDecl *ND) {
      annotate_class_and_specializations(ND, func);
    });
    operateOnType<ClassScopeFunctionSpecializationDecl>(
        D, [&](const ClassScopeFunctionSpecializationDecl *ND) {
        });
    operateOnType<FunctionTemplateDecl>(D, [&](const FunctionTemplateDecl *ND) {
      annotate_method_and_specializations(ND, func);
    });
  }
}
}  // end anonymous namespace
#endif
