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
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice,
//  this list of
//    conditions and the disclaimer below.
//  * Redistributions in binary form must reproduce the above copyright notice,
//  this list of
//    conditions and the disclaimer (as noted below) in the documentation and/or
//    other materials provided with the distribution.
//  * Neither the name of the LLNS/LLNL nor the names of its contributors may be
//  used to endorse
//    or promote products derived from this software without specific prior
//    written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL LAWRENCE LIVERMORE NATIONAL SECURITY, LLC,
// THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

//------------------------------------------------------------------------------
// Clang tool which instruments a file for use with Caliper
//
// David Poliakoff (poliakoff1@llnl.gov)
//------------------------------------------------------------------------------

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Rewrite/Frontend/Rewriters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include "infrastructure/llnl_static_analysis_utilities.h"

using namespace clang;

CompilerInstance TheCompInst;
Rewriter TheRewriter;
std::regex m_regex;
std::vector<std::regex> whitelisted_funcs;
std::vector<std::regex> blacklisted_funcs;
static bool rewrit = false;
// By implementing RecursiveASTVisitor, we can specify which AST nodes
// we're interested in by overriding relevant methods.
class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor>
{
public:
  MyASTVisitor(CompilerInstance* CI) : astContext(&(CI->getASTContext())) {
    TheRewriter.setSourceMgr(astContext->getSourceManager(), astContext->getLangOpts());
  }

  std::string getCaliStartAnnotation(std::string name)
  {
    return std::string(
               "if (cali_function_attr_id == CALI_INV_ID)\n "
               " cali_init();\n "
               " cali_begin_string(cali_function_attr_id, \"")
           + name + std::string("\");\n");
  }
  template <typename handled_type, typename modifier>
  void modify_recursively(const Stmt *modified, modifier mod)
  {
    operateOnType<handled_type>(modified,
                                [&](const handled_type *stm) { mod(stm); });
    if (modified) {
      operateOnType<Stmt>(modified, [&](const Stmt *checked_modified) {
        std::vector<const Stmt *> rev_vec;
        for (auto *child : checked_modified->children()) {
          if (child) {
            rev_vec.push_back(child);
          }
        };
        for (auto *child : rev_vec) {
          modify_recursively<ReturnStmt>(child, mod);
        }
      });
    }
  }

  bool VisitFunctionDecl(FunctionDecl *f)
  {
    // Only function definitions (with bodies), not declarations.
    static FileID moddedFile;
    bool hits = true;
    std::string fname = f->getQualifiedNameAsString();
    if (TheRewriter.getSourceMgr().getFileID(f->getLocStart()) != moddedFile) {
      moddedFile = TheRewriter.getSourceMgr().getFileID(f->getLocStart());
      SourceLocation fileStartLoc =
          TheRewriter.getSourceMgr().translateLineCol(moddedFile, 1, 1);
      TheRewriter.InsertTextBefore(fileStartLoc, "#include<caliper/cali.h>\n");
    }
    hits = true;
    if (hits) {
      if (f->hasBody()) {
        rewrit = true;
        Stmt *FuncBody = f->getBody();

        modify_recursively<ReturnStmt>(FuncBody, [&](const ReturnStmt *ret) {
          SourceLocation locStart = ret->getLocStart();
          TheRewriter.InsertTextBefore(locStart,
                                       " {cali_end(cali_function_attr_id);");
          SourceLocation locEnd = ret->getLocEnd();
          if (ret->getRetValue()) {
            int tokLength =
                Lexer::MeasureTokenLength(ret->getRetValue()->getLocEnd(),
                                          TheRewriter.getSourceMgr(),
                                          TheCompInst.getLangOpts());
            locEnd =
                ret->getRetValue()->getLocEnd().getLocWithOffset(tokLength + 1);
            TheRewriter.InsertTextAfter(locEnd, "}\n");
          } else {
            // locEnd = locEnd.getLocWithOffset(8);
            TheRewriter.InsertTextAfter(locEnd, "}\n");
          }
        });
        auto firstStatementChildVec = FuncBody->children();
        if (firstStatementChildVec.begin() != firstStatementChildVec.end()) {
          auto firstStatement = firstStatementChildVec.begin();
          TheRewriter.InsertTextBefore(
              firstStatement->getSourceRange().getBegin(),
              getCaliStartAnnotation(f->getNameAsString()));
          TheRewriter.InsertTextBefore(f->getBodyRBrace(),
                                       "cali_end(cali_function_attr_id);\n");
        }
        //// Type name as string
        // QualType QT = f->getReturnType();
        // std::string TypeStr = QT.getAsString();

        //// Function name
        // DeclarationName DeclName = f->getNameInfo().getName();
        // std::string FuncName = DeclName.getAsString();

        //// Add comment before
        // std::stringstream SSBefore;
        // SSBefore << "// Begin function " << FuncName << " returning " <<
        // TypeStr
        //         << "\n";
        // SourceLocation ST = f->getSourceRange().getBegin();
        // TheRewriter.InsertText(ST, SSBefore.str(), true, true);

        //// And after
        // std::stringstream SSAfter;
        // SSAfter << "\n// End function " << FuncName;
        // ST = FuncBody->getLocEnd().getLocWithOffset(1);
        // TheRewriter.InsertText(ST, SSAfter.str(), true, true);
      }
    }
    return true;
  }

private:
  ASTContext* astContext;
};

