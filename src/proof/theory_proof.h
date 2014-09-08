/*********************                                                        */
/*! \file theory_proof.h
** \verbatim
** Original author: Liana Hadarean
** Major contributors: Morgan Deters
** Minor contributors (to current version): none
** This file is part of the CVC4 project.
** Copyright (c) 2009-2014  New York University and The University of Iowa
** See the file COPYING in the top-level source directory for licensing
** information.\endverbatim
**
** \brief A manager for UfProofs.
**
** A manager for UfProofs.
**
**
**/


#ifndef __CVC4__THEORY_PROOF_H
#define __CVC4__THEORY_PROOF_H

#include "cvc4_private.h"
#include "util/proof.h"
#include "expr/expr.h"
#include <ext/hash_set>
#include <iostream>

namespace CVC4 {

namespace theory {
class Theory;
}

class TheoryProof; 
typedef int ClauseId;

typedef __gnu_cxx::hash_set<Expr, ExprHashFunction > ExprSet;
typedef std::map<theory::TheoryId, TheoryProof* > TheoryProofTable;

class TheoryProofEngine {
protected:
  ExprSet d_registrationCache;
  TheoryProofTable d_theoryProofTable;

public:
  TheoryProofEngine();
  virtual ~TheoryProofEngine();
  /** 
   * Print the theory term (could be atom) by delegating to the
   * proper theory
   * 
   * @param term 
   * @param os 
   * 
   * @return 
   */
  virtual void printTerm(Expr term, std::ostream& os) = 0;
  /** 
   * Print the proof representation of the given sort.
   * 
   * @param os 
   */
  virtual void printSort(Type type, std::ostream& os) = 0; 
  /** 
   * Print the theory assertions (arbitrary formulas over
   * theory atoms)
   * 
   * @param os 
   * @param paren closing parenthesis
   */
  virtual void printAssertions(std::ostream& os, std::ostream& paren) = 0;
  /** 
   * Print proofs of all the theory lemmas (must prove
   * actual clause used in resolution proof).
   * 
   * @param os 
   * @param paren 
   */
  virtual void printTheoryLemmas(std::ostream& os, std::ostream& paren) = 0;
  /** 
   * Register theory atom (ensures all terms and atoms are declared). 
   * 
   * @param atom 
   */
  void registerTerm(Expr atom);
  /** 
   * Ensures that a theory proof class for the given theory
   * is created.
   * 
   * @param theory 
   */
  void registerTheory(theory::Theory* theory);
  theory::TheoryId getTheoryForLemma(ClauseId id);
  TheoryProof* getTheoryProof(theory::TheoryId id); 
};

class LFSCTheoryProofEngine : public TheoryProofEngine {
public:
  void printDeclarations(std::ostream& os, std::ostream& paren);
  virtual void printCoreTerm(Expr term, std::ostream& os);
  virtual void printTerm(Expr term, std::ostream& os);
  virtual void printAssertions(std::ostream& os, std::ostream& paren);
  virtual void printTheoryLemmas(std::ostream& os, std::ostream& paren);
  virtual void printSort(Type type, std::ostream& os); 
};

class TheoryProof {
protected:
  // Pointer to the theory for this proof
  theory::Theory* d_theory;
  TheoryProofEngine* d_proofEngine;
public:
  TheoryProof(theory::Theory* th, TheoryProofEngine* proofEngine)
    : d_theory(th)
    , d_proofEngine(proofEngine)
  {}
  virtual ~TheoryProof() {}; 
  /** 
   * Print a term belonging to this theory.
   * 
   * @param term expresion representing term
   * @param os output stream
   */
  virtual void printTerm(Expr term, std::ostream& os) = 0;
  /** 
   * Print the proof representation of the given type.
   * 
   * @param type 
   * @param os 
   */
  virtual void printSort(Type type, std::ostream& os) = 0; 
  /** 
   * Print a proof for the theory lemmas. Must prove
   * clause representing lemmas to be used in resolution proof.
   * 
   * @param os output stream
   */
  virtual void printTheoryLemmaProof(std::vector<Expr>& lemma, std::ostream& os, std::ostream& paren) = 0;
  /** 
   * Print the variable/sorts declarations for this theory.
   * 
   * @param os 
   * @param paren 
   */
  virtual void printDeclarations(std::ostream& os, std::ostream& paren) = 0;
  /** 
   * Register a term of this theory that appears in the proof.
   * 
   * @param term 
   */
  virtual void registerTerm(Expr term) = 0; 
};

class BooleanProof : public TheoryProof {
protected:
  ExprSet d_declarations; // all the boolean variables
public:
  BooleanProof(TheoryProofEngine* proofEngine);

  virtual void registerTerm(Expr term);
  
  virtual void printTerm(Expr term, std::ostream& os) = 0;
  virtual void printSort(Type type, std::ostream& os) = 0; 
  virtual void printTheoryLemmaProof(std::vector<Expr>& lemma, std::ostream& os, std::ostream& paren) = 0;
  virtual void printDeclarations(std::ostream& os, std::ostream& paren) = 0;
};

class LFSCBooleanProof : public BooleanProof {
public:
  LFSCBooleanProof(TheoryProofEngine* proofEngine)
    : BooleanProof(proofEngine)
  {}
  virtual void printTerm(Expr term, std::ostream& os);
  virtual void printSort(Type type, std::ostream& os); 
  virtual void printTheoryLemmaProof(std::vector<Expr>& lemma, std::ostream& os, std::ostream& paren) { Unreachable("No boolean lemmas yet!"); }
  virtual void printDeclarations(std::ostream& os, std::ostream& paren);
};


} /* CVC4 namespace */

#endif /* __CVC4__THEORY_PROOF_H */