std::set<FileID> containers;

// Implementation of the ASTConsumer interface for reading an AST produced
// by the Clang parser.
class MyASTConsumer : public ASTConsumer
{
public:
  MyASTConsumer(CompilerInstance* CI) : Visitor(CI){}

  // Override the method that gets called for each parsed top-level
  // declaration.
  virtual bool HandleTopLevelDecl(DeclGroupRef DR)
  {
    if (DR.begin() != DR.end()) {
      auto first_decl = *(DR.begin());
      // TheRewriter.InsertTextBefore(first_decl->getLocStart(),
      // "#include<caliper/cali.h>\n");
    }
    for (DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b)
      // Traverse the declaration using our AST visitor.
      Visitor.TraverseDecl(*b);
    return true;
  }

private:
  MyASTVisitor Visitor;
};

class ExampleFrontendAction : public ASTFrontendAction
{
public:
  virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file)
  {
    std::unique_ptr<ASTConsumer> ret(new MyASTConsumer(&CI));  // pass CI pointer to ASTConsumer
    return std::move(ret);  // pass CI pointer to ASTConsumer
  }
};

int main(int argc, const char *argv[])
{
  if (argc < 2) {
    llvm::errs() << "Usage: rewritersample <filename> <regexes>\n";
    return 1;
  }
  llvm::cl::OptionCategory newCategory("Sample", "Sample");

  clang::tooling::CommonOptionsParser op(argc, argv, newCategory);
  clang::tooling::RefactoringTool Tool(op.getCompilations(), op.getSourcePathList());
  if (int Result = Tool.run(clang::tooling::newFrontendActionFactory<ExampleFrontendAction>().get())) {
    return Result;
  }
  // whitelisted_funcs.emplace_back(std::regex(".*",std::regex::optimize));
  // CompilerInstance will hold the instance of the Clang compiler for us,
  // managing the various objects needed to run the compiler.
  // TheCompInst.createDiagnostics();

  // LangOptions &lo = TheCompInst.getLangOpts();
  // lo.CPlusPlus = 1;

  //// Initialize target info with the default triple for our platform.
  // auto TO = std::make_shared<TargetOptions>();
  // TO->Triple = llvm::sys::getDefaultTargetTriple();
  // TargetInfo *TI =
  //    TargetInfo::CreateTargetInfo(TheCompInst.getDiagnostics(), TO);
  // TheCompInst.setTarget(TI);

  // TheCompInst.createFileManager();
  // FileManager &FileMgr = TheCompInst.getFileManager();
  // TheCompInst.createSourceManager(FileMgr);
  // SourceManager &SourceMgr = TheCompInst.getSourceManager();
  // TheCompInst.createPreprocessor(TU_Module);
  // TheCompInst.createASTContext();

  //// A Rewriter helps us manage the code rewriting task.
  // Rewriter TheRewriter;
  // TheRewriter.setSourceMgr(SourceMgr, TheCompInst.getLangOpts());

  //// Set the main file handled by the source manager to the input file.
  // const FileEntry *FileIn = FileMgr.getFile(argv[1]);
  // SourceMgr.setMainFileID(
  //    SourceMgr.createFileID(FileIn, SourceLocation(), SrcMgr::C_User));
  // TheCompInst.getDiagnosticClient().BeginSourceFile(
  //    TheCompInst.getLangOpts(), &TheCompInst.getPreprocessor());

  //// Create an AST consumer instance which is going to get called by
  //// ParseAST.
  // MyASTConsumer TheConsumer(TheRewriter);

  //// Parse the file to AST, registering our consumer as the AST consumer.
  // ParseAST(TheCompInst.getPreprocessor(), &TheConsumer,
  //         TheCompInst.getASTContext());

  //// At this point the rewriter's buffer should be full with the rewritten
  //// file contents.
  const RewriteBuffer *RewriteBuf =
      TheRewriter.getRewriteBufferFor(TheRewriter.getSourceMgr().getMainFileID());
   if(true){
    llvm::outs() << std::string(RewriteBuf->begin(), RewriteBuf->end());
  }
  // return 0;
}
